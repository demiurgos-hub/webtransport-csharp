#include "webtransport_native.h"
#include "wt_datagrams.h"
#include "wt_msquic.h"
#include "wt_http3.h"
#include "wt_qpack.h"
#include "wt_streams.h"

#include <stdlib.h>
#include <string.h>

#define WT_MAX_PENDING_EVENTS 4096u
#define WT_MAX_STREAM_BUFFER_BYTES (1024u * 1024u)
#define WT_MAX_DATAGRAM_QUEUE_COUNT 1024u
#define WT_MAX_DATAGRAM_QUEUE_BYTES (4u * 1024u * 1024u)

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wt_init_lock_once(PINIT_ONCE init_once, PVOID parameter, PVOID* context)
{
    (void)init_once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void wt_lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, wt_init_lock_once, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void wt_unlock(void)
{
    LeaveCriticalSection(&g_lock);
}
#else
#include <pthread.h>
static pthread_mutex_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static void wt_init_lock_once(void)
{
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) == 0) {
        (void)pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
        (void)pthread_mutex_init(&g_lock, &attributes);
        (void)pthread_mutexattr_destroy(&attributes);
    }
}

static void wt_lock(void)
{
    (void)pthread_once(&g_lock_once, wt_init_lock_once);
    (void)pthread_mutex_lock(&g_lock);
}

static void wt_unlock(void)
{
    (void)pthread_mutex_unlock(&g_lock);
}
#endif

typedef enum wt_handle_kind {
    WT_HANDLE_CLIENT = 1,
    WT_HANDLE_SESSION = 2,
    WT_HANDLE_STREAM = 3
} wt_handle_kind;

typedef struct wt_client_context {
    wt_msquic_context* msquic;
    wt_event* pending_events;
    size_t pending_event_offset;
    size_t pending_event_count;
    size_t pending_event_capacity;
    uint8_t closing;
} wt_client_context;

typedef struct wt_datagram_node {
    uint8_t* payload;
    size_t payload_length;
    struct wt_datagram_node* next;
} wt_datagram_node;

typedef struct wt_session_context {
    uint64_t client_id;
    wt_datagram_node* datagram_head;
    wt_datagram_node* datagram_tail;
    size_t datagram_count;
    size_t datagram_bytes;
} wt_session_context;

typedef struct wt_stream_context {
    uint64_t client_id;
    uint64_t session_id;
    void* native_stream;
    uint8_t* buffer;
    size_t buffer_length;
    size_t read_offset;
    uint8_t prefix_sent;
    uint8_t prefix_parsed;
    uint8_t finished;
    uint8_t reset;
} wt_stream_context;

typedef struct wt_handle {
    uint64_t id;
    wt_handle_kind kind;
    void* context;
    struct wt_handle* next;
} wt_handle;

static uint64_t g_next_handle_id = 1;
static wt_handle* g_handles;

static wt_status wt_session_enqueue_datagram(uint64_t session_id, const uint8_t* payload, size_t payload_length);
static wt_status wt_emit_session_closed(uint64_t session_id, wt_status status, uint64_t error_code);
static wt_status wt_accept_peer_stream_from_msquic(void* user_data, void* native_stream, uint8_t bidirectional, uint64_t* stream_id);
static wt_status wt_allocate_stream(uint64_t session_id, uint64_t client_id, uint64_t* stream_id);

static wt_status wt_allocate_handle(wt_handle_kind kind, uint64_t* handle_id)
{
    if (handle_id == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* handle = (wt_handle*)calloc(1, sizeof(wt_handle));
    if (handle == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    handle->id = g_next_handle_id++;
    handle->kind = kind;
    handle->next = g_handles;
    g_handles = handle;
    *handle_id = handle->id;
    return WT_STATUS_OK;
}

static wt_handle* wt_find_handle(uint64_t handle_id, wt_handle_kind kind)
{
    wt_handle* current = g_handles;
    while (current != NULL) {
        if (current->id == handle_id && current->kind == kind) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

static wt_status wt_get_client_id_for_session(uint64_t session_id, uint64_t* client_id)
{
    if (client_id == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_session_context* context = (wt_session_context*)session->context;
    *client_id = context->client_id;
    return WT_STATUS_OK;
}

static wt_status wt_get_msquic_for_client(uint64_t client_id, wt_msquic_context** msquic)
{
    if (msquic == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* client = wt_find_handle(client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_client_context* context = (wt_client_context*)client->context;
    *msquic = context->msquic;
    return WT_STATUS_OK;
}

static wt_status wt_push_event(uint64_t client_id, const wt_event* event)
{
    wt_handle* client = wt_find_handle(client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL || event == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_client_context* context = (wt_client_context*)client->context;
    if (context->closing) {
        return WT_STATUS_CANCELLED;
    }

    if (context->pending_event_count >= WT_MAX_PENDING_EVENTS) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    if (context->pending_event_offset != 0 &&
        context->pending_event_offset + context->pending_event_count == context->pending_event_capacity) {
        memmove(
            &context->pending_events[0],
            &context->pending_events[context->pending_event_offset],
            context->pending_event_count * sizeof(wt_event));
        context->pending_event_offset = 0;
    }

    if (context->pending_event_offset + context->pending_event_count == context->pending_event_capacity) {
        size_t next_capacity = context->pending_event_capacity == 0 ? 32 : context->pending_event_capacity * 2;
        if (next_capacity > WT_MAX_PENDING_EVENTS) {
            next_capacity = WT_MAX_PENDING_EVENTS;
        }

        wt_event* next_events = (wt_event*)realloc(context->pending_events, next_capacity * sizeof(wt_event));
        if (next_events == NULL) {
            return WT_STATUS_OUT_OF_MEMORY;
        }

        context->pending_events = next_events;
        context->pending_event_capacity = next_capacity;
    }

    context->pending_events[context->pending_event_offset + context->pending_event_count] = *event;
    context->pending_event_count++;
    return WT_STATUS_OK;
}

static wt_status wt_push_event_from_msquic(void* user_data, const wt_event* event)
{
    uint64_t client_id = (uint64_t)(uintptr_t)user_data;
    wt_lock();
    wt_status status = wt_push_event(client_id, event);
    wt_unlock();
    return status;
}

static wt_status wt_append_stream_data_from_msquic(void* user_data, uint64_t stream_id, const uint8_t* data, size_t length)
{
    (void)user_data;

    wt_lock();

    if (data == NULL && length != 0) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    if (stream == NULL || stream->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (length != 0) {
        wt_stream_context* context = (wt_stream_context*)stream->context;
        if (context->read_offset != 0) {
            size_t available = context->buffer_length - context->read_offset;
            memmove(context->buffer, context->buffer + context->read_offset, available);
            context->buffer_length = available;
            context->read_offset = 0;
        }

        if (length > WT_MAX_STREAM_BUFFER_BYTES || context->buffer_length > WT_MAX_STREAM_BUFFER_BYTES - length) {
            wt_unlock();
            return WT_STATUS_OUT_OF_MEMORY;
        }

        uint8_t* next = (uint8_t*)realloc(context->buffer, context->buffer_length + length);
        if (next == NULL) {
            wt_unlock();
            return WT_STATUS_OUT_OF_MEMORY;
        }

        context->buffer = next;
        memcpy(context->buffer + context->buffer_length, data, length);
        context->buffer_length += length;
    }

    wt_event event;
    memset(&event, 0, sizeof(event));
    event.type = WT_EVENT_STREAM_DATA_RECEIVED;
    event.status = WT_STATUS_OK;
    event.stream_id = stream_id;

    uint64_t client_id = 0;
    wt_stream_context* context = (wt_stream_context*)stream->context;
    event.session_id = context->session_id;
    wt_status status = wt_get_client_id_for_session(context->session_id, &client_id);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    status = wt_push_event(client_id, &event);
    wt_unlock();
    return status;
}

static wt_status wt_enqueue_datagram_from_msquic(void* user_data, uint64_t session_id, const uint8_t* payload, size_t payload_length)
{
    (void)user_data;

    wt_lock();

    wt_status status = wt_session_enqueue_datagram(session_id, payload, payload_length);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    wt_event event;
    memset(&event, 0, sizeof(event));
    event.type = WT_EVENT_DATAGRAM_RECEIVED;
    event.status = WT_STATUS_OK;
    event.session_id = session_id;

    uint64_t client_id = 0;
    status = wt_get_client_id_for_session(session_id, &client_id);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    status = wt_push_event(client_id, &event);
    wt_unlock();
    return status;
}

static wt_status wt_accept_peer_stream_from_msquic(void* user_data, void* native_stream, uint8_t bidirectional, uint64_t* stream_id)
{
    wt_client_context* client_context = (wt_client_context*)user_data;
    if (client_context == NULL || stream_id == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    // NOTE: this implementation supports a single WebTransport session per
    // client, so a peer-initiated stream is attached to that client's session.
    // Multi-session support would require parsing the WebTransport stream
    // prefix to map the stream to its originating session.
    wt_lock();
    uint64_t session_id = 0;
    wt_handle* current = g_handles;
    uint64_t client_id = 0;
    while (current != NULL) {
        if (current->kind == WT_HANDLE_CLIENT && current->context == client_context) {
            client_id = current->id;
            break;
        }

        current = current->next;
    }

    if (client_id == 0) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    current = g_handles;
    while (current != NULL) {
        if (current->kind == WT_HANDLE_SESSION && current->context != NULL) {
            wt_session_context* session_context = (wt_session_context*)current->context;
            if (session_context->client_id == client_id) {
                session_id = current->id;
                break;
            }
        }

        current = current->next;
    }

    if (session_id == 0) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_status status = wt_allocate_stream(session_id, client_id, stream_id);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    wt_handle* stream = wt_find_handle(*stream_id, WT_HANDLE_STREAM);
    wt_stream_context* stream_context = stream == NULL ? NULL : (wt_stream_context*)stream->context;
    if (stream_context != NULL) {
        stream_context->native_stream = native_stream;
        stream_context->prefix_parsed = 1;
    }

    wt_event event;
    memset(&event, 0, sizeof(event));
    event.type = bidirectional ? WT_EVENT_BIDI_STREAM_OPENED : WT_EVENT_UNI_STREAM_OPENED;
    event.status = WT_STATUS_OK;
    event.client_id = client_id;
    event.session_id = session_id;
    event.stream_id = *stream_id;
    status = wt_push_event(client_id, &event);
    wt_unlock();
    return status;
}

static wt_status wt_emit_session_closed(uint64_t session_id, wt_status status, uint64_t error_code)
{
    uint64_t client_id = 0;
    wt_status client_status = wt_get_client_id_for_session(session_id, &client_id);
    if (client_status != WT_STATUS_OK) {
        return client_status;
    }

    wt_event event;
    memset(&event, 0, sizeof(event));
    event.type = WT_EVENT_SESSION_CLOSED;
    event.status = status;
    event.client_id = client_id;
    event.session_id = session_id;
    event.error_code = error_code;
    return wt_push_event(client_id, &event);
}

static wt_status wt_allocate_session(uint64_t client_id, uint64_t* session_id)
{
    wt_session_context* session_context = (wt_session_context*)calloc(1, sizeof(wt_session_context));
    if (session_context == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    session_context->client_id = client_id;

    wt_status status = wt_allocate_handle(WT_HANDLE_SESSION, session_id);
    if (status != WT_STATUS_OK) {
        free(session_context);
        return status;
    }

    wt_find_handle(*session_id, WT_HANDLE_SESSION)->context = session_context;
    return WT_STATUS_OK;
}

static wt_status wt_allocate_stream(uint64_t session_id, uint64_t client_id, uint64_t* stream_id)
{
    wt_stream_context* stream_context = (wt_stream_context*)calloc(1, sizeof(wt_stream_context));
    if (stream_context == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    stream_context->client_id = client_id;
    stream_context->session_id = session_id;

    wt_status status = wt_allocate_handle(WT_HANDLE_STREAM, stream_id);
    if (status != WT_STATUS_OK) {
        free(stream_context);
        return status;
    }

    wt_find_handle(*stream_id, WT_HANDLE_STREAM)->context = stream_context;
    return WT_STATUS_OK;
}

static wt_status wt_session_enqueue_datagram(uint64_t session_id, const uint8_t* payload, size_t payload_length)
{
    if (payload == NULL && payload_length != 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_session_context* context = (wt_session_context*)session->context;
    if (payload_length > WT_MAX_DATAGRAM_QUEUE_BYTES ||
        context->datagram_count >= WT_MAX_DATAGRAM_QUEUE_COUNT ||
        context->datagram_bytes > WT_MAX_DATAGRAM_QUEUE_BYTES - payload_length) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    wt_datagram_node* node = (wt_datagram_node*)calloc(1, sizeof(wt_datagram_node));
    if (node == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    if (payload_length != 0) {
        node->payload = (uint8_t*)malloc(payload_length);
        if (node->payload == NULL) {
            free(node);
            return WT_STATUS_OUT_OF_MEMORY;
        }

        memcpy(node->payload, payload, payload_length);
        node->payload_length = payload_length;
    }

    if (context->datagram_tail == NULL) {
        context->datagram_head = node;
        context->datagram_tail = node;
    } else {
        context->datagram_tail->next = node;
        context->datagram_tail = node;
    }

    context->datagram_count++;
    context->datagram_bytes += payload_length;
    return WT_STATUS_OK;
}

static void wt_session_free_datagrams(wt_session_context* context)
{
    if (context == NULL) {
        return;
    }

    wt_datagram_node* current = context->datagram_head;
    while (current != NULL) {
        wt_datagram_node* next = current->next;
        free(current->payload);
        free(current);
        current = next;
    }

    context->datagram_head = NULL;
    context->datagram_tail = NULL;
    context->datagram_count = 0;
    context->datagram_bytes = 0;
}

static void wt_free_session_context(wt_session_context* context)
{
    wt_session_free_datagrams(context);
    free(context);
}

static void wt_free_stream_context(wt_stream_context* context)
{
    if (context == NULL) {
        return;
    }

    free(context->buffer);
    free(context);
}

static void wt_remove_child_handles_for_client(uint64_t client_id)
{
    wt_handle** current = &g_handles;
    while (*current != NULL) {
        wt_handle* handle = *current;
        uint8_t remove = 0;

        if (handle->kind == WT_HANDLE_SESSION && handle->context != NULL) {
            wt_session_context* session_context = (wt_session_context*)handle->context;
            remove = session_context->client_id == client_id;
        } else if (handle->kind == WT_HANDLE_STREAM && handle->context != NULL) {
            wt_stream_context* stream_context = (wt_stream_context*)handle->context;
            remove = stream_context->client_id == client_id;
        }

        if (!remove) {
            current = &handle->next;
            continue;
        }

        *current = handle->next;
        if (handle->kind == WT_HANDLE_SESSION) {
            wt_free_session_context((wt_session_context*)handle->context);
        } else if (handle->kind == WT_HANDLE_STREAM) {
            wt_free_stream_context((wt_stream_context*)handle->context);
        }

        free(handle);
    }
}

WT_API uint32_t wt_get_abi_version(void)
{
    return WT_ABI_VERSION;
}

WT_API wt_status wt_client_create(uint64_t* client_id)
{
    if (client_id == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_lock();

    wt_msquic_context* context = NULL;
    wt_status status = wt_msquic_context_create(&context);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    wt_client_context* client_context = (wt_client_context*)calloc(1, sizeof(wt_client_context));
    if (client_context == NULL) {
        wt_unlock();
        wt_msquic_context_destroy(context);
        return WT_STATUS_OUT_OF_MEMORY;
    }

    client_context->msquic = context;

    status = wt_allocate_handle(WT_HANDLE_CLIENT, client_id);
    if (status != WT_STATUS_OK) {
        free(client_context);
        wt_unlock();
        wt_msquic_context_destroy(context);
        return status;
    }

    wt_find_handle(*client_id, WT_HANDLE_CLIENT)->context = client_context;
    wt_msquic_set_event_sink(
        context,
        *client_id,
        wt_push_event_from_msquic,
        (void*)(uintptr_t)(*client_id));
    wt_msquic_set_stream_data_sink(
        context,
        wt_append_stream_data_from_msquic,
        client_context);
    wt_msquic_set_datagram_sink(
        context,
        wt_enqueue_datagram_from_msquic,
        client_context);
    wt_msquic_set_peer_stream_sink(
        context,
        wt_accept_peer_stream_from_msquic,
        client_context);
    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_client_connect(uint64_t client_id, const wt_connect_options* options, uint64_t* operation_id)
{
    wt_lock();

    wt_handle* client = wt_find_handle(client_id, WT_HANDLE_CLIENT);
    if (client == NULL || options == NULL || operation_id == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (options->url_utf8 == NULL || options->url_length == 0) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_client_context* client_context = (wt_client_context*)client->context;
    uint64_t session_id = 0;
    wt_status status = wt_allocate_session(client_id, &session_id);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    wt_msquic_set_session_id(client_context->msquic, session_id);
    wt_unlock();

    status = wt_msquic_connect(client_context->msquic, options);
    if (status != WT_STATUS_OK) {
        wt_lock();
        wt_release(session_id);
        wt_unlock();
        return status;
    }

    *operation_id = session_id;
    return WT_STATUS_OK;
}

WT_API wt_status wt_client_shutdown(uint64_t client_id, uint64_t error_code)
{
    wt_lock();

    wt_handle* client = wt_find_handle(client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_client_context* context = (wt_client_context*)client->context;
    context->closing = 1;
    wt_msquic_context* msquic = context->msquic;
    wt_unlock();

    wt_msquic_shutdown(msquic, error_code);
    return WT_STATUS_OK;
}

WT_API wt_status wt_poll_event(uint64_t client_id, wt_event* event)
{
    wt_lock();

    wt_handle* client = wt_find_handle(client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL || event == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_client_context* context = (wt_client_context*)client->context;
    if (context->pending_event_count == 0) {
        memset(event, 0, sizeof(*event));
        wt_unlock();
        return WT_STATUS_NOT_FOUND;
    }

    *event = context->pending_events[context->pending_event_offset];
    context->pending_event_offset++;
    context->pending_event_count--;
    if (context->pending_event_count == 0) {
        context->pending_event_offset = 0;
    }

    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_session_open_bidi_stream(uint64_t session_id, uint64_t* operation_id)
{
    wt_lock();

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL || operation_id == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint64_t stream_id = 0;
    wt_session_context* session_context = (wt_session_context*)session->context;
    wt_handle* client = wt_find_handle(session_context->client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_client_context* client_context = (wt_client_context*)client->context;
    wt_status status = wt_allocate_stream(session_id, session_context->client_id, &stream_id);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    wt_unlock();

    status = wt_msquic_open_bidi_stream(client_context->msquic, session_id, stream_id);
    if (status != WT_STATUS_OK) {
        wt_lock();
        wt_release(stream_id);
        wt_unlock();
        return status;
    }

    *operation_id = stream_id;
    return WT_STATUS_OK;
}

WT_API wt_status wt_session_open_uni_stream(uint64_t session_id, uint64_t* operation_id)
{
    wt_lock();

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL || operation_id == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_status status = wt_streams_open_uni(session_id, operation_id);
    if (status != WT_STATUS_OK) {
        wt_unlock();
        return status;
    }

    wt_session_context* session_context = (wt_session_context*)session->context;
    status = wt_allocate_stream(session_id, session_context->client_id, operation_id);
    wt_unlock();
    return status;
}

WT_API wt_status wt_session_send_datagram(uint64_t session_id, const uint8_t* payload, size_t payload_length, uint64_t* operation_id)
{
    wt_lock();

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || operation_id == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (payload == NULL && payload_length != 0) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_session_context* session_context = (wt_session_context*)session->context;
    wt_handle* client = wt_find_handle(session_context->client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_client_context* client_context = (wt_client_context*)client->context;
    wt_unlock();

    wt_status status = wt_msquic_datagram_send(client_context->msquic, session_id, payload, payload_length);
    if (status != WT_STATUS_OK) {
        return status;
    }

    *operation_id = session_id;
    return WT_STATUS_OK;
}

WT_API wt_status wt_session_receive_datagram(uint64_t session_id, uint8_t* buffer, size_t buffer_length, size_t* bytes_read)
{
    wt_lock();

    if (buffer == NULL || bytes_read == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_session_context* context = (wt_session_context*)session->context;
    wt_datagram_node* node = context->datagram_head;
    if (node == NULL) {
        *bytes_read = 0;
        wt_unlock();
        return WT_STATUS_NOT_FOUND;
    }

    if (buffer_length < node->payload_length) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (node->payload_length != 0) {
        memcpy(buffer, node->payload, node->payload_length);
    }

    *bytes_read = node->payload_length;
    context->datagram_head = node->next;
    if (context->datagram_head == NULL) {
        context->datagram_tail = NULL;
    }
    if (context->datagram_count != 0) {
        context->datagram_count--;
    }
    if (context->datagram_bytes >= node->payload_length) {
        context->datagram_bytes -= node->payload_length;
    } else {
        context->datagram_bytes = 0;
    }

    free(node->payload);
    free(node);
    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_stream_read(uint64_t stream_id, uint8_t* buffer, size_t buffer_length, size_t* bytes_read)
{
    wt_lock();

    if (wt_find_handle(stream_id, WT_HANDLE_STREAM) == NULL || bytes_read == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (buffer == NULL && buffer_length != 0) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_handle* stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    wt_stream_context* context = (wt_stream_context*)stream->context;
    if (context == NULL || context->read_offset >= context->buffer_length) {
        *bytes_read = 0;
        wt_unlock();
        return WT_STATUS_NOT_FOUND;
    }

    size_t available = context->buffer_length - context->read_offset;
    size_t to_copy = available < buffer_length ? available : buffer_length;
    if (to_copy > 0) {
        memcpy(buffer, context->buffer + context->read_offset, to_copy);
        context->read_offset += to_copy;
    }

    *bytes_read = to_copy;
    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_stream_write(uint64_t stream_id, const uint8_t* payload, size_t payload_length, uint8_t end_stream, uint64_t* operation_id)
{
    wt_lock();

    wt_handle* stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    if (stream == NULL || operation_id == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (payload == NULL && payload_length != 0) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_stream_context* context = (wt_stream_context*)stream->context;
    if (context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_handle* session = wt_find_handle(context->session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_session_context* session_context = (wt_session_context*)session->context;
    wt_handle* client = wt_find_handle(session_context->client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_client_context* client_context = (wt_client_context*)client->context;
    wt_unlock();

    wt_status status = wt_msquic_stream_write(client_context->msquic, stream_id, payload, payload_length, end_stream);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_lock();
    stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    context = stream == NULL ? NULL : (wt_stream_context*)stream->context;
    if (context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    context->finished = end_stream ? 1 : context->finished;
    *operation_id = stream_id;
    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_stream_finish(uint64_t stream_id, uint64_t* operation_id)
{
    wt_lock();

    wt_handle* stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    if (stream == NULL || operation_id == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_stream_context* context = (wt_stream_context*)stream->context;
    if (context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_handle* session = wt_find_handle(context->session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_session_context* session_context = (wt_session_context*)session->context;
    wt_handle* client = wt_find_handle(session_context->client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_client_context* client_context = (wt_client_context*)client->context;
    wt_unlock();

    wt_status status = wt_msquic_stream_finish(client_context->msquic, stream_id);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_lock();
    stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    context = stream == NULL ? NULL : (wt_stream_context*)stream->context;
    if (context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    context->finished = 1;
    *operation_id = stream_id;
    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_stream_reset(uint64_t stream_id, uint64_t error_code)
{
    wt_lock();

    wt_handle* stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    if (stream == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_stream_context* context = (wt_stream_context*)stream->context;
    if (context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_handle* session = wt_find_handle(context->session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_session_context* session_context = (wt_session_context*)session->context;
    wt_handle* client = wt_find_handle(session_context->client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    wt_client_context* client_context = (wt_client_context*)client->context;
    wt_unlock();

    wt_status status = wt_msquic_stream_reset(client_context->msquic, stream_id, error_code);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_lock();
    stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    context = stream == NULL ? NULL : (wt_stream_context*)stream->context;
    if (context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_STATE;
    }

    context->reset = 1;
    wt_unlock();
    return WT_STATUS_OK;
}

WT_API wt_status wt_session_close(uint64_t session_id, uint64_t error_code, const uint8_t* reason_utf8, size_t reason_length)
{
    wt_lock();

    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    if (session == NULL || session->context == NULL) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (reason_utf8 == NULL && reason_length != 0) {
        wt_unlock();
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_session_context* session_context = (wt_session_context*)session->context;
    wt_msquic_context* msquic = NULL;
    wt_status status = wt_get_msquic_for_client(session_context->client_id, &msquic);
    if (status == WT_STATUS_OK) {
        status = wt_emit_session_closed(session_id, WT_STATUS_OK, error_code);
    }
    wt_unlock();

    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_msquic_shutdown(msquic, error_code);
    return WT_STATUS_OK;
}

WT_API void wt_release(uint64_t handle_id)
{
    wt_handle_kind removed_kind = 0;
    void* removed_context = NULL;
    wt_msquic_context* removed_msquic = NULL;
    uint64_t removed_stream_id = 0;

    wt_lock();

    wt_handle** current = &g_handles;
    while (*current != NULL) {
        if ((*current)->id == handle_id) {
            wt_handle* removed = *current;
            *current = removed->next;
            removed_kind = removed->kind;
            removed_context = removed->context;
            if (removed_kind == WT_HANDLE_CLIENT && removed_context != NULL) {
                wt_client_context* client_context = (wt_client_context*)removed_context;
                client_context->closing = 1;
                removed_msquic = client_context->msquic;
                wt_remove_child_handles_for_client(removed->id);
            } else if (removed_kind == WT_HANDLE_STREAM && removed_context != NULL) {
                wt_stream_context* stream_context = (wt_stream_context*)removed_context;
                removed_stream_id = removed->id;
                wt_get_msquic_for_client(stream_context->client_id, &removed_msquic);
            }
            free(removed);
            wt_unlock();
            break;
        }

        current = &(*current)->next;
    }

    if (removed_context == NULL) {
        if (removed_kind == 0) {
            wt_unlock();
        }
        return;
    }

    if (removed_kind == WT_HANDLE_CLIENT) {
        wt_client_context* context = (wt_client_context*)removed_context;
        // Detach the MsQuic event/stream/datagram sinks (which point at this
        // client context) and begin connection shutdown synchronously. This is
        // non-blocking and guarantees no in-flight MsQuic worker callback can
        // touch the client context after it is freed below.
        wt_msquic_shutdown(removed_msquic, 0);
        free(context->pending_events);
        free(context);
        // The blocking MsQuic handle close (ConnectionClose/RegistrationClose/
        // MsQuicClose) is performed on a detached thread so callers never block
        // on worker-thread drain, which otherwise deadlocks process teardown.
        wt_msquic_context_destroy_detached(removed_msquic);
    } else if (removed_kind == WT_HANDLE_SESSION) {
        wt_free_session_context((wt_session_context*)removed_context);
    } else if (removed_kind == WT_HANDLE_STREAM) {
        wt_msquic_close_stream_handle(removed_msquic, removed_stream_id);
        wt_free_stream_context((wt_stream_context*)removed_context);
    }
}

#if defined(WT_ENABLE_TEST_HOOKS)
WT_API wt_status wt_test_http3_settings_roundtrip(void)
{
    wt_buffer buffer;
    wt_buffer_init(&buffer);
    wt_status status = wt_http3_encode_client_settings(1, &buffer);
    if (status != WT_STATUS_OK) {
        wt_buffer_free(&buffer);
        return status;
    }

    wt_http3_peer_settings settings;
    status = wt_http3_parse_settings(buffer.data, buffer.length, &settings);
    if (status == WT_STATUS_OK) {
        status = wt_http3_validate_peer_settings(&settings);
    }

    wt_buffer_free(&buffer);
    return status;
}

WT_API wt_status wt_test_qpack_status_decode(uint16_t* status_code)
{
    if (status_code == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    static const uint8_t encoded[] = {
        0x00,
        0x00,
        0x27,
        0x00,
        ':',
        's',
        't',
        'a',
        't',
        'u',
        's',
        0x03,
        '2',
        '0',
        '0'
    };

    return wt_qpack_decode_status(encoded, sizeof(encoded), status_code);
}

WT_API wt_status wt_test_webtransport_stream_prefix(uint64_t session_id, uint8_t* output, size_t output_length, size_t* bytes_written)
{
    if (output == NULL || bytes_written == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_buffer prefix;
    wt_buffer_init(&prefix);
    wt_status status = wt_http3_encode_varint(WT_H3_WEBTRANSPORT_STREAM_FRAME_TYPE, &prefix);
    if (status == WT_STATUS_OK) {
        status = wt_http3_encode_varint(session_id, &prefix);
    }

    if (status != WT_STATUS_OK) {
        wt_buffer_free(&prefix);
        return status;
    }

    if (prefix.length > output_length) {
        wt_buffer_free(&prefix);
        return WT_STATUS_INVALID_ARGUMENT;
    }

    memcpy(output, prefix.data, prefix.length);
    *bytes_written = prefix.length;
    wt_buffer_free(&prefix);
    return WT_STATUS_OK;
}

WT_API wt_status wt_test_connect_request_includes_headers(void)
{
    static const uint8_t authority[] = "example.com";
    static const uint8_t path[] = "/transport";
    static const uint8_t headers[] = "x-test: abc\n";
    static const uint8_t expected_name[] = "x-test";
    static const uint8_t expected_value[] = "abc";

    wt_http3_session_request request;
    memset(&request, 0, sizeof(request));
    request.authority = authority;
    request.authority_length = sizeof(authority) - 1;
    request.path = path;
    request.path_length = sizeof(path) - 1;
    request.headers = headers;
    request.headers_length = sizeof(headers) - 1;

    wt_buffer output;
    wt_buffer_init(&output);
    wt_status status = wt_http3_encode_webtransport_connect(&request, &output);
    if (status != WT_STATUS_OK) {
        wt_buffer_free(&output);
        return status;
    }

    int found_name = 0;
    int found_value = 0;
    for (size_t i = 0; i + sizeof(expected_name) - 1 <= output.length; i++) {
        if (memcmp(output.data + i, expected_name, sizeof(expected_name) - 1) == 0) {
            found_name = 1;
            break;
        }
    }

    for (size_t i = 0; i + sizeof(expected_value) - 1 <= output.length; i++) {
        if (memcmp(output.data + i, expected_value, sizeof(expected_value) - 1) == 0) {
            found_value = 1;
            break;
        }
    }

    wt_buffer_free(&output);
    return found_name && found_value ? WT_STATUS_OK : WT_STATUS_NOT_FOUND;
}

WT_API wt_status wt_test_datagram_roundtrip(uint64_t session_id, const uint8_t* payload, size_t payload_length, uint64_t* decoded_session_id, uint8_t* output, size_t output_length, size_t* bytes_written)
{
    if (decoded_session_id == NULL || output == NULL || bytes_written == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_buffer encoded;
    wt_buffer_init(&encoded);
    wt_status status = wt_datagrams_encode(session_id, payload, payload_length, &encoded);
    if (status != WT_STATUS_OK) {
        wt_buffer_free(&encoded);
        return status;
    }

    const uint8_t* decoded_payload = NULL;
    size_t decoded_payload_length = 0;
    status = wt_datagrams_decode(encoded.data, encoded.length, decoded_session_id, &decoded_payload, &decoded_payload_length);
    if (status == WT_STATUS_OK) {
        if (decoded_payload_length > output_length) {
            status = WT_STATUS_INVALID_ARGUMENT;
        } else {
            memcpy(output, decoded_payload, decoded_payload_length);
            *bytes_written = decoded_payload_length;
        }
    }

    wt_buffer_free(&encoded);
    return status;
}

WT_API wt_status wt_test_datagram_wire_local_mapping(
    const uint8_t* payload,
    size_t payload_length,
    uint64_t* local_session_id,
    uint64_t* wire_session_id,
    uint64_t* encoded_session_id,
    uint8_t* output,
    size_t output_length,
    size_t* bytes_written)
{
    if ((payload == NULL && payload_length != 0) ||
        local_session_id == NULL ||
        wire_session_id == NULL ||
        encoded_session_id == NULL ||
        output == NULL ||
        bytes_written == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (payload_length > (size_t)-1 - 8) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *local_session_id = 0;
    *wire_session_id = 0;
    *encoded_session_id = 0;
    *bytes_written = 0;

    uint8_t* encoded = (uint8_t*)malloc(payload_length + 8);
    if (encoded == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    uint64_t client_id = 0;
    wt_status status = wt_client_create(&client_id);
    if (status != WT_STATUS_OK) {
        free(encoded);
        return status;
    }

    wt_client_context* client_context = NULL;
    wt_lock();
    wt_handle* client = wt_find_handle(client_id, WT_HANDLE_CLIENT);
    if (client == NULL || client->context == NULL) {
        status = WT_STATUS_INVALID_STATE;
    } else {
        client_context = (wt_client_context*)client->context;
        status = wt_allocate_session(client_id, local_session_id);
    }

    if (status == WT_STATUS_OK) {
        *wire_session_id = (*local_session_id + 1000) * 4;
        wt_msquic_set_session_id(client_context->msquic, *local_session_id);
        wt_msquic_test_set_wire_session_id(client_context->msquic, *wire_session_id);
    }
    wt_unlock();

    size_t encoded_length = 0;
    if (status == WT_STATUS_OK) {
        status = wt_msquic_test_encode_datagram(
            client_context->msquic,
            *local_session_id,
            payload,
            payload_length,
            encoded,
            payload_length + 8,
            &encoded_length);
    }

    if (status == WT_STATUS_OK) {
        const uint8_t* decoded_payload = NULL;
        size_t decoded_payload_length = 0;
        status = wt_datagrams_decode(
            encoded,
            encoded_length,
            encoded_session_id,
            &decoded_payload,
            &decoded_payload_length);
        if (status == WT_STATUS_OK && *encoded_session_id != *wire_session_id / 4) {
            status = WT_STATUS_PROTOCOL_ERROR;
        }
    }

    if (status == WT_STATUS_OK) {
        status = wt_msquic_test_deliver_datagram(client_context->msquic, encoded, encoded_length);
    }

    if (status == WT_STATUS_OK) {
        status = wt_session_receive_datagram(*local_session_id, output, output_length, bytes_written);
    }

    if (*local_session_id != 0) {
        wt_release(*local_session_id);
    }

    wt_release(client_id);
    free(encoded);
    return status;
}

WT_API wt_status wt_test_datagram_allows_zero_flow_id(
    const uint8_t* payload,
    size_t payload_length,
    uint64_t* encoded_session_id,
    uint8_t* output,
    size_t output_length,
    size_t* bytes_written)
{
    if ((payload == NULL && payload_length != 0) ||
        encoded_session_id == NULL ||
        output == NULL ||
        bytes_written == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *encoded_session_id = 0;
    *bytes_written = 0;

    uint8_t* encoded = (uint8_t*)malloc(payload_length + 8);
    if (encoded == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    wt_msquic_context* context = NULL;
    wt_status status = wt_msquic_context_create(&context);
    if (status == WT_STATUS_OK) {
        wt_msquic_set_session_id(context, 1);
        wt_msquic_test_set_connect_stream_id(context, 0);
    }

    size_t encoded_length = 0;
    if (status == WT_STATUS_OK) {
        status = wt_msquic_test_encode_datagram(
            context,
            1,
            payload,
            payload_length,
            encoded,
            payload_length + 8,
            &encoded_length);
    }

    const uint8_t* decoded_payload = NULL;
    size_t decoded_payload_length = 0;
    if (status == WT_STATUS_OK) {
        status = wt_datagrams_decode(
            encoded,
            encoded_length,
            encoded_session_id,
            &decoded_payload,
            &decoded_payload_length);
    }

    if (status == WT_STATUS_OK) {
        if (decoded_payload_length > output_length) {
            status = WT_STATUS_INVALID_ARGUMENT;
        } else {
            memcpy(output, decoded_payload, decoded_payload_length);
            *bytes_written = decoded_payload_length;
        }
    }

    wt_msquic_context_destroy(context);
    free(encoded);
    return status;
}

static wt_status wt_test_event_counter(void* user_data, const wt_event* event)
{
    uint32_t* count = (uint32_t*)user_data;
    if (count == NULL || event == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (event->type == WT_EVENT_SESSION_CONNECTED) {
        (*count)++;
    }

    return WT_STATUS_OK;
}

WT_API wt_status wt_test_session_connected_requires_acceptance_and_stream_id(
    uint32_t* accepted_before_stream_count,
    uint32_t* stream_before_accepted_count,
    uint32_t* both_ready_count)
{
    if (accepted_before_stream_count == NULL ||
        stream_before_accepted_count == NULL ||
        both_ready_count == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *accepted_before_stream_count = 0;
    *stream_before_accepted_count = 0;
    *both_ready_count = 0;

    wt_msquic_context* context = NULL;
    wt_status status = wt_msquic_context_create(&context);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_msquic_set_session_id(context, 1);
    wt_msquic_set_event_sink(context, 1, wt_test_event_counter, accepted_before_stream_count);
    wt_msquic_test_set_connect_accepted(context);

    *stream_before_accepted_count = *accepted_before_stream_count;
    wt_msquic_context_destroy(context);

    context = NULL;
    status = wt_msquic_context_create(&context);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_msquic_set_session_id(context, 1);
    wt_msquic_set_event_sink(context, 1, wt_test_event_counter, stream_before_accepted_count);
    wt_msquic_test_set_connect_stream_id(context, 0);
    wt_msquic_context_destroy(context);

    context = NULL;
    status = wt_msquic_context_create(&context);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_msquic_set_session_id(context, 1);
    wt_msquic_set_event_sink(context, 1, wt_test_event_counter, both_ready_count);
    wt_msquic_test_set_connect_accepted(context);
    wt_msquic_test_set_connect_stream_id(context, 0);
    wt_msquic_test_set_connect_accepted(context);
    wt_msquic_context_destroy(context);

    return WT_STATUS_OK;
}

WT_API wt_status wt_test_enqueue_datagram(uint64_t session_id, const uint8_t* payload, size_t payload_length)
{
    return wt_enqueue_datagram_from_msquic(NULL, session_id, payload, payload_length);
}

WT_API wt_status wt_test_append_stream_data(uint64_t stream_id, const uint8_t* payload, size_t payload_length)
{
    return wt_append_stream_data_from_msquic(NULL, stream_id, payload, payload_length);
}

WT_API wt_status wt_test_client_release_cleans_children(void)
{
    uint64_t client_id = 0;
    wt_status status = wt_client_create(&client_id);
    if (status != WT_STATUS_OK) {
        return status;
    }

    uint64_t session_id = 0;
    uint64_t stream_id = 0;
    wt_lock();

    status = wt_allocate_session(client_id, &session_id);
    if (status == WT_STATUS_OK) {
        status = wt_allocate_stream(session_id, client_id, &stream_id);
    }

    wt_unlock();
    if (status != WT_STATUS_OK) {
        wt_release(client_id);
        return status;
    }

    wt_release(client_id);

    wt_lock();
    wt_handle* session = wt_find_handle(session_id, WT_HANDLE_SESSION);
    wt_handle* stream = wt_find_handle(stream_id, WT_HANDLE_STREAM);
    wt_unlock();

    return session == NULL && stream == NULL ? WT_STATUS_OK : WT_STATUS_INVALID_STATE;
}
#endif

#include "wt_msquic.h"
#include "wt_datagrams.h"
#include "wt_http3.h"
#include "wt_qpack.h"
#include "wt_url.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WT_MAX_REQUEST_STREAM_BUFFER_BYTES (1024u * 1024u)
#define WT_MAX_SEND_BUFFER_BYTES UINT32_MAX

#if defined(WT_HAS_MSQUIC)
#include <msquic.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif
typedef struct wt_msquic_stream_entry wt_msquic_stream_entry;
#endif

struct wt_msquic_context {
#if defined(WT_HAS_MSQUIC)
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    uint8_t lock_initialized;
#endif
#if defined(WT_HAS_MSQUIC)
    const QUIC_API_TABLE* api;
    HQUIC registration;
    HQUIC configuration;
    HQUIC connection;
    HQUIC local_control_stream;
    HQUIC request_stream;
    wt_url parsed_url;
    char authority[320];
    wt_msquic_stream_entry* streams;
    wt_buffer request_stream_buffer;
    uint8_t* headers_utf8;
    size_t headers_length;
    uint64_t connect_stream_id;
    uint64_t datagram_flow_id;
    uint8_t connect_stream_id_ready;
    uint8_t connect_accepted;
    uint8_t session_connected_emitted;
#else
    int unavailable;
#endif
    uint64_t session_id;
    uint64_t client_id;
    wt_msquic_event_sink event_sink;
    void* event_sink_user_data;
    wt_msquic_stream_data_sink stream_data_sink;
    void* stream_data_sink_user_data;
    wt_msquic_datagram_sink datagram_sink;
    void* datagram_sink_user_data;
    wt_msquic_peer_stream_sink peer_stream_sink;
    void* peer_stream_sink_user_data;
    uint8_t closing;
};

#if defined(WT_HAS_MSQUIC)
typedef struct wt_msquic_stream_entry {
    uint64_t stream_id;
    HQUIC stream;
    struct wt_msquic_stream_entry* next;
} wt_msquic_stream_entry;

typedef struct wt_msquic_send_context {
    QUIC_BUFFER buffer;
    uint8_t* data;
} wt_msquic_send_context;

typedef struct wt_msquic_event_sink_snapshot {
    wt_msquic_event_sink sink;
    void* user_data;
    uint64_t client_id;
    uint64_t session_id;
} wt_msquic_event_sink_snapshot;

typedef struct wt_msquic_datagram_sink_snapshot {
    wt_msquic_datagram_sink sink;
    void* user_data;
    uint64_t session_id;
} wt_msquic_datagram_sink_snapshot;

typedef struct wt_msquic_stream_data_sink_snapshot {
    wt_msquic_stream_data_sink sink;
    void* user_data;
} wt_msquic_stream_data_sink_snapshot;

typedef struct wt_msquic_peer_stream_sink_snapshot {
    wt_msquic_peer_stream_sink sink;
    void* user_data;
} wt_msquic_peer_stream_sink_snapshot;

static void wt_msquic_emit_error(wt_msquic_context* context, wt_status status, uint64_t error_code);
static void wt_msquic_try_emit_session_connected(wt_msquic_context* context);

static wt_status wt_msquic_lock_init(wt_msquic_context* context)
{
    if (context == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

#ifdef _WIN32
    InitializeCriticalSection(&context->lock);
    context->lock_initialized = 1;
    return WT_STATUS_OK;
#else
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) != 0) {
        return WT_STATUS_INVALID_STATE;
    }

    if (pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) != 0) {
        (void)pthread_mutexattr_destroy(&attributes);
        return WT_STATUS_INVALID_STATE;
    }

    if (pthread_mutex_init(&context->lock, &attributes) != 0) {
        (void)pthread_mutexattr_destroy(&attributes);
        return WT_STATUS_INVALID_STATE;
    }

    (void)pthread_mutexattr_destroy(&attributes);
    context->lock_initialized = 1;
    return WT_STATUS_OK;
#endif
}

static void wt_msquic_lock_destroy(wt_msquic_context* context)
{
    if (context == NULL || !context->lock_initialized) {
        return;
    }

#ifdef _WIN32
    DeleteCriticalSection(&context->lock);
#else
    (void)pthread_mutex_destroy(&context->lock);
#endif
    context->lock_initialized = 0;
}

static void wt_msquic_lock(wt_msquic_context* context)
{
    if (context == NULL || !context->lock_initialized) {
        return;
    }

#ifdef _WIN32
    EnterCriticalSection(&context->lock);
#else
    (void)pthread_mutex_lock(&context->lock);
#endif
}

static void wt_msquic_unlock(wt_msquic_context* context)
{
    if (context == NULL || !context->lock_initialized) {
        return;
    }

#ifdef _WIN32
    LeaveCriticalSection(&context->lock);
#else
    (void)pthread_mutex_unlock(&context->lock);
#endif
}

static wt_status wt_msquic_encode_datagram_for_send(
    wt_msquic_context* context,
    uint64_t session_id,
    const uint8_t* payload,
    size_t payload_length,
    wt_buffer* encoded)
{
    if (context == NULL || session_id == 0 || encoded == NULL || (payload == NULL && payload_length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_msquic_lock(context);
    uint8_t connect_stream_id_ready = context->connect_stream_id_ready;
    uint64_t datagram_flow_id = context->datagram_flow_id;
    wt_msquic_unlock(context);

    if (!connect_stream_id_ready) {
        return WT_STATUS_INVALID_STATE;
    }

    return wt_datagrams_encode(datagram_flow_id, payload, payload_length, encoded);
}

static wt_status wt_msquic_deliver_datagram(
    wt_msquic_context* context,
    const uint8_t* datagram,
    size_t datagram_length)
{
    if (context == NULL || datagram == NULL || datagram_length == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint64_t datagram_flow_id = 0;
    const uint8_t* payload = NULL;
    size_t payload_length = 0;
    wt_status status = wt_datagrams_decode(
        datagram,
        datagram_length,
        &datagram_flow_id,
        &payload,
        &payload_length);
    if (status != WT_STATUS_OK) {
        return status;
    }

    wt_msquic_datagram_sink_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    wt_msquic_lock(context);
    uint8_t connect_stream_id_ready = context->connect_stream_id_ready;
    uint64_t expected_datagram_flow_id = context->datagram_flow_id;
    snapshot.sink = context->datagram_sink;
    snapshot.user_data = context->datagram_sink_user_data;
    snapshot.session_id = context->session_id;
    wt_msquic_unlock(context);

    if (!connect_stream_id_ready || datagram_flow_id != expected_datagram_flow_id) {
        return WT_STATUS_PROTOCOL_ERROR;
    }

    if (snapshot.sink == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    return snapshot.sink(
        snapshot.user_data,
        snapshot.session_id,
        payload,
        payload_length);
}

static wt_msquic_send_context* wt_msquic_create_send_context(const uint8_t* data, size_t length)
{
    if (data == NULL && length != 0) {
        return NULL;
    }

    if (length > WT_MAX_SEND_BUFFER_BYTES) {
        return NULL;
    }

    wt_msquic_send_context* send_context = (wt_msquic_send_context*)calloc(1, sizeof(wt_msquic_send_context));
    if (send_context == NULL) {
        return NULL;
    }

    if (length != 0) {
        send_context->data = (uint8_t*)malloc(length);
        if (send_context->data == NULL) {
            free(send_context);
            return NULL;
        }

        memcpy(send_context->data, data, length);
    }

    send_context->buffer.Length = (uint32_t)length;
    send_context->buffer.Buffer = send_context->data;
    return send_context;
}

static void wt_msquic_free_send_context(void* context)
{
    wt_msquic_send_context* send_context = (wt_msquic_send_context*)context;
    if (send_context == NULL) {
        return;
    }

    free(send_context->data);
    free(send_context);
}

static wt_status wt_msquic_process_request_stream(wt_msquic_context* context, const uint8_t* data, size_t length)
{
    if (context == NULL || (data == NULL && length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_msquic_lock(context);

    if (length > WT_MAX_REQUEST_STREAM_BUFFER_BYTES ||
        context->request_stream_buffer.length > WT_MAX_REQUEST_STREAM_BUFFER_BYTES - length) {
        wt_msquic_unlock(context);
        return WT_STATUS_OUT_OF_MEMORY;
    }

    wt_status status = wt_buffer_append(&context->request_stream_buffer, data, length);
    if (status != WT_STATUS_OK) {
        wt_msquic_unlock(context);
        return status;
    }

    uint8_t should_emit_connected = 0;
    uint8_t should_emit_error = 0;
    uint64_t error_code = 0;
    size_t offset = 0;
    while (offset < context->request_stream_buffer.length) {
        size_t frame_start = offset;
        uint64_t frame_type;
        uint64_t payload_length;
        status = wt_http3_decode_varint(context->request_stream_buffer.data, context->request_stream_buffer.length, &offset, &frame_type);
        if (status != WT_STATUS_OK) {
            offset = frame_start;
            break;
        }

        status = wt_http3_decode_varint(context->request_stream_buffer.data, context->request_stream_buffer.length, &offset, &payload_length);
        if (status != WT_STATUS_OK) {
            offset = frame_start;
            break;
        }

        if (payload_length > SIZE_MAX ||
            offset > context->request_stream_buffer.length ||
            (size_t)payload_length > context->request_stream_buffer.length - offset) {
            offset = frame_start;
            break;
        }

        if (frame_type == WT_H3_FRAME_HEADERS) {
            uint16_t status_code = 0;
            if (payload_length > WT_MAX_REQUEST_STREAM_BUFFER_BYTES) {
                status = WT_STATUS_PROTOCOL_ERROR;
            } else {
                status = wt_qpack_decode_status(context->request_stream_buffer.data + offset, (size_t)payload_length, &status_code);
            }
            if (status == WT_STATUS_OK && status_code >= 200 && status_code < 300) {
                context->connect_accepted = 1;
                should_emit_connected = 1;
            } else {
                should_emit_error = 1;
                error_code = status_code;
            }
        }

        offset += (size_t)payload_length;
    }

    if (offset != 0) {
        size_t remaining = context->request_stream_buffer.length - offset;
        memmove(context->request_stream_buffer.data, context->request_stream_buffer.data + offset, remaining);
        context->request_stream_buffer.length = remaining;
    }

    wt_msquic_unlock(context);
    if (should_emit_connected) {
        wt_msquic_try_emit_session_connected(context);
    }

    if (should_emit_error) {
        wt_msquic_emit_error(context, WT_STATUS_PROTOCOL_ERROR, error_code);
    }

    return WT_STATUS_OK;
}

static wt_msquic_stream_entry* wt_msquic_find_stream_by_handle(wt_msquic_context* context, HQUIC stream)
{
    wt_msquic_stream_entry* current = context == NULL ? NULL : context->streams;
    while (current != NULL) {
        if (current->stream == stream) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

static uint64_t wt_msquic_find_stream_id_by_handle_locked(wt_msquic_context* context, HQUIC stream)
{
    wt_msquic_stream_entry* entry = wt_msquic_find_stream_by_handle(context, stream);
    return entry == NULL ? 0 : entry->stream_id;
}

static wt_msquic_stream_entry* wt_msquic_find_stream_by_id(wt_msquic_context* context, uint64_t stream_id)
{
    wt_msquic_stream_entry* current = context == NULL ? NULL : context->streams;
    while (current != NULL) {
        if (current->stream_id == stream_id) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

static wt_status wt_msquic_add_stream(wt_msquic_context* context, uint64_t stream_id, HQUIC stream)
{
    if (context == NULL || stream == NULL || stream_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_msquic_stream_entry* entry = (wt_msquic_stream_entry*)calloc(1, sizeof(wt_msquic_stream_entry));
    if (entry == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    wt_msquic_lock(context);
    entry->stream_id = stream_id;
    entry->stream = stream;
    entry->next = context->streams;
    context->streams = entry;
    wt_msquic_unlock(context);
    return WT_STATUS_OK;
}

static void wt_msquic_remove_stream_locked(wt_msquic_context* context, HQUIC stream)
{
    if (context == NULL || stream == NULL) {
        return;
    }

    wt_msquic_stream_entry** current = &context->streams;
    while (*current != NULL) {
        if ((*current)->stream == stream) {
            wt_msquic_stream_entry* removed = *current;
            *current = removed->next;
            free(removed);
            return;
        }

        current = &(*current)->next;
    }
}

static void wt_msquic_remove_stream(wt_msquic_context* context, HQUIC stream)
{
    wt_msquic_lock(context);
    wt_msquic_remove_stream_locked(context, stream);
    wt_msquic_unlock(context);
}

static HQUIC* wt_msquic_copy_stream_handles_locked(wt_msquic_context* context, size_t* count)
{
    if (count == NULL) {
        return NULL;
    }

    *count = 0;
    if (context == NULL) {
        return NULL;
    }

    size_t stream_count = 0;
    wt_msquic_stream_entry* current = context->streams;
    while (current != NULL) {
        if (current->stream != NULL) {
            stream_count++;
        }

        current = current->next;
    }

    if (stream_count == 0) {
        return NULL;
    }

    HQUIC* streams = (HQUIC*)calloc(stream_count, sizeof(HQUIC));
    if (streams == NULL) {
        return NULL;
    }

    size_t index = 0;
    current = context->streams;
    while (current != NULL) {
        if (current->stream != NULL) {
            streams[index++] = current->stream;
        }

        current = current->next;
    }

    *count = index;
    return streams;
}

static void wt_msquic_emit_error(wt_msquic_context* context, wt_status status, uint64_t error_code)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_event_sink_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    wt_msquic_lock(context);
    if (context->closing || context->event_sink == NULL) {
        wt_msquic_unlock(context);
        return;
    }

    snapshot.sink = context->event_sink;
    snapshot.user_data = context->event_sink_user_data;
    snapshot.client_id = context->client_id;
    wt_msquic_unlock(context);

    wt_event native_event;
    memset(&native_event, 0, sizeof(native_event));
    native_event.type = WT_EVENT_ERROR;
    native_event.status = status;
    native_event.client_id = snapshot.client_id;
    native_event.error_code = error_code;
    snapshot.sink(snapshot.user_data, &native_event);
}

static void wt_msquic_try_emit_session_connected(wt_msquic_context* context)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_event_sink_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    wt_msquic_lock(context);
    if (context->closing ||
        context->event_sink == NULL ||
        !context->connect_accepted ||
        !context->connect_stream_id_ready ||
        context->session_connected_emitted) {
        wt_msquic_unlock(context);
        return;
    }

    snapshot.sink = context->event_sink;
    snapshot.user_data = context->event_sink_user_data;
    snapshot.client_id = context->client_id;
    snapshot.session_id = context->session_id;
    context->session_connected_emitted = 1;
    wt_msquic_unlock(context);

    wt_event native_event;
    memset(&native_event, 0, sizeof(native_event));
    native_event.type = WT_EVENT_SESSION_CONNECTED;
    native_event.status = WT_STATUS_OK;
    native_event.client_id = snapshot.client_id;
    native_event.session_id = snapshot.session_id;
    snapshot.sink(snapshot.user_data, &native_event);
}

static QUIC_STATUS QUIC_API wt_msquic_stream_callback(
    HQUIC stream,
    void* context,
    QUIC_STREAM_EVENT* event)
{
    (void)stream;

    wt_msquic_context* wt_context = (wt_msquic_context*)context;
    if (wt_context == NULL || event == NULL) {
        return QUIC_STATUS_SUCCESS;
    }

    wt_msquic_lock(wt_context);
    uint8_t closing = wt_context->closing;
    HQUIC request_stream = wt_context->request_stream;
    wt_msquic_unlock(wt_context);

    if (closing) {
        if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
            wt_msquic_free_send_context(event->SEND_COMPLETE.ClientContext);
        }

        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_START_COMPLETE && stream == request_stream) {
        wt_msquic_lock(wt_context);
        wt_context->connect_stream_id = event->START_COMPLETE.ID;
        wt_context->datagram_flow_id = event->START_COMPLETE.ID / 4;
        wt_context->connect_stream_id_ready = 1;
        wt_msquic_unlock(wt_context);
        wt_msquic_try_emit_session_connected(wt_context);
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        wt_msquic_free_send_context(event->SEND_COMPLETE.ClientContext);
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_RECEIVE && stream == request_stream) {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            const QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[i];
            if (wt_msquic_process_request_stream(wt_context, buffer->Buffer, buffer->Length) != WT_STATUS_OK) {
                wt_msquic_emit_error(wt_context, WT_STATUS_PROTOCOL_ERROR, 0);
            }
        }
    }

    wt_msquic_stream_data_sink_snapshot stream_data_snapshot;
    memset(&stream_data_snapshot, 0, sizeof(stream_data_snapshot));

    wt_msquic_event_sink_snapshot event_snapshot;
    memset(&event_snapshot, 0, sizeof(event_snapshot));

    wt_msquic_lock(wt_context);
    uint64_t stream_id = wt_msquic_find_stream_id_by_handle_locked(wt_context, stream);
    stream_data_snapshot.sink = wt_context->stream_data_sink;
    stream_data_snapshot.user_data = wt_context->stream_data_sink_user_data;
    event_snapshot.sink = wt_context->event_sink;
    event_snapshot.user_data = wt_context->event_sink_user_data;
    event_snapshot.client_id = wt_context->client_id;
    event_snapshot.session_id = wt_context->session_id;
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE && stream_id != 0) {
        wt_msquic_remove_stream_locked(wt_context, stream);
    }
    wt_msquic_unlock(wt_context);

    if (event->Type == QUIC_STREAM_EVENT_RECEIVE && stream_id == 0) {
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_RECEIVE && stream_id != 0 && stream_data_snapshot.sink != NULL) {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            const QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[i];
            stream_data_snapshot.sink(
                stream_data_snapshot.user_data,
                stream_id,
                buffer->Buffer,
                buffer->Length);
        }
    } else if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE && stream_id != 0) {
        wt_event native_event;
        memset(&native_event, 0, sizeof(native_event));
        native_event.type = WT_EVENT_STREAM_CLOSED;
        native_event.status = WT_STATUS_OK;
        native_event.client_id = event_snapshot.client_id;
        native_event.session_id = event_snapshot.session_id;
        native_event.stream_id = stream_id;
        if (event_snapshot.sink != NULL) {
            event_snapshot.sink(event_snapshot.user_data, &native_event);
        }
    }

    return QUIC_STATUS_SUCCESS;
}

static wt_status wt_msquic_send_control_settings(wt_msquic_context* context)
{
    if (context == NULL || context->api == NULL || context->connection == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (context->local_control_stream != NULL) {
        return WT_STATUS_OK;
    }

    QUIC_STATUS quic_status = context->api->StreamOpen(
        context->connection,
        QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        wt_msquic_stream_callback,
        context,
        &context->local_control_stream);
    if (QUIC_FAILED(quic_status)) {
        return WT_STATUS_TRANSPORT_ERROR;
    }

    wt_buffer payload;
    wt_buffer_init(&payload);

    wt_status status = wt_http3_encode_varint(WT_H3_STREAM_TYPE_CONTROL, &payload);
    if (status == WT_STATUS_OK) {
        status = wt_http3_encode_client_settings(1, &payload);
    }

    if (status != WT_STATUS_OK) {
        wt_buffer_free(&payload);
        return status;
    }

    wt_msquic_send_context* send_context = wt_msquic_create_send_context(payload.data, payload.length);
    if (send_context == NULL) {
        wt_buffer_free(&payload);
        return WT_STATUS_OUT_OF_MEMORY;
    }

    quic_status = context->api->StreamSend(
        context->local_control_stream,
        &send_context->buffer,
        1,
        QUIC_SEND_FLAG_START,
        send_context);

    wt_buffer_free(&payload);
    if (QUIC_FAILED(quic_status)) {
        wt_msquic_free_send_context(send_context);
    }
    return QUIC_FAILED(quic_status) ? WT_STATUS_TRANSPORT_ERROR : WT_STATUS_OK;
}

static wt_status wt_msquic_send_webtransport_connect(wt_msquic_context* context)
{
    if (context == NULL || context->api == NULL || context->connection == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (context->request_stream != NULL) {
        return WT_STATUS_OK;
    }

    QUIC_STATUS quic_status = context->api->StreamOpen(
        context->connection,
        QUIC_STREAM_OPEN_FLAG_NONE,
        wt_msquic_stream_callback,
        context,
        &context->request_stream);
    if (QUIC_FAILED(quic_status)) {
        return WT_STATUS_TRANSPORT_ERROR;
    }

    wt_http3_session_request request;
    memset(&request, 0, sizeof(request));
    request.authority = (const uint8_t*)context->authority;
    request.authority_length = strlen(context->authority);
    request.path = (const uint8_t*)context->parsed_url.path;
    request.path_length = strlen(context->parsed_url.path);
    request.headers = context->headers_utf8;
    request.headers_length = context->headers_length;

    wt_buffer payload;
    wt_buffer_init(&payload);
    wt_status status = wt_http3_encode_webtransport_connect(&request, &payload);
    if (status != WT_STATUS_OK) {
        wt_buffer_free(&payload);
        return status;
    }

    wt_msquic_send_context* send_context = wt_msquic_create_send_context(payload.data, payload.length);
    if (send_context == NULL) {
        wt_buffer_free(&payload);
        return WT_STATUS_OUT_OF_MEMORY;
    }

    quic_status = context->api->StreamSend(
        context->request_stream,
        &send_context->buffer,
        1,
        QUIC_SEND_FLAG_START,
        send_context);

    wt_buffer_free(&payload);
    if (QUIC_FAILED(quic_status)) {
        wt_msquic_free_send_context(send_context);
    }
    return QUIC_FAILED(quic_status) ? WT_STATUS_TRANSPORT_ERROR : WT_STATUS_OK;
}

static QUIC_STATUS QUIC_API wt_msquic_connection_callback(
    HQUIC connection,
    void* context,
    QUIC_CONNECTION_EVENT* event)
{
    (void)connection;

    wt_msquic_context* wt_context = (wt_msquic_context*)context;
    if (wt_context == NULL || event == NULL) {
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED) {
        if (event->DATAGRAM_SEND_STATE_CHANGED.State != QUIC_DATAGRAM_SEND_SENT) {
            wt_msquic_free_send_context(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
        }

        return QUIC_STATUS_SUCCESS;
    }

    wt_msquic_event_sink_snapshot event_snapshot;
    memset(&event_snapshot, 0, sizeof(event_snapshot));

    wt_msquic_lock(wt_context);
    if (wt_context->closing || wt_context->event_sink == NULL) {
        wt_msquic_unlock(wt_context);
        return QUIC_STATUS_SUCCESS;
    }

    event_snapshot.sink = wt_context->event_sink;
    event_snapshot.user_data = wt_context->event_sink_user_data;
    event_snapshot.client_id = wt_context->client_id;
    const QUIC_API_TABLE* api = wt_context->api;
    wt_msquic_peer_stream_sink_snapshot peer_stream_snapshot;
    memset(&peer_stream_snapshot, 0, sizeof(peer_stream_snapshot));
    peer_stream_snapshot.sink = wt_context->peer_stream_sink;
    peer_stream_snapshot.user_data = wt_context->peer_stream_sink_user_data;
    wt_msquic_unlock(wt_context);

    wt_event native_event;
    memset(&native_event, 0, sizeof(native_event));
    native_event.client_id = event_snapshot.client_id;
    native_event.status = WT_STATUS_OK;

    switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            native_event.type = WT_EVENT_CLIENT_CONNECTED;
            wt_status status = wt_msquic_send_control_settings(wt_context);
            if (status != WT_STATUS_OK) {
                wt_msquic_emit_error(wt_context, status, 1);
            }
            status = wt_msquic_send_webtransport_connect(wt_context);
            if (status != WT_STATUS_OK) {
                wt_msquic_emit_error(wt_context, status, 2);
            }
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            native_event.type = WT_EVENT_ERROR;
            native_event.status = WT_STATUS_TRANSPORT_ERROR;
            native_event.error_code = event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            native_event.type = WT_EVENT_CLIENT_CLOSED;
            native_event.error_code = event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            native_event.type = WT_EVENT_CLIENT_CLOSED;
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            // Register the stream (ABI handle + MsQuic stream list) BEFORE
            // installing the receive callback handler. Otherwise a RECEIVE
            // could fire on a worker thread before the stream is tracked and
            // its initial data would be dropped.
            uint8_t bidirectional = (event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) == 0 ? 1 : 0;
            uint8_t registered = 0;
            if (peer_stream_snapshot.sink != NULL) {
                uint64_t stream_id = 0;
                if (peer_stream_snapshot.sink(peer_stream_snapshot.user_data, event->PEER_STREAM_STARTED.Stream, bidirectional, &stream_id) == WT_STATUS_OK) {
                    wt_msquic_add_stream(wt_context, stream_id, event->PEER_STREAM_STARTED.Stream);
                    registered = 1;
                }
            }

            if (registered && api != NULL) {
                api->SetCallbackHandler(
                    event->PEER_STREAM_STARTED.Stream,
                    (void*)wt_msquic_stream_callback,
                    wt_context);
            }

            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            wt_status datagram_status = wt_msquic_deliver_datagram(
                wt_context,
                event->DATAGRAM_RECEIVED.Buffer->Buffer,
                event->DATAGRAM_RECEIVED.Buffer->Length);
            if (datagram_status != WT_STATUS_OK) {
                wt_msquic_emit_error(wt_context, WT_STATUS_PROTOCOL_ERROR, 0);
            }

            return QUIC_STATUS_SUCCESS;
        }

        default:
            return QUIC_STATUS_SUCCESS;
    }

    event_snapshot.sink(event_snapshot.user_data, &native_event);
    return QUIC_STATUS_SUCCESS;
}
#endif

wt_status wt_msquic_context_create(wt_msquic_context** context)
{
    if (context == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_msquic_context* created = (wt_msquic_context*)calloc(1, sizeof(wt_msquic_context));
    if (created == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

#if defined(WT_HAS_MSQUIC)
    wt_status lock_status = wt_msquic_lock_init(created);
    if (lock_status != WT_STATUS_OK) {
        free(created);
        return lock_status;
    }

    wt_buffer_init(&created->request_stream_buffer);
#endif

#if defined(WT_HAS_MSQUIC)
    if (QUIC_FAILED(MsQuicOpen2(&created->api))) {
        wt_msquic_lock_destroy(created);
        free(created);
        return WT_STATUS_TRANSPORT_ERROR;
    }

    const QUIC_REGISTRATION_CONFIG registration_config = {
        "webtransport-csharp",
        QUIC_EXECUTION_PROFILE_LOW_LATENCY
    };

    if (QUIC_FAILED(created->api->RegistrationOpen(&registration_config, &created->registration))) {
        MsQuicClose(created->api);
        wt_msquic_lock_destroy(created);
        free(created);
        return WT_STATUS_TRANSPORT_ERROR;
    }
#endif

    *context = created;
    return WT_STATUS_OK;
}

void wt_msquic_set_event_sink(
    wt_msquic_context* context,
    uint64_t client_id,
    wt_msquic_event_sink event_sink,
    void* event_sink_user_data)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_lock(context);
    context->client_id = client_id;
    context->event_sink = event_sink;
    context->event_sink_user_data = event_sink_user_data;
    wt_msquic_unlock(context);
}

void wt_msquic_set_session_id(wt_msquic_context* context, uint64_t session_id)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_lock(context);
    context->session_id = session_id;
    wt_msquic_unlock(context);
}

void wt_msquic_set_stream_data_sink(
    wt_msquic_context* context,
    wt_msquic_stream_data_sink stream_data_sink,
    void* stream_data_sink_user_data)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_lock(context);
    context->stream_data_sink = stream_data_sink;
    context->stream_data_sink_user_data = stream_data_sink_user_data;
    wt_msquic_unlock(context);
}

void wt_msquic_set_datagram_sink(
    wt_msquic_context* context,
    wt_msquic_datagram_sink datagram_sink,
    void* datagram_sink_user_data)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_lock(context);
    context->datagram_sink = datagram_sink;
    context->datagram_sink_user_data = datagram_sink_user_data;
    wt_msquic_unlock(context);
}

void wt_msquic_set_peer_stream_sink(
    wt_msquic_context* context,
    wt_msquic_peer_stream_sink peer_stream_sink,
    void* peer_stream_sink_user_data)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_lock(context);
    context->peer_stream_sink = peer_stream_sink;
    context->peer_stream_sink_user_data = peer_stream_sink_user_data;
    wt_msquic_unlock(context);
}

wt_status wt_msquic_datagram_send(wt_msquic_context* context, uint64_t session_id, const uint8_t* payload, size_t payload_length)
{
    if (context == NULL || session_id == 0 || (payload == NULL && payload_length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    HQUIC connection = context->connection;
    const QUIC_API_TABLE* api = context->api;
    wt_msquic_unlock(context);

    if (connection == NULL || api == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    wt_buffer encoded;
    wt_buffer_init(&encoded);
    wt_status status = wt_msquic_encode_datagram_for_send(context, session_id, payload, payload_length, &encoded);
    if (status != WT_STATUS_OK) {
        wt_buffer_free(&encoded);
        return status;
    }

    wt_msquic_send_context* send_context = wt_msquic_create_send_context(encoded.data, encoded.length);
    if (send_context == NULL) {
        wt_buffer_free(&encoded);
        return WT_STATUS_OUT_OF_MEMORY;
    }

    QUIC_STATUS quic_status = api->DatagramSend(
        connection,
        &send_context->buffer,
        1,
        QUIC_SEND_FLAG_NONE,
        send_context);

    wt_buffer_free(&encoded);
    if (QUIC_FAILED(quic_status)) {
        wt_msquic_free_send_context(send_context);
    }
    return QUIC_FAILED(quic_status) ? WT_STATUS_TRANSPORT_ERROR : WT_STATUS_OK;
#else
    return WT_STATUS_UNSUPPORTED;
#endif
}

wt_status wt_msquic_open_bidi_stream(wt_msquic_context* context, uint64_t session_id, uint64_t stream_id)
{
    if (context == NULL || session_id == 0 || stream_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    HQUIC connection = context->connection;
    const QUIC_API_TABLE* api = context->api;
    uint8_t connect_stream_id_ready = context->connect_stream_id_ready;
    uint64_t connect_stream_id = context->connect_stream_id;
    wt_msquic_unlock(context);

    if (connection == NULL || api == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    HQUIC stream = NULL;
    QUIC_STATUS quic_status = api->StreamOpen(
        connection,
        QUIC_STREAM_OPEN_FLAG_NONE,
        wt_msquic_stream_callback,
        context,
        &stream);
    if (QUIC_FAILED(quic_status)) {
        return WT_STATUS_TRANSPORT_ERROR;
    }

    wt_status status = wt_msquic_add_stream(context, stream_id, stream);
    if (status != WT_STATUS_OK) {
        api->StreamClose(stream);
        return status;
    }

    wt_buffer prefix;
    wt_buffer_init(&prefix);
    status = wt_http3_encode_varint(WT_H3_WEBTRANSPORT_STREAM_FRAME_TYPE, &prefix);
    if (status == WT_STATUS_OK) {
        if (!connect_stream_id_ready) {
            status = WT_STATUS_INVALID_STATE;
        } else {
            status = wt_http3_encode_varint(connect_stream_id, &prefix);
        }
    }

    if (status != WT_STATUS_OK) {
        wt_buffer_free(&prefix);
        api->StreamClose(stream);
        wt_msquic_remove_stream(context, stream);
        return status;
    }

    wt_msquic_send_context* send_context = wt_msquic_create_send_context(prefix.data, prefix.length);
    if (send_context == NULL) {
        wt_buffer_free(&prefix);
        api->StreamClose(stream);
        wt_msquic_remove_stream(context, stream);
        return WT_STATUS_OUT_OF_MEMORY;
    }

    quic_status = api->StreamSend(stream, &send_context->buffer, 1, QUIC_SEND_FLAG_START, send_context);

    wt_buffer_free(&prefix);
    if (QUIC_FAILED(quic_status)) {
        wt_msquic_free_send_context(send_context);
        api->StreamClose(stream);
        wt_msquic_remove_stream(context, stream);
        return WT_STATUS_TRANSPORT_ERROR;
    }

    return WT_STATUS_OK;
#else
    return WT_STATUS_UNSUPPORTED;
#endif
}

wt_status wt_msquic_stream_write(wt_msquic_context* context, uint64_t stream_id, const uint8_t* payload, size_t payload_length, uint8_t end_stream)
{
    if (context == NULL || stream_id == 0 || (payload == NULL && payload_length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    wt_msquic_stream_entry* entry = wt_msquic_find_stream_by_id(context, stream_id);
    HQUIC stream = entry == NULL ? NULL : entry->stream;
    const QUIC_API_TABLE* api = context->api;
    wt_msquic_unlock(context);
    if (stream == NULL || api == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    wt_msquic_send_context* send_context = wt_msquic_create_send_context(payload, payload_length);
    if (send_context == NULL) {
        return WT_STATUS_OUT_OF_MEMORY;
    }

    QUIC_SEND_FLAGS flags = end_stream ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE;
    QUIC_STATUS status = api->StreamSend(
        stream,
        payload_length == 0 ? NULL : &send_context->buffer,
        payload_length == 0 ? 0 : 1,
        flags,
        send_context);
    if (QUIC_FAILED(status)) {
        wt_msquic_free_send_context(send_context);
    }
    return QUIC_FAILED(status) ? WT_STATUS_TRANSPORT_ERROR : WT_STATUS_OK;
#else
    return WT_STATUS_UNSUPPORTED;
#endif
}

wt_status wt_msquic_stream_finish(wt_msquic_context* context, uint64_t stream_id)
{
    return wt_msquic_stream_write(context, stream_id, NULL, 0, 1);
}

wt_status wt_msquic_stream_reset(wt_msquic_context* context, uint64_t stream_id, uint64_t error_code)
{
    if (context == NULL || stream_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    wt_msquic_stream_entry* entry = wt_msquic_find_stream_by_id(context, stream_id);
    HQUIC stream = entry == NULL ? NULL : entry->stream;
    const QUIC_API_TABLE* api = context->api;
    wt_msquic_unlock(context);
    if (stream == NULL || api == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    QUIC_STATUS status = api->StreamShutdown(
        stream,
        QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
        error_code);
    return QUIC_FAILED(status) ? WT_STATUS_TRANSPORT_ERROR : WT_STATUS_OK;
#else
    return WT_STATUS_UNSUPPORTED;
#endif
}

void wt_msquic_close_stream_handle(wt_msquic_context* context, uint64_t stream_id)
{
#if defined(WT_HAS_MSQUIC)
    if (context == NULL || context->api == NULL || stream_id == 0) {
        return;
    }

    wt_msquic_lock(context);
    wt_msquic_stream_entry* entry = wt_msquic_find_stream_by_id(context, stream_id);
    HQUIC stream = entry == NULL ? NULL : entry->stream;
    const QUIC_API_TABLE* api = context->api;
    if (stream != NULL) {
        wt_msquic_remove_stream_locked(context, stream);
    }
    wt_msquic_unlock(context);

    if (stream == NULL || api == NULL) {
        return;
    }

    api->StreamClose(stream);
#else
    (void)context;
    (void)stream_id;
#endif
}

void wt_msquic_shutdown(wt_msquic_context* context, uint64_t error_code)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_lock(context);
    context->closing = 1;
    context->event_sink = NULL;
    context->event_sink_user_data = NULL;
    context->stream_data_sink = NULL;
    context->stream_data_sink_user_data = NULL;
    context->datagram_sink = NULL;
    context->datagram_sink_user_data = NULL;
    context->peer_stream_sink = NULL;
    context->peer_stream_sink_user_data = NULL;

#if defined(WT_HAS_MSQUIC)
    const QUIC_API_TABLE* api = context->api;
    HQUIC request_stream = context->request_stream;
    HQUIC local_control_stream = context->local_control_stream;
    HQUIC connection = context->connection;
    size_t stream_count = 0;
    HQUIC* streams = wt_msquic_copy_stream_handles_locked(context, &stream_count);
    if (api == NULL) {
        wt_msquic_unlock(context);
        free(streams);
        return;
    }
    wt_msquic_unlock(context);

    for (size_t i = 0; i < stream_count; i++) {
        api->StreamShutdown(
            streams[i],
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
            error_code);
    }
    free(streams);

    if (request_stream != NULL) {
        api->StreamShutdown(
            request_stream,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
            error_code);
    }

    if (local_control_stream != NULL) {
        api->StreamShutdown(
            local_control_stream,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
            error_code);
    }

    if (connection != NULL) {
        api->ConnectionShutdown(
            connection,
            (QUIC_CONNECTION_SHUTDOWN_FLAGS)0,
            error_code);
    }
#else
    wt_msquic_unlock(context);
    (void)error_code;
#endif
}

wt_status wt_msquic_connect(wt_msquic_context* context, const wt_connect_options* options)
{
    if (context == NULL || options == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (options->url_utf8 == NULL || options->url_length == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_url parsed_url;
    wt_status parse_status = wt_url_parse_https(options->url_utf8, options->url_length, &parsed_url);
    if (parse_status != WT_STATUS_OK) {
        return parse_status;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    context->parsed_url = parsed_url;
    context->connect_stream_id = 0;
    context->datagram_flow_id = 0;
    context->connect_stream_id_ready = 0;
    context->connect_accepted = 0;
    context->session_connected_emitted = 0;
    context->request_stream_buffer.length = 0;
    free(context->headers_utf8);
    context->headers_utf8 = NULL;
    context->headers_length = 0;
    if (options->headers_length != 0) {
        if (options->headers_utf8 == NULL) {
            wt_msquic_unlock(context);
            return WT_STATUS_INVALID_ARGUMENT;
        }

        context->headers_utf8 = (uint8_t*)malloc(options->headers_length);
        if (context->headers_utf8 == NULL) {
            wt_msquic_unlock(context);
            return WT_STATUS_OUT_OF_MEMORY;
        }

        memcpy(context->headers_utf8, options->headers_utf8, options->headers_length);
        context->headers_length = options->headers_length;
    }
    const QUIC_API_TABLE* api = context->api;
    HQUIC registration = context->registration;
    HQUIC configuration = context->configuration;
    HQUIC existing_connection = context->connection;
    context->connection = NULL;
    wt_msquic_unlock(context);

    if (api == NULL || registration == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    if (parsed_url.port == 443) {
        snprintf(context->authority, sizeof(context->authority), "%s", parsed_url.host);
    } else {
        snprintf(context->authority, sizeof(context->authority), "%s:%u", parsed_url.host, parsed_url.port);
    }

    const QUIC_BUFFER alpn = {
        2,
        (uint8_t*)"h3"
    };

    QUIC_SETTINGS quic_settings;
    memset(&quic_settings, 0, sizeof(quic_settings));
    quic_settings.IdleTimeoutMs = options->idle_timeout_milliseconds;
    quic_settings.IsSet.IdleTimeoutMs = TRUE;
    quic_settings.DatagramReceiveEnabled = options->enable_datagrams ? TRUE : FALSE;
    quic_settings.IsSet.DatagramReceiveEnabled = TRUE;
    quic_settings.PeerBidiStreamCount = 16;
    quic_settings.IsSet.PeerBidiStreamCount = TRUE;
    quic_settings.PeerUnidiStreamCount = 16;
    quic_settings.IsSet.PeerUnidiStreamCount = TRUE;

    if (configuration == NULL) {
        QUIC_STATUS status = api->ConfigurationOpen(
            registration,
            &alpn,
            1,
            &quic_settings,
            sizeof(quic_settings),
            NULL,
            &configuration);
        if (QUIC_FAILED(status)) {
            return WT_STATUS_TRANSPORT_ERROR;
        }

        QUIC_CREDENTIAL_CONFIG credentials;
        memset(&credentials, 0, sizeof(credentials));
        credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
        if (options->allow_untrusted_certificates) {
            credentials.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        }

        status = api->ConfigurationLoadCredential(configuration, &credentials);
        if (QUIC_FAILED(status)) {
            api->ConfigurationClose(configuration);
            return WT_STATUS_TLS_ERROR;
        }

        wt_msquic_lock(context);
        if (context->configuration == NULL) {
            context->configuration = configuration;
            configuration = NULL;
        }
        wt_msquic_unlock(context);

        if (configuration != NULL) {
            api->ConfigurationClose(configuration);
        }
    }

    if (existing_connection != NULL) {
        api->ConnectionClose(existing_connection);
    }

    wt_msquic_lock(context);
    configuration = context->configuration;
    wt_msquic_unlock(context);

    if (configuration == NULL) {
        return WT_STATUS_INVALID_STATE;
    }

    HQUIC connection = NULL;
    QUIC_STATUS status = api->ConnectionOpen(
        registration,
        wt_msquic_connection_callback,
        context,
        &connection);
    if (QUIC_FAILED(status)) {
        return WT_STATUS_TRANSPORT_ERROR;
    }

    wt_msquic_lock(context);
    context->connection = connection;
    wt_msquic_unlock(context);

    status = api->ConnectionStart(
        connection,
        configuration,
        QUIC_ADDRESS_FAMILY_UNSPEC,
        parsed_url.host,
        parsed_url.port);
    if (QUIC_FAILED(status)) {
        api->ConnectionClose(connection);
        wt_msquic_lock(context);
        context->connection = NULL;
        wt_msquic_unlock(context);
        return WT_STATUS_TRANSPORT_ERROR;
    }

    return WT_STATUS_OK;
#else
    wt_http3_peer_settings settings = { 1, 1, 1 };
    wt_status settings_status = wt_http3_validate_peer_settings(&settings);
    if (settings_status != WT_STATUS_OK) {
        return settings_status;
    }

    return WT_STATUS_UNSUPPORTED;
#endif
}

void wt_msquic_context_destroy(wt_msquic_context* context)
{
    if (context == NULL) {
        return;
    }

    wt_msquic_shutdown(context, 0);

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    const QUIC_API_TABLE* api = context->api;
    size_t stream_count = 0;
    HQUIC* streams = wt_msquic_copy_stream_handles_locked(context, &stream_count);
    HQUIC request_stream = context->request_stream;
    HQUIC local_control_stream = context->local_control_stream;
    HQUIC connection = context->connection;
    HQUIC configuration = context->configuration;
    HQUIC registration = context->registration;
    context->request_stream = NULL;
    context->local_control_stream = NULL;
    context->connection = NULL;
    context->configuration = NULL;
    context->registration = NULL;

    wt_msquic_stream_entry* current = context->streams;
    while (current != NULL) {
        wt_msquic_stream_entry* next = current->next;
        free(current);
        current = next;
    }

    context->streams = NULL;
    context->api = NULL;
    wt_msquic_unlock(context);

    for (size_t i = 0; i < stream_count; i++) {
        api->StreamClose(streams[i]);
    }
    free(streams);

    if (request_stream != NULL) {
        api->StreamClose(request_stream);
    }

    if (local_control_stream != NULL) {
        api->StreamClose(local_control_stream);
    }

    if (connection != NULL) {
        api->ConnectionClose(connection);
    }

    if (configuration != NULL) {
        api->ConfigurationClose(configuration);
    }

    if (registration != NULL) {
        api->RegistrationClose(registration);
    }

    if (api != NULL) {
        MsQuicClose(api);
    }

    free(context->headers_utf8);
    context->headers_utf8 = NULL;
    context->headers_length = 0;
    wt_buffer_free(&context->request_stream_buffer);
    wt_msquic_lock_destroy(context);
#endif

    free(context);
}

#if defined(WT_HAS_MSQUIC)
#ifdef _WIN32
static unsigned __stdcall wt_msquic_destroy_thread_proc(void* arg)
{
    wt_msquic_context_destroy((wt_msquic_context*)arg);
    return 0;
}
#else
static void* wt_msquic_destroy_thread_proc(void* arg)
{
    wt_msquic_context_destroy((wt_msquic_context*)arg);
    return NULL;
}
#endif
#endif

void wt_msquic_context_destroy_detached(wt_msquic_context* context)
{
    if (context == NULL) {
        return;
    }

#if defined(WT_HAS_MSQUIC)
    // MsQuicClose/RegistrationClose block until the MsQuic worker threads drain.
    // During process/plugin teardown (e.g. Unity quit) that join can deadlock,
    // so run the blocking teardown on a detached thread and let the caller
    // return immediately. If the thread cannot be started, fall back to a
    // synchronous destroy so resources are still released.
#ifdef _WIN32
    uintptr_t thread = _beginthreadex(NULL, 0, wt_msquic_destroy_thread_proc, context, 0, NULL);
    if (thread == 0) {
        wt_msquic_context_destroy(context);
    } else {
        CloseHandle((HANDLE)thread);
    }
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, wt_msquic_destroy_thread_proc, context) == 0) {
        pthread_detach(thread);
    } else {
        wt_msquic_context_destroy(context);
    }
#endif
#else
    wt_msquic_context_destroy(context);
#endif
}

#if defined(WT_ENABLE_TEST_HOOKS)
void wt_msquic_test_set_wire_session_id(wt_msquic_context* context, uint64_t wire_session_id)
{
    if (context == NULL) {
        return;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    context->connect_stream_id = wire_session_id;
    context->datagram_flow_id = wire_session_id / 4;
    context->connect_stream_id_ready = 1;
    wt_msquic_unlock(context);
#else
    (void)wire_session_id;
#endif
}

void wt_msquic_test_set_connect_accepted(wt_msquic_context* context)
{
    if (context == NULL) {
        return;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    context->connect_accepted = 1;
    wt_msquic_unlock(context);
    wt_msquic_try_emit_session_connected(context);
#endif
}

void wt_msquic_test_set_connect_stream_id(wt_msquic_context* context, uint64_t connect_stream_id)
{
    if (context == NULL) {
        return;
    }

#if defined(WT_HAS_MSQUIC)
    wt_msquic_lock(context);
    context->connect_stream_id = connect_stream_id;
    context->datagram_flow_id = connect_stream_id / 4;
    context->connect_stream_id_ready = 1;
    wt_msquic_unlock(context);
    wt_msquic_try_emit_session_connected(context);
#else
    (void)connect_stream_id;
#endif
}

wt_status wt_msquic_test_encode_datagram(
    wt_msquic_context* context,
    uint64_t session_id,
    const uint8_t* payload,
    size_t payload_length,
    uint8_t* output,
    size_t output_length,
    size_t* bytes_written)
{
    if (output == NULL || bytes_written == NULL) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *bytes_written = 0;

#if defined(WT_HAS_MSQUIC)
    wt_buffer encoded;
    wt_buffer_init(&encoded);
    wt_status status = wt_msquic_encode_datagram_for_send(context, session_id, payload, payload_length, &encoded);
    if (status == WT_STATUS_OK) {
        if (encoded.length > output_length) {
            status = WT_STATUS_INVALID_ARGUMENT;
        } else {
            memcpy(output, encoded.data, encoded.length);
            *bytes_written = encoded.length;
        }
    }

    wt_buffer_free(&encoded);
    return status;
#else
    (void)context;
    (void)session_id;
    (void)payload;
    (void)payload_length;
    (void)output_length;
    return WT_STATUS_UNSUPPORTED;
#endif
}

wt_status wt_msquic_test_deliver_datagram(
    wt_msquic_context* context,
    const uint8_t* datagram,
    size_t datagram_length)
{
#if defined(WT_HAS_MSQUIC)
    return wt_msquic_deliver_datagram(context, datagram, datagram_length);
#else
    (void)context;
    (void)datagram;
    (void)datagram_length;
    return WT_STATUS_UNSUPPORTED;
#endif
}
#endif

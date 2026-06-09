#ifndef WEBTRANSPORT_NATIVE_H
#define WEBTRANSPORT_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WT_API __declspec(dllexport)
#else
#define WT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WT_ABI_VERSION 1u

typedef enum wt_status {
    WT_STATUS_OK = 0,
    WT_STATUS_PENDING = 1,
    WT_STATUS_NOT_FOUND = 2,
    WT_STATUS_INVALID_ARGUMENT = -1,
    WT_STATUS_INVALID_STATE = -2,
    WT_STATUS_TRANSPORT_ERROR = -3,
    WT_STATUS_PROTOCOL_ERROR = -4,
    WT_STATUS_TLS_ERROR = -5,
    WT_STATUS_CANCELLED = -6,
    WT_STATUS_OUT_OF_MEMORY = -7,
    WT_STATUS_UNSUPPORTED = -8,
    WT_STATUS_UNKNOWN = -255
} wt_status;

typedef enum wt_event_type {
    WT_EVENT_NONE = 0,
    WT_EVENT_CLIENT_CONNECTED = 1,
    WT_EVENT_CLIENT_CLOSED = 2,
    WT_EVENT_SESSION_CONNECTED = 3,
    WT_EVENT_SESSION_CLOSED = 4,
    WT_EVENT_BIDI_STREAM_OPENED = 5,
    WT_EVENT_UNI_STREAM_OPENED = 6,
    WT_EVENT_STREAM_DATA_RECEIVED = 7,
    WT_EVENT_STREAM_CLOSED = 8,
    WT_EVENT_DATAGRAM_RECEIVED = 9,
    WT_EVENT_ERROR = 10
} wt_event_type;

typedef struct wt_connect_options {
    const uint8_t* url_utf8;
    size_t url_length;
    const uint8_t* headers_utf8;
    size_t headers_length;
    uint32_t connect_timeout_milliseconds;
    uint32_t idle_timeout_milliseconds;
    uint8_t allow_untrusted_certificates;
    uint8_t enable_datagrams;
    uint16_t reserved;
} wt_connect_options;

typedef struct wt_event {
    wt_event_type type;
    wt_status status;
    uint64_t client_id;
    uint64_t session_id;
    uint64_t stream_id;
    const uint8_t* data;
    size_t data_length;
    uint64_t error_code;
} wt_event;

WT_API uint32_t wt_get_abi_version(void);
WT_API wt_status wt_client_create(uint64_t* client_id);
WT_API wt_status wt_client_connect(uint64_t client_id, const wt_connect_options* options, uint64_t* operation_id);
WT_API wt_status wt_client_shutdown(uint64_t client_id, uint64_t error_code);
WT_API wt_status wt_poll_event(uint64_t client_id, wt_event* event);
WT_API wt_status wt_session_open_bidi_stream(uint64_t session_id, uint64_t* operation_id);
WT_API wt_status wt_session_open_uni_stream(uint64_t session_id, uint64_t* operation_id);
WT_API wt_status wt_session_send_datagram(uint64_t session_id, const uint8_t* payload, size_t payload_length, uint64_t* operation_id);
WT_API wt_status wt_session_receive_datagram(uint64_t session_id, uint8_t* buffer, size_t buffer_length, size_t* bytes_read);
WT_API wt_status wt_stream_read(uint64_t stream_id, uint8_t* buffer, size_t buffer_length, size_t* bytes_read);
WT_API wt_status wt_stream_write(uint64_t stream_id, const uint8_t* payload, size_t payload_length, uint8_t end_stream, uint64_t* operation_id);
WT_API wt_status wt_stream_finish(uint64_t stream_id, uint64_t* operation_id);
WT_API wt_status wt_stream_reset(uint64_t stream_id, uint64_t error_code);
WT_API wt_status wt_session_close(uint64_t session_id, uint64_t error_code, const uint8_t* reason_utf8, size_t reason_length);
WT_API void wt_release(uint64_t handle);

#ifdef __cplusplus
}
#endif

#endif

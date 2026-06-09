#ifndef WT_MSQUIC_H
#define WT_MSQUIC_H

#include "webtransport_native.h"

typedef struct wt_msquic_context wt_msquic_context;

typedef wt_status (*wt_msquic_event_sink)(void* user_data, const wt_event* event);
typedef wt_status (*wt_msquic_stream_data_sink)(void* user_data, uint64_t stream_id, const uint8_t* data, size_t length);
typedef wt_status (*wt_msquic_datagram_sink)(void* user_data, uint64_t session_id, const uint8_t* payload, size_t payload_length);
typedef wt_status (*wt_msquic_peer_stream_sink)(void* user_data, void* stream, uint8_t bidirectional, uint64_t* stream_id);

wt_status wt_msquic_context_create(wt_msquic_context** context);
void wt_msquic_set_event_sink(
    wt_msquic_context* context,
    uint64_t client_id,
    wt_msquic_event_sink event_sink,
    void* event_sink_user_data);
void wt_msquic_set_stream_data_sink(
    wt_msquic_context* context,
    wt_msquic_stream_data_sink stream_data_sink,
    void* stream_data_sink_user_data);
void wt_msquic_set_datagram_sink(
    wt_msquic_context* context,
    wt_msquic_datagram_sink datagram_sink,
    void* datagram_sink_user_data);
void wt_msquic_set_peer_stream_sink(
    wt_msquic_context* context,
    wt_msquic_peer_stream_sink peer_stream_sink,
    void* peer_stream_sink_user_data);
void wt_msquic_set_session_id(wt_msquic_context* context, uint64_t session_id);
wt_status wt_msquic_datagram_send(wt_msquic_context* context, uint64_t session_id, const uint8_t* payload, size_t payload_length);
wt_status wt_msquic_open_bidi_stream(wt_msquic_context* context, uint64_t session_id, uint64_t stream_id);
wt_status wt_msquic_stream_write(wt_msquic_context* context, uint64_t stream_id, const uint8_t* payload, size_t payload_length, uint8_t end_stream);
wt_status wt_msquic_stream_finish(wt_msquic_context* context, uint64_t stream_id);
wt_status wt_msquic_stream_reset(wt_msquic_context* context, uint64_t stream_id, uint64_t error_code);
void wt_msquic_close_stream_handle(wt_msquic_context* context, uint64_t stream_id);
void wt_msquic_shutdown(wt_msquic_context* context, uint64_t error_code);
wt_status wt_msquic_connect(
    wt_msquic_context* context,
    const wt_connect_options* options);
void wt_msquic_context_destroy(wt_msquic_context* context);
void wt_msquic_context_destroy_detached(wt_msquic_context* context);

#if defined(WT_ENABLE_TEST_HOOKS)
void wt_msquic_test_set_wire_session_id(wt_msquic_context* context, uint64_t wire_session_id);
void wt_msquic_test_set_connect_accepted(wt_msquic_context* context);
void wt_msquic_test_set_connect_stream_id(wt_msquic_context* context, uint64_t connect_stream_id);
wt_status wt_msquic_test_encode_datagram(
    wt_msquic_context* context,
    uint64_t session_id,
    const uint8_t* payload,
    size_t payload_length,
    uint8_t* output,
    size_t output_length,
    size_t* bytes_written);
wt_status wt_msquic_test_deliver_datagram(
    wt_msquic_context* context,
    const uint8_t* datagram,
    size_t datagram_length);
#endif

#endif

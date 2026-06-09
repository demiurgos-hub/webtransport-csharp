#ifndef WT_HTTP3_H
#define WT_HTTP3_H

#include "webtransport_native.h"

#define WT_H3_SETTING_ENABLE_CONNECT_PROTOCOL 0x08u
#define WT_H3_SETTING_H3_DATAGRAM 0x33u
#define WT_H3_SETTING_WEBTRANSPORT_DRAFT 0x2b603742u
#define WT_H3_WEBTRANSPORT_STREAM_FRAME_TYPE 0x41u
#define WT_H3_STREAM_TYPE_CONTROL 0x00u
#define WT_H3_FRAME_DATA 0x00u
#define WT_H3_FRAME_HEADERS 0x01u
#define WT_H3_FRAME_SETTINGS 0x04u

typedef struct wt_http3_session_request {
    const uint8_t* authority;
    size_t authority_length;
    const uint8_t* path;
    size_t path_length;
    const uint8_t* headers;
    size_t headers_length;
} wt_http3_session_request;

typedef struct wt_http3_peer_settings {
    uint8_t enable_connect_protocol;
    uint8_t enable_datagram;
    uint8_t enable_webtransport;
} wt_http3_peer_settings;

typedef struct wt_buffer {
    uint8_t* data;
    size_t length;
    size_t capacity;
} wt_buffer;

void wt_buffer_init(wt_buffer* buffer);
void wt_buffer_free(wt_buffer* buffer);
wt_status wt_buffer_append(wt_buffer* buffer, const uint8_t* data, size_t length);
wt_status wt_http3_encode_varint(uint64_t value, wt_buffer* output);
wt_status wt_http3_decode_varint(const uint8_t* data, size_t length, size_t* offset, uint64_t* value);
wt_status wt_http3_encode_frame_header(uint64_t frame_type, uint64_t payload_length, wt_buffer* output);
wt_status wt_http3_encode_client_settings(uint8_t enable_datagrams, wt_buffer* output);
wt_status wt_http3_parse_settings(const uint8_t* data, size_t length, wt_http3_peer_settings* settings);
wt_status wt_http3_validate_peer_settings(const wt_http3_peer_settings* settings);
wt_status wt_http3_encode_webtransport_connect(const wt_http3_session_request* request, wt_buffer* output);
wt_status wt_http3_prepare_webtransport_connect(const wt_http3_session_request* request);

#endif

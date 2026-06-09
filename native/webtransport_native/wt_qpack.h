#ifndef WT_QPACK_H
#define WT_QPACK_H

#include "wt_http3.h"

typedef struct wt_qpack_header {
    const uint8_t* name;
    size_t name_length;
    const uint8_t* value;
    size_t value_length;
} wt_qpack_header;

wt_status wt_qpack_encode_headers(const wt_qpack_header* headers, size_t header_count, wt_buffer* output);
wt_status wt_qpack_encode_header_field(const wt_qpack_header* header, wt_buffer* output);
wt_status wt_qpack_encode_webtransport_request_headers(const wt_qpack_header* headers, size_t header_count, wt_buffer* output);
wt_status wt_qpack_decode_status(const uint8_t* data, size_t length, uint16_t* status_code);

#endif

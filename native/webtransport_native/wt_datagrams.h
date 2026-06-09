#ifndef WT_DATAGRAMS_H
#define WT_DATAGRAMS_H

#include "webtransport_native.h"
#include "wt_http3.h"

wt_status wt_datagrams_encode(uint64_t session_id, const uint8_t* payload, size_t payload_length, wt_buffer* output);
wt_status wt_datagrams_decode(const uint8_t* datagram, size_t datagram_length, uint64_t* session_id, const uint8_t** payload, size_t* payload_length);
wt_status wt_datagrams_send(uint64_t session_id, const uint8_t* payload, size_t payload_length, uint64_t* operation_id);

#endif

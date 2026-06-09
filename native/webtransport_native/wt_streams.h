#ifndef WT_STREAMS_H
#define WT_STREAMS_H

#include "webtransport_native.h"

wt_status wt_streams_open_bidi(uint64_t session_id, uint64_t* operation_id);
wt_status wt_streams_open_uni(uint64_t session_id, uint64_t* operation_id);
wt_status wt_streams_read(uint64_t stream_id, uint8_t* buffer, size_t buffer_length, size_t* bytes_read);
wt_status wt_streams_write(uint64_t stream_id, const uint8_t* payload, size_t payload_length, uint8_t end_stream, uint64_t* operation_id);
wt_status wt_streams_finish(uint64_t stream_id, uint64_t* operation_id);
wt_status wt_streams_reset(uint64_t stream_id, uint64_t error_code);

#endif

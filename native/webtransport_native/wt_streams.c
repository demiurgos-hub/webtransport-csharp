#include "wt_streams.h"

wt_status wt_streams_open_bidi(uint64_t session_id, uint64_t* operation_id)
{
    if (session_id == 0 || operation_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *operation_id = 0;
    return WT_STATUS_UNSUPPORTED;
}

wt_status wt_streams_open_uni(uint64_t session_id, uint64_t* operation_id)
{
    if (session_id == 0 || operation_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *operation_id = 0;
    return WT_STATUS_UNSUPPORTED;
}

wt_status wt_streams_read(uint64_t stream_id, uint8_t* buffer, size_t buffer_length, size_t* bytes_read)
{
    if (stream_id == 0 || bytes_read == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (buffer == 0 && buffer_length != 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *bytes_read = 0;
    return WT_STATUS_NOT_FOUND;
}

wt_status wt_streams_write(uint64_t stream_id, const uint8_t* payload, size_t payload_length, uint8_t end_stream, uint64_t* operation_id)
{
    (void)end_stream;

    if (stream_id == 0 || operation_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (payload == 0 && payload_length != 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *operation_id = 0;
    return WT_STATUS_UNSUPPORTED;
}

wt_status wt_streams_finish(uint64_t stream_id, uint64_t* operation_id)
{
    if (stream_id == 0 || operation_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *operation_id = 0;
    return WT_STATUS_UNSUPPORTED;
}

wt_status wt_streams_reset(uint64_t stream_id, uint64_t error_code)
{
    (void)error_code;

    if (stream_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    return WT_STATUS_UNSUPPORTED;
}

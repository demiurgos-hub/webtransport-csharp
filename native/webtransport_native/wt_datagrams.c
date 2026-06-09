#include "wt_datagrams.h"

wt_status wt_datagrams_encode(uint64_t session_id, const uint8_t* payload, size_t payload_length, wt_buffer* output)
{
    if (output == 0 || (payload == 0 && payload_length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_status status = wt_http3_encode_varint(session_id, output);
    if (status != WT_STATUS_OK) {
        return status;
    }

    return wt_buffer_append(output, payload, payload_length);
}

wt_status wt_datagrams_decode(const uint8_t* datagram, size_t datagram_length, uint64_t* session_id, const uint8_t** payload, size_t* payload_length)
{
    if (datagram == 0 || datagram_length == 0 || session_id == 0 || payload == 0 || payload_length == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    size_t offset = 0;
    wt_status status = wt_http3_decode_varint(datagram, datagram_length, &offset, session_id);
    if (status != WT_STATUS_OK) {
        return status;
    }

    *payload = datagram + offset;
    *payload_length = datagram_length - offset;
    return WT_STATUS_OK;
}

wt_status wt_datagrams_send(uint64_t session_id, const uint8_t* payload, size_t payload_length, uint64_t* operation_id)
{
    if (session_id == 0 || operation_id == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (payload == 0 && payload_length != 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *operation_id = 0;
    return WT_STATUS_UNSUPPORTED;
}

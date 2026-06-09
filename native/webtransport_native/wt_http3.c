#include "wt_http3.h"
#include "wt_qpack.h"

#include <stdlib.h>
#include <string.h>

void wt_buffer_init(wt_buffer* buffer)
{
    if (buffer == 0) {
        return;
    }

    buffer->data = 0;
    buffer->length = 0;
    buffer->capacity = 0;
}

void wt_buffer_free(wt_buffer* buffer)
{
    if (buffer == 0) {
        return;
    }

    free(buffer->data);
    buffer->data = 0;
    buffer->length = 0;
    buffer->capacity = 0;
}

wt_status wt_buffer_append(wt_buffer* buffer, const uint8_t* data, size_t length)
{
    if (buffer == 0 || (data == 0 && length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (length == 0) {
        return WT_STATUS_OK;
    }

    size_t required = buffer->length + length;
    if (required < buffer->length) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (required > buffer->capacity) {
        size_t next_capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (next_capacity < required) {
            next_capacity *= 2;
        }

        uint8_t* next = (uint8_t*)realloc(buffer->data, next_capacity);
        if (next == 0) {
            return WT_STATUS_OUT_OF_MEMORY;
        }

        buffer->data = next;
        buffer->capacity = next_capacity;
    }

    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return WT_STATUS_OK;
}

wt_status wt_http3_encode_varint(uint64_t value, wt_buffer* output)
{
    uint8_t encoded[8];
    size_t length;

    if (output == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (value <= 63u) {
        encoded[0] = (uint8_t)value;
        length = 1;
    } else if (value <= 16383u) {
        encoded[0] = (uint8_t)(0x40u | ((value >> 8) & 0x3fu));
        encoded[1] = (uint8_t)(value & 0xffu);
        length = 2;
    } else if (value <= 1073741823u) {
        encoded[0] = (uint8_t)(0x80u | ((value >> 24) & 0x3fu));
        encoded[1] = (uint8_t)((value >> 16) & 0xffu);
        encoded[2] = (uint8_t)((value >> 8) & 0xffu);
        encoded[3] = (uint8_t)(value & 0xffu);
        length = 4;
    } else if (value <= 4611686018427387903ull) {
        encoded[0] = (uint8_t)(0xc0u | ((value >> 56) & 0x3fu));
        encoded[1] = (uint8_t)((value >> 48) & 0xffu);
        encoded[2] = (uint8_t)((value >> 40) & 0xffu);
        encoded[3] = (uint8_t)((value >> 32) & 0xffu);
        encoded[4] = (uint8_t)((value >> 24) & 0xffu);
        encoded[5] = (uint8_t)((value >> 16) & 0xffu);
        encoded[6] = (uint8_t)((value >> 8) & 0xffu);
        encoded[7] = (uint8_t)(value & 0xffu);
        length = 8;
    } else {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    return wt_buffer_append(output, encoded, length);
}

wt_status wt_http3_decode_varint(const uint8_t* data, size_t length, size_t* offset, uint64_t* value)
{
    if (data == 0 || offset == 0 || value == 0 || *offset >= length) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint8_t first = data[*offset];
    uint8_t prefix = first >> 6;
    size_t encoded_length = (size_t)1u << prefix;
    if (*offset + encoded_length > length) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint64_t decoded = first & 0x3fu;
    for (size_t i = 1; i < encoded_length; i++) {
        decoded = (decoded << 8) | data[*offset + i];
    }

    *offset += encoded_length;
    *value = decoded;
    return WT_STATUS_OK;
}

wt_status wt_http3_encode_frame_header(uint64_t frame_type, uint64_t payload_length, wt_buffer* output)
{
    wt_status status = wt_http3_encode_varint(frame_type, output);
    if (status != WT_STATUS_OK) {
        return status;
    }

    return wt_http3_encode_varint(payload_length, output);
}

static wt_status wt_http3_append_setting(wt_buffer* payload, uint64_t id, uint64_t value)
{
    wt_status status = wt_http3_encode_varint(id, payload);
    if (status != WT_STATUS_OK) {
        return status;
    }

    return wt_http3_encode_varint(value, payload);
}

static wt_status wt_http3_append_serialized_headers(
    wt_buffer* header_block,
    const uint8_t* headers,
    size_t headers_length)
{
    if (header_block == 0 || (headers == 0 && headers_length != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    size_t offset = 0;
    while (offset < headers_length) {
        size_t line_start = offset;
        while (offset < headers_length && headers[offset] != '\n') {
            offset++;
        }

        size_t line_length = offset - line_start;
        if (offset < headers_length && headers[offset] == '\n') {
            offset++;
        }

        if (line_length == 0) {
            continue;
        }

        size_t separator = line_start;
        while (separator < line_start + line_length && headers[separator] != ':') {
            separator++;
        }

        if (separator == line_start || separator >= line_start + line_length) {
            return WT_STATUS_INVALID_ARGUMENT;
        }

        size_t value_start = separator + 1;
        if (value_start < line_start + line_length && headers[value_start] == ' ') {
            value_start++;
        }

        wt_qpack_header header = {
            headers + line_start,
            separator - line_start,
            headers + value_start,
            line_start + line_length - value_start
        };

        wt_status status = wt_qpack_encode_header_field(&header, header_block);
        if (status != WT_STATUS_OK) {
            return status;
        }
    }

    return WT_STATUS_OK;
}

wt_status wt_http3_encode_client_settings(uint8_t enable_datagrams, wt_buffer* output)
{
    if (output == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_buffer payload;
    wt_buffer_init(&payload);

    wt_status status = wt_http3_append_setting(&payload, WT_H3_SETTING_ENABLE_CONNECT_PROTOCOL, 1);
    if (status == WT_STATUS_OK) {
        status = wt_http3_append_setting(&payload, WT_H3_SETTING_H3_DATAGRAM, enable_datagrams ? 1 : 0);
    }

    if (status == WT_STATUS_OK) {
        status = wt_http3_append_setting(&payload, WT_H3_SETTING_WEBTRANSPORT_DRAFT, 1);
    }

    if (status == WT_STATUS_OK) {
        status = wt_http3_encode_frame_header(WT_H3_FRAME_SETTINGS, payload.length, output);
    }

    if (status == WT_STATUS_OK) {
        status = wt_buffer_append(output, payload.data, payload.length);
    }

    wt_buffer_free(&payload);
    return status;
}

wt_status wt_http3_parse_settings(const uint8_t* data, size_t length, wt_http3_peer_settings* settings)
{
    if (data == 0 || settings == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    memset(settings, 0, sizeof(*settings));

    size_t offset = 0;
    uint64_t frame_type;
    uint64_t payload_length;
    wt_status status = wt_http3_decode_varint(data, length, &offset, &frame_type);
    if (status != WT_STATUS_OK) {
        return status;
    }

    status = wt_http3_decode_varint(data, length, &offset, &payload_length);
    if (status != WT_STATUS_OK) {
        return status;
    }

    if (frame_type != WT_H3_FRAME_SETTINGS || offset + payload_length > length) {
        return WT_STATUS_PROTOCOL_ERROR;
    }

    size_t end = offset + (size_t)payload_length;
    while (offset < end) {
        uint64_t id;
        uint64_t value;
        status = wt_http3_decode_varint(data, end, &offset, &id);
        if (status != WT_STATUS_OK) {
            return status;
        }

        status = wt_http3_decode_varint(data, end, &offset, &value);
        if (status != WT_STATUS_OK) {
            return status;
        }

        if (id == WT_H3_SETTING_ENABLE_CONNECT_PROTOCOL && value != 0) {
            settings->enable_connect_protocol = 1;
        } else if (id == WT_H3_SETTING_H3_DATAGRAM && value != 0) {
            settings->enable_datagram = 1;
        } else if (id == WT_H3_SETTING_WEBTRANSPORT_DRAFT && value != 0) {
            settings->enable_webtransport = 1;
        }
    }

    return WT_STATUS_OK;
}

wt_status wt_http3_validate_peer_settings(const wt_http3_peer_settings* settings)
{
    if (settings == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (!settings->enable_connect_protocol) {
        return WT_STATUS_PROTOCOL_ERROR;
    }

    if (!settings->enable_datagram) {
        return WT_STATUS_PROTOCOL_ERROR;
    }

    if (!settings->enable_webtransport) {
        return WT_STATUS_PROTOCOL_ERROR;
    }

    return WT_STATUS_OK;
}

wt_status wt_http3_prepare_webtransport_connect(const wt_http3_session_request* request)
{
    if (request == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (request->authority == 0 || request->authority_length == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (request->path == 0 || request->path_length == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    /*
     * TODO: QPACK encode:
     * :method = CONNECT
     * :protocol = webtransport
     * :scheme = https
     * :authority = request authority
     * :path = request path
     * plus caller-provided headers.
     */
    return WT_STATUS_OK;
}

wt_status wt_http3_encode_webtransport_connect(const wt_http3_session_request* request, wt_buffer* output)
{
    wt_status status = wt_http3_prepare_webtransport_connect(request);
    if (status != WT_STATUS_OK) {
        return status;
    }

    if (output == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    static const uint8_t method_name[] = ":method";
    static const uint8_t method_value[] = "CONNECT";
    static const uint8_t protocol_name[] = ":protocol";
    static const uint8_t protocol_value[] = "webtransport";
    static const uint8_t scheme_name[] = ":scheme";
    static const uint8_t scheme_value[] = "https";
    static const uint8_t authority_name[] = ":authority";
    static const uint8_t path_name[] = ":path";

    wt_qpack_header headers[] = {
        { method_name, sizeof(method_name) - 1, method_value, sizeof(method_value) - 1 },
        { protocol_name, sizeof(protocol_name) - 1, protocol_value, sizeof(protocol_value) - 1 },
        { scheme_name, sizeof(scheme_name) - 1, scheme_value, sizeof(scheme_value) - 1 },
        { authority_name, sizeof(authority_name) - 1, request->authority, request->authority_length },
        { path_name, sizeof(path_name) - 1, request->path, request->path_length }
    };

    wt_buffer header_block;
    wt_buffer_init(&header_block);
    status = wt_qpack_encode_webtransport_request_headers(headers, sizeof(headers) / sizeof(headers[0]), &header_block);
    if (status == WT_STATUS_OK && request->headers_length != 0) {
        status = wt_http3_append_serialized_headers(&header_block, request->headers, request->headers_length);
    }

    if (status == WT_STATUS_OK) {
        status = wt_http3_encode_frame_header(WT_H3_FRAME_HEADERS, header_block.length, output);
    }

    if (status == WT_STATUS_OK) {
        status = wt_buffer_append(output, header_block.data, header_block.length);
    }

    wt_buffer_free(&header_block);
    return status;
}

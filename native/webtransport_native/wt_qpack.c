#include "wt_qpack.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WT_QPACK_MAX_VARINT_SHIFT 56u

static wt_status wt_qpack_encode_prefixed_integer(uint8_t prefix_bits, uint8_t prefix_mask, uint64_t value, wt_buffer* output);

static wt_status wt_qpack_encode_string_literal(const uint8_t* value, size_t length, wt_buffer* output)
{
    if (value == 0 && length != 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_status status = wt_qpack_encode_prefixed_integer(7, 0x00, length, output);
    if (status == WT_STATUS_OK) {
        status = wt_buffer_append(output, value, length);
    }

    return status;
}

static wt_status wt_qpack_encode_prefixed_integer(uint8_t prefix_bits, uint8_t prefix_mask, uint64_t value, wt_buffer* output)
{
    if (output == 0 || prefix_bits == 0 || prefix_bits > 8) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint8_t max_prefix_value = (uint8_t)((1u << prefix_bits) - 1u);
    if (value < max_prefix_value) {
        uint8_t encoded = (uint8_t)(prefix_mask | (uint8_t)value);
        return wt_buffer_append(output, &encoded, 1);
    }

    uint8_t encoded = (uint8_t)(prefix_mask | max_prefix_value);
    wt_status status = wt_buffer_append(output, &encoded, 1);
    if (status != WT_STATUS_OK) {
        return status;
    }

    value -= max_prefix_value;
    while (value >= 128u) {
        encoded = (uint8_t)((value & 0x7fu) | 0x80u);
        status = wt_buffer_append(output, &encoded, 1);
        if (status != WT_STATUS_OK) {
            return status;
        }

        value >>= 7;
    }

    encoded = (uint8_t)value;
    return wt_buffer_append(output, &encoded, 1);
}

static wt_status wt_qpack_encode_static_index(uint64_t index, wt_buffer* output)
{
    return wt_qpack_encode_prefixed_integer(6, 0xc0, index, output);
}

static wt_status wt_qpack_encode_literal_with_static_name(uint64_t index, const uint8_t* value, size_t value_length, wt_buffer* output)
{
    wt_status status = wt_qpack_encode_prefixed_integer(4, 0x50, index, output);
    if (status != WT_STATUS_OK) {
        return status;
    }

    return wt_qpack_encode_string_literal(value, value_length, output);
}

static int wt_qpack_matches(const wt_qpack_header* header, const char* name, const char* value)
{
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    return header->name_length == name_length &&
        header->value_length == value_length &&
        memcmp(header->name, name, name_length) == 0 &&
        memcmp(header->value, value, value_length) == 0;
}

static int wt_qpack_name_matches(const wt_qpack_header* header, const char* name)
{
    size_t name_length = strlen(name);
    return header->name_length == name_length && memcmp(header->name, name, name_length) == 0;
}

wt_status wt_qpack_encode_headers(const wt_qpack_header* headers, size_t header_count, wt_buffer* output)
{
    if (output == 0 || (headers == 0 && header_count != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint8_t prefix[] = { 0x00, 0x00 };
    wt_status status = wt_buffer_append(output, prefix, sizeof(prefix));
    if (status != WT_STATUS_OK) {
        return status;
    }

    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name == 0 || headers[i].name_length == 0 || headers[i].value == 0) {
            return WT_STATUS_INVALID_ARGUMENT;
        }

        status = wt_qpack_encode_prefixed_integer(3, 0x20, headers[i].name_length, output);
        if (status == WT_STATUS_OK) {
            status = wt_buffer_append(output, headers[i].name, headers[i].name_length);
        }

        if (status == WT_STATUS_OK) {
            status = wt_qpack_encode_string_literal(headers[i].value, headers[i].value_length, output);
        }

        if (status != WT_STATUS_OK) {
            return status;
        }
    }

    return WT_STATUS_OK;
}

wt_status wt_qpack_encode_header_field(const wt_qpack_header* header, wt_buffer* output)
{
    if (header == 0 || output == 0 || header->name == 0 || header->name_length == 0 || header->value == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    wt_status status = wt_qpack_encode_prefixed_integer(3, 0x20, header->name_length, output);
    if (status == WT_STATUS_OK) {
        status = wt_buffer_append(output, header->name, header->name_length);
    }

    if (status == WT_STATUS_OK) {
        status = wt_qpack_encode_string_literal(header->value, header->value_length, output);
    }

    return status;
}

wt_status wt_qpack_encode_webtransport_request_headers(const wt_qpack_header* headers, size_t header_count, wt_buffer* output)
{
    if (output == 0 || (headers == 0 && header_count != 0)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    uint8_t prefix[] = { 0x00, 0x00 };
    wt_status status = wt_buffer_append(output, prefix, sizeof(prefix));
    if (status != WT_STATUS_OK) {
        return status;
    }

    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name == 0 || headers[i].name_length == 0 || headers[i].value == 0) {
            return WT_STATUS_INVALID_ARGUMENT;
        }

        if (wt_qpack_matches(&headers[i], ":method", "CONNECT")) {
            status = wt_qpack_encode_static_index(15, output);
        } else if (wt_qpack_matches(&headers[i], ":scheme", "https")) {
            status = wt_qpack_encode_static_index(23, output);
        } else if (wt_qpack_name_matches(&headers[i], ":authority")) {
            status = wt_qpack_encode_literal_with_static_name(0, headers[i].value, headers[i].value_length, output);
        } else if (wt_qpack_name_matches(&headers[i], ":path")) {
            status = wt_qpack_encode_literal_with_static_name(1, headers[i].value, headers[i].value_length, output);
        } else {
            status = wt_qpack_encode_prefixed_integer(3, 0x20, headers[i].name_length, output);
            if (status == WT_STATUS_OK) {
                status = wt_buffer_append(output, headers[i].name, headers[i].name_length);
            }

            if (status == WT_STATUS_OK) {
                status = wt_qpack_encode_string_literal(headers[i].value, headers[i].value_length, output);
            }
        }

        if (status != WT_STATUS_OK) {
            return status;
        }
    }

    return WT_STATUS_OK;
}

static uint16_t wt_qpack_static_status(uint8_t index)
{
    switch (index) {
        case 25:
            return 200;
        case 26:
            return 204;
        case 27:
            return 206;
        case 28:
            return 304;
        case 29:
            return 400;
        case 30:
            return 404;
        case 31:
            return 500;
        default:
            return 0;
    }
}

wt_status wt_qpack_decode_status(const uint8_t* data, size_t length, uint16_t* status_code)
{
    if (data == 0 || status_code == 0 || length < 2) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    *status_code = 0;
    size_t offset = 2;

    while (offset < length) {
        uint8_t first = data[offset++];
        if ((first & 0xc0u) == 0xc0u) {
            uint8_t index = first & 0x3fu;
            uint16_t status = wt_qpack_static_status(index);
            if (status != 0) {
                *status_code = status;
                return WT_STATUS_OK;
            }
        } else if ((first & 0xc0u) == 0x40u) {
            uint64_t index = first & 0x0fu;
            if (index == 0x0fu) {
                size_t shift = 0;
                uint8_t next;
                do {
                    if (offset >= length) {
                        return WT_STATUS_PROTOCOL_ERROR;
                    }

                    if (shift > WT_QPACK_MAX_VARINT_SHIFT) {
                        return WT_STATUS_PROTOCOL_ERROR;
                    }

                    next = data[offset++];
                    if ((next & 0x7fu) > UINT64_MAX >> shift) {
                        return WT_STATUS_PROTOCOL_ERROR;
                    }

                    index += (uint64_t)(next & 0x7fu) << shift;
                    shift += 7;
                } while ((next & 0x80u) != 0);
            }

            if (offset >= length) {
                return WT_STATUS_PROTOCOL_ERROR;
            }

            size_t value_length = data[offset++] & 0x7fu;
            if (offset > length || value_length > length - offset) {
                return WT_STATUS_PROTOCOL_ERROR;
            }

            const uint8_t* value = data + offset;
            offset += value_length;

            if ((index == 24 || index == 25 || index == 63 || index == 64 || index == 65 || index == 67 || index == 71) && value_length == 3) {
                if (value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9') {
                    return WT_STATUS_PROTOCOL_ERROR;
                }

                *status_code = (uint16_t)(((value[0] - '0') * 100) + ((value[1] - '0') * 10) + (value[2] - '0'));
                return WT_STATUS_OK;
            }
        } else if ((first & 0xe0u) == 0x20u) {
            if (offset >= length) {
                return WT_STATUS_PROTOCOL_ERROR;
            }

            size_t name_length = first & 0x07u;
            if (name_length == 0x07u) {
                size_t shift = 0;
                uint8_t next;
                do {
                    if (offset >= length) {
                        return WT_STATUS_PROTOCOL_ERROR;
                    }

                    if (shift > WT_QPACK_MAX_VARINT_SHIFT) {
                        return WT_STATUS_PROTOCOL_ERROR;
                    }

                    next = data[offset++];
                    if ((size_t)(next & 0x7fu) > SIZE_MAX >> shift) {
                        return WT_STATUS_PROTOCOL_ERROR;
                    }

                    name_length += (size_t)(next & 0x7fu) << shift;
                    shift += 7;
                } while ((next & 0x80u) != 0);
            }

            if (offset > length || name_length > length - offset) {
                return WT_STATUS_PROTOCOL_ERROR;
            }

            const uint8_t* name = data + offset;
            offset += name_length;

            if (offset >= length) {
                return WT_STATUS_PROTOCOL_ERROR;
            }

            size_t value_length = data[offset++] & 0x7fu;
            if (offset > length || value_length > length - offset) {
                return WT_STATUS_PROTOCOL_ERROR;
            }

            const uint8_t* value = data + offset;
            offset += value_length;

            if (name_length == 7 && memcmp(name, ":status", 7) == 0 && value_length == 3) {
                if (value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9') {
                    return WT_STATUS_PROTOCOL_ERROR;
                }

                *status_code = (uint16_t)(((value[0] - '0') * 100) + ((value[1] - '0') * 10) + (value[2] - '0'));
                return WT_STATUS_OK;
            }
        } else {
            return WT_STATUS_UNSUPPORTED;
        }
    }

    return WT_STATUS_NOT_FOUND;
}

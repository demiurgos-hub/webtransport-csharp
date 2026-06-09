#include "wt_url.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int wt_starts_with_https(const uint8_t* value, size_t length)
{
    static const char prefix[] = "https://";
    return length >= sizeof(prefix) - 1 && memcmp(value, prefix, sizeof(prefix) - 1) == 0;
}

wt_status wt_url_parse_https(const uint8_t* url_utf8, size_t url_length, wt_url* parsed)
{
    if (url_utf8 == 0 || url_length == 0 || parsed == 0) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    if (!wt_starts_with_https(url_utf8, url_length)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    memset(parsed, 0, sizeof(*parsed));
    parsed->port = 443;

    size_t authority_start = 8;
    size_t authority_end = authority_start;
    while (authority_end < url_length && url_utf8[authority_end] != '/') {
        authority_end++;
    }

    if (authority_end == authority_start) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    size_t host_end = authority_end;
    size_t port_start = 0;
    for (size_t i = authority_start; i < authority_end; i++) {
        if (url_utf8[i] == ':') {
            host_end = i;
            port_start = i + 1;
            break;
        }
    }

    size_t host_length = host_end - authority_start;
    if (host_length == 0 || host_length >= sizeof(parsed->host)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    memcpy(parsed->host, url_utf8 + authority_start, host_length);
    parsed->host[host_length] = 0;

    if (port_start != 0) {
        if (port_start >= authority_end) {
            return WT_STATUS_INVALID_ARGUMENT;
        }

        uint32_t port = 0;
        for (size_t i = port_start; i < authority_end; i++) {
            if (!isdigit(url_utf8[i])) {
                return WT_STATUS_INVALID_ARGUMENT;
            }

            port = (port * 10u) + (uint32_t)(url_utf8[i] - '0');
            if (port > 65535u) {
                return WT_STATUS_INVALID_ARGUMENT;
            }
        }

        if (port == 0) {
            return WT_STATUS_INVALID_ARGUMENT;
        }

        parsed->port = (uint16_t)port;
    }

    if (authority_end == url_length) {
        parsed->path[0] = '/';
        parsed->path[1] = 0;
        return WT_STATUS_OK;
    }

    size_t path_length = url_length - authority_end;
    if (path_length == 0 || path_length >= sizeof(parsed->path)) {
        return WT_STATUS_INVALID_ARGUMENT;
    }

    memcpy(parsed->path, url_utf8 + authority_end, path_length);
    parsed->path[path_length] = 0;
    return WT_STATUS_OK;
}

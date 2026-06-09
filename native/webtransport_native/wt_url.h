#ifndef WT_URL_H
#define WT_URL_H

#include "webtransport_native.h"

#include <stdint.h>

typedef struct wt_url {
    char host[256];
    char path[1024];
    uint16_t port;
} wt_url;

wt_status wt_url_parse_https(const uint8_t* url_utf8, size_t url_length, wt_url* parsed);

#endif

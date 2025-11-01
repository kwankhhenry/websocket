#ifndef INTERNAL_H
#define INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Internal types and helpers (no dynamic allocations in hot path)

#define WS_MAX_HEADER_SIZE 4096
#define WS_HTTP_UPGRADE_HEADER_SIZE 512

// Precomputed HTTP upgrade headers (avoid runtime string construction)
extern const char* const ws_http_upgrade_template;

#endif // INTERNAL_H


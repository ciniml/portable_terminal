/*
 * Streaming SAX-style JSON parser for Tailscale MapResponse and similar.
 *
 * Walks an in-memory JSON document once and emits events (object/array
 * start/end, keys, primitive values). Builds no tree, so memory usage is
 * O(depth) plus the input buffer itself. Designed for the constrained-RAM
 * ESP32 environment where Tailscale's ~28 KB MapResponse JSON exhausts
 * cJSON's heap allocations.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TS_JS_MAX_DEPTH 32

typedef enum {
    TS_JS_NONE = 0,
    TS_JS_OBJ_START,    /* `{` */
    TS_JS_OBJ_END,      /* `}` */
    TS_JS_ARR_START,    /* `[` */
    TS_JS_ARR_END,      /* `]` */
    TS_JS_KEY,          /* member name in an object (string, no quotes) */
    TS_JS_STRING,       /* string value */
    TS_JS_NUMBER,       /* number value (raw text) */
    TS_JS_BOOL,         /* see ts_js_t.bool_value */
    TS_JS_NULL,
    TS_JS_END,          /* input fully consumed */
    TS_JS_ERROR,        /* malformed input */
} ts_js_evt_t;

typedef struct {
    const char *p;
    const char *end;

    /* Filled when the most recent ts_js_next() returned KEY/STRING/NUMBER:
     * `tok` points into the original input buffer (not NUL-terminated),
     * `tok_len` is the byte length excluding surrounding quotes for strings.
     * Escape sequences are NOT decoded — use ts_js_str_copy() for an
     * unescaped C string. */
    const char *tok;
    size_t      tok_len;

    /* Set when the most recent event is TS_JS_BOOL. */
    bool        bool_value;

    /* Internal scope tracking. */
    char    scope[TS_JS_MAX_DEPTH];   /* '{' or '[' for each open container */
    uint8_t depth;
    bool    expect_key;
} ts_js_t;

void ts_js_init(ts_js_t *j, const char *data, size_t len);

/* Read the next event. Returns TS_JS_END at end of input,
 * TS_JS_ERROR on malformed input. */
ts_js_evt_t ts_js_next(ts_js_t *j);

/* Skip the value about to be parsed (or, when called after receiving KEY,
 * skip the corresponding value). For containers, consumes through the
 * matching close brace/bracket. Returns the last event consumed (or
 * TS_JS_ERROR / TS_JS_END). */
ts_js_evt_t ts_js_skip(ts_js_t *j);

/* True if the most recent KEY/STRING token equals `literal` byte-for-byte
 * (no escape decoding). */
bool ts_js_str_eq(const ts_js_t *j, const char *literal);

/* Copy the most recent KEY/STRING/NUMBER token into `dst` as a C string,
 * decoding the most common escape sequences (\\, \", \/, \n, \r, \t).
 * Returns true if it fit, false if truncated. */
bool ts_js_str_copy(const ts_js_t *j, char *dst, size_t dst_size);

/* Parse the most recent NUMBER token as a 64-bit signed integer. */
int64_t ts_js_int64(const ts_js_t *j);

/* Parse the most recent NUMBER token as an int (for region IDs etc.). */
int ts_js_int(const ts_js_t *j);

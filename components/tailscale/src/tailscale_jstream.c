/*
 * Streaming SAX-style JSON parser implementation.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_jstream.h"

#include <stdlib.h>
#include <string.h>

void ts_js_init(ts_js_t *j, const char *data, size_t len)
{
    memset(j, 0, sizeof(*j));
    j->p   = data;
    j->end = data + len;
}

static void skip_ws(ts_js_t *j)
{
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
        else break;
    }
}

static ts_js_evt_t parse_string(ts_js_t *j, ts_js_evt_t evt)
{
    /* Caller has already consumed the opening quote. tok points at the
     * first byte of the string body. */
    j->tok = j->p;
    while (j->p < j->end) {
        char c = *j->p;
        if (c == '"') break;
        if (c == '\\') {
            if (j->p + 1 >= j->end) return TS_JS_ERROR;
            j->p += 2;   /* skip "\X" — \uXXXX consumes 2 here, then 4 hex */
            if (j->p > j->end) return TS_JS_ERROR;
        } else {
            j->p++;
        }
    }
    if (j->p >= j->end) return TS_JS_ERROR;
    j->tok_len = (size_t)(j->p - j->tok);
    j->p++;  /* closing quote */
    return evt;
}

static ts_js_evt_t parse_number(ts_js_t *j)
{
    j->tok = j->p;
    if (j->p < j->end && (*j->p == '-' || *j->p == '+')) j->p++;
    while (j->p < j->end) {
        char c = *j->p;
        if ((c >= '0' && c <= '9') ||
            c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            j->p++;
        } else break;
    }
    j->tok_len = (size_t)(j->p - j->tok);
    return j->tok_len > 0 ? TS_JS_NUMBER : TS_JS_ERROR;
}

static ts_js_evt_t parse_literal(ts_js_t *j, const char *lit, size_t lit_len,
                                 ts_js_evt_t evt, bool bool_val)
{
    if ((size_t)(j->end - j->p) < lit_len)        return TS_JS_ERROR;
    if (memcmp(j->p, lit, lit_len) != 0)          return TS_JS_ERROR;
    j->p += lit_len;
    j->bool_value = bool_val;
    return evt;
}

ts_js_evt_t ts_js_next(ts_js_t *j)
{
    skip_ws(j);

    /* Consume separators between elements (`,` between members or items,
     * `:` between key and value). Looping handles repeated whitespace. */
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ',') {
            j->p++;
            if (j->depth > 0 && j->scope[j->depth - 1] == '{')
                j->expect_key = true;
            skip_ws(j);
            continue;
        }
        if (c == ':') {
            j->p++;
            skip_ws(j);
            continue;
        }
        break;
    }

    if (j->p >= j->end) return TS_JS_END;

    char c = *j->p;

    if (c == '{') {
        j->p++;
        if (j->depth >= TS_JS_MAX_DEPTH) return TS_JS_ERROR;
        j->scope[j->depth++] = '{';
        j->expect_key = true;
        return TS_JS_OBJ_START;
    }
    if (c == '}') {
        j->p++;
        if (j->depth == 0) return TS_JS_ERROR;
        j->depth--;
        j->expect_key = (j->depth > 0 && j->scope[j->depth - 1] == '{');
        return TS_JS_OBJ_END;
    }
    if (c == '[') {
        j->p++;
        if (j->depth >= TS_JS_MAX_DEPTH) return TS_JS_ERROR;
        j->scope[j->depth++] = '[';
        j->expect_key = false;
        return TS_JS_ARR_START;
    }
    if (c == ']') {
        j->p++;
        if (j->depth == 0) return TS_JS_ERROR;
        j->depth--;
        j->expect_key = (j->depth > 0 && j->scope[j->depth - 1] == '{');
        return TS_JS_ARR_END;
    }

    ts_js_evt_t evt;
    if (c == '"') {
        bool was_key = j->expect_key &&
                       j->depth > 0 && j->scope[j->depth - 1] == '{';
        j->p++;   /* skip opening quote */
        evt = parse_string(j, was_key ? TS_JS_KEY : TS_JS_STRING);
        if (was_key) {
            j->expect_key = false;
            /* Caller's next ts_js_next() will skip the `:` separator. */
        } else {
            /* After a value inside an object, expect a key next. */
            if (j->depth > 0 && j->scope[j->depth - 1] == '{')
                j->expect_key = true;
        }
        return evt;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        evt = parse_number(j);
    } else if (c == 't') {
        evt = parse_literal(j, "true",  4, TS_JS_BOOL, true);
    } else if (c == 'f') {
        evt = parse_literal(j, "false", 5, TS_JS_BOOL, false);
    } else if (c == 'n') {
        evt = parse_literal(j, "null",  4, TS_JS_NULL, false);
    } else {
        return TS_JS_ERROR;
    }

    if (evt != TS_JS_ERROR &&
        j->depth > 0 && j->scope[j->depth - 1] == '{') {
        j->expect_key = true;
    }
    return evt;
}

ts_js_evt_t ts_js_skip(ts_js_t *j)
{
    uint8_t start_depth = j->depth;
    ts_js_evt_t e = ts_js_next(j);
    if (e == TS_JS_OBJ_START || e == TS_JS_ARR_START) {
        while (j->depth > start_depth) {
            e = ts_js_next(j);
            if (e == TS_JS_END || e == TS_JS_ERROR) return e;
        }
    }
    return e;
}

bool ts_js_str_eq(const ts_js_t *j, const char *literal)
{
    size_t llen = strlen(literal);
    if (llen != j->tok_len) return false;
    return memcmp(j->tok, literal, llen) == 0;
}

bool ts_js_str_copy(const ts_js_t *j, char *dst, size_t dst_size)
{
    if (dst_size == 0) return false;
    size_t out = 0;
    size_t cap = dst_size - 1;
    for (size_t i = 0; i < j->tok_len && out < cap; i++) {
        char c = j->tok[i];
        if (c == '\\' && i + 1 < j->tok_len) {
            char n = j->tok[++i];
            switch (n) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'u':
                    /* Skip 4 hex digits — we don't decode unicode escapes;
                     * the consumers of this parser (IPs, hex keys, hostnames)
                     * never contain non-ASCII in practice. */
                    if (i + 4 < j->tok_len) i += 4;
                    c = '?';
                    break;
                default:   c = n;    break;
            }
        }
        dst[out++] = c;
    }
    dst[out] = '\0';
    return out == j->tok_len; /* fit if we copied all input bytes */
}

int64_t ts_js_int64(const ts_js_t *j)
{
    char buf[32];
    size_t n = j->tok_len < sizeof(buf) - 1 ? j->tok_len : sizeof(buf) - 1;
    memcpy(buf, j->tok, n);
    buf[n] = '\0';
    return (int64_t)strtoll(buf, NULL, 10);
}

int ts_js_int(const ts_js_t *j)
{
    return (int)ts_js_int64(j);
}

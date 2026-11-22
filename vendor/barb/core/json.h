// JSON for the core: a pull reader, a few lookups, and a writer.
//
// The reader hands out one token at a time and checks the grammar as it
// goes (RFC 8259: no trailing commas, no leading zeros, no raw control
// characters in strings, escapes must be valid) and refuses nesting past
// JSON_MAX_DEPTH, so a hostile or broken reply cannot overflow anything.
// It does not build a tree and does not allocate. String and key tokens
// are given still escaped; json_unescape turns one into real text.
// UTF-8 is not checked on the way in; the writer makes sure what goes
// out is valid (see jw_str), because the API rejects a request with a bad
// byte in it for good, and one binary file read by a tool would otherwise
// poison every later request of the conversation.
#ifndef BARB_JSON_H
#define BARB_JSON_H

#include "str.h"

#define JSON_MAX_DEPTH 64

typedef enum {
    JT_ERR = 0,   // the input is not valid JSON; see j->err
    JT_END,       // the one top-level value is complete and nothing follows
    JT_OBJ, JT_OBJ_END,
    JT_ARR, JT_ARR_END,
    JT_KEY,       // an object key; j->tok is its text, escaped, without quotes
    JT_STR,       // a string value; same
    JT_NUM,       // a number; j->tok is its text
    JT_TRUE, JT_FALSE, JT_NULL
} jtok_t;

typedef struct {
    str_t src;
    size_t pos;
    str_t tok;
    size_t tok_start;  // offset of the first byte of the current token (the quote, for a string)
    int depth;
    uint8_t is_arr[JSON_MAX_DEPTH];
    uint8_t mode;
    const char* err;
    size_t err_pos;
} json_t;

void json_open(json_t* j, str_t src);
jtok_t json_next(json_t* j);

// Having just read the first token of a value (JT_OBJ, JT_ARR or a scalar),
// skip the rest of it. 0 if the input turned out to be invalid.
int json_skip(json_t* j, jtok_t first);

// The same, giving the value's exact text (a string's includes its quotes).
int json_capture(json_t* j, jtok_t first, str_t* out);

// In the object `obj`, the raw text of the value under `key` (first match wins).
// 0 if there is no such key, or `obj` is not a valid object.
int json_get(str_t obj, const char* key, str_t* val);

// Decode the text of a string token (no quotes) into an arena string. Every
// escape is decoded, \u surrogate pairs into one character and a lone
// surrogate into U+FFFD. {0,0} on out of memory.
str_t json_unescape(arena_t* a, str_t raw);

// `raw` is a string value as captured, with its quotes. 0 if it is not one.
int json_as_str(arena_t* a, str_t raw, str_t* out);

// A whole number in int64 range, no fraction or exponent. 0 otherwise.
int json_as_int(str_t raw, int64_t* out);

// true or false, as captured. 0 if it is neither.
int json_as_bool(str_t raw, int* out);

// Is the whole text one valid JSON value?
int json_valid(str_t s);

// ---- writer ----

#define JW_MAX_DEPTH 64

typedef struct {
    sb_t* sb;
    uint8_t first[JW_MAX_DEPTH + 1];  // nothing written yet at this depth
    int depth;
    int after_key;
    int bad;                          // calls did not nest properly
} jw_t;

void jw_init(jw_t* w, sb_t* sb);
void jw_obj(jw_t* w);
void jw_obj_end(jw_t* w);
void jw_arr(jw_t* w);
void jw_arr_end(jw_t* w);
void jw_key(jw_t* w, const char* key);
void jw_str(jw_t* w, str_t s);   // any bytes: invalid UTF-8 becomes U+FFFD
void jw_strz(jw_t* w, const char* z);
void jw_int(jw_t* w, int64_t v);
void jw_bool(jw_t* w, int v);
void jw_null(jw_t* w);
void jw_raw(jw_t* w, str_t json);  // text that is already valid JSON, written as it is

#endif

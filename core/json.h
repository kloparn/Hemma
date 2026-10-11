/* core/json.h — strict, allocation-free JSON reader (tokens go into a caller
 * array) and writer helpers that always emit valid UTF-8.
 *
 *   JsonTok tok[64];
 *   Json j;
 *   if (json_parse(&j, text, len, tok, 64)) {
 *       int a = json_get(&j, 0, "a");            // value token of key "a"
 *       char s[100]; json_string(&j, a, s, sizeof s);
 *       for (int k = json_child(&j, 0); k >= 0; k = json_next(&j, 0, k)) ...
 *   }
 */
#ifndef HEMMA_JSON_H
#define HEMMA_JSON_H

#include "base.h"

typedef enum { JSON_NONE, JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_TRUE, JSON_FALSE, JSON_NULL } JsonType;

typedef struct {
    JsonType type;
    int start, len; /* raw span: string body without quotes, number literal, or the whole container */
    int count;      /* number of elements (array) or keys (object) */
    int skip;       /* index of the first token after this subtree */
} JsonTok;

typedef struct {
    const char *src;
    JsonTok *tok;
    int n;
} Json;

/* Parses exactly one value (surrounding whitespace allowed). Fails on syntax
 * errors, nesting deeper than 32, or more than `max` tokens. */
bool json_parse(Json *j, const char *src, size_t len, JsonTok *toks, int max);

int json_get(const Json *j, int obj, const char *key); /* value token or -1 */
int json_index(const Json *j, int arr, int i);         /* element token or -1 */
int json_child(const Json *j, int parent);             /* first element / first key, or -1 */
int json_next(const Json *j, int parent, int cur);     /* next element / next key, or -1 */
static inline JsonType json_type(const Json *j, int t) { return t >= 0 && t < j->n ? j->tok[t].type : JSON_NONE; }

bool json_string(const Json *j, int t, char *out, size_t cap); /* unescaped, UTF-8-safe truncation */
bool json_integer(const Json *j, int t, long long *out);       /* number without fraction/exponent */
bool json_bool(const Json *j, int t, bool *out);
bool json_eq(const Json *j, int t, const char *s); /* string token equals s */

/* Writers: append a quoted, escaped JSON string. Invalid UTF-8 becomes U+FFFD. */
void json_str(Buf *b, const char *s);
void json_strn(Buf *b, const char *s, size_t n);

#endif

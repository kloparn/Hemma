/* core/i18n.h — Hemma in English, Swedish and Spanish (issue #46).
 *
 * The code is written in English, and every text people read goes through T():
 *     H(b, "<h2>%s</h2>", T("Bookings"));
 *     ctx_ok(c, T("Booked %s for %s"), thing, who);
 *     H(b, "<p>%r</p>", T("Press <b>Save</b> when you're done"));  (inline tags: shown raw, so %r)
 *     H(b, "%s", TN(n, "%d video", "%d videos"))  ->  then format the n into it
 * The English is the key: T() looks it up in the language in use and gives the English back when
 * there's no translation. TN() picks "one" for 1 and "other" for the rest - the rule is the same in
 * all three languages. Text said LATER, to OTHER people, or kept (toasts to everyone, the Latest
 * feed, news on someone's page) is marked N_("...") where it's written and translated where it's
 * shown: app_notify/app_notify_but/app_notify_person and activity_add translate their format per
 * screen themselves; i18n_pack/i18n_unpack keep a format with its values for later.
 *
 * Translations sit next to the code that uses them: one <id>_lang.c per module (core's in
 * core/<name>_lang.c), compiled in like the rest - English, Swedish, Spanish:
 *     static const Tr TR[] = {
 *         {"Bookings", "Bokningar", "Reservas"},
 *         {"Booked %s for %s", "Bokade %s åt %s", "Reservado %s para %s"},
 *     };
 *     TRANSLATIONS(book, TR)
 * A translation keeps its English's format specifiers (H()'s and printf's) in the same order and
 * its inline tags, and the same English never has two translations - core/i18n_test.c checks all of
 * that, and that every T()/TN()/N_() text in the code has both translations.
 *
 * Which language a request gets (core/app.c ctx_lang): the person's own choice (pref PREF_LANG,
 * "en"/"sv"/"es", "" = automatic), else the browser's - the cookie I18N_COOKIE (the sign-in page's
 * switcher), else its Accept-Language - else English. Dates follow it (base.c weekday_name/
 * month_name). Logs, the command line and the docs stay English. */
#ifndef HEMMA_I18N_H
#define HEMMA_I18N_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "base.h"

typedef enum { LANG_EN, LANG_SV, LANG_ES, LANG_COUNT } Lang;

typedef struct {
    const char *en, *sv, *es;
} Tr;

typedef struct I18nTable {
    const char *name;
    const Tr *tr;
    int n;
    struct I18nTable *next;
} I18nTable;

void i18n_register(I18nTable *t);
#define TRANSLATIONS(name, arr)                                                                                        \
    static I18nTable i18n_table_##name = {#name, arr, (int)(sizeof(arr) / sizeof((arr)[0])), 0};                      \
    __attribute__((constructor)) static void i18n_register_##name(void) { i18n_register(&i18n_table_##name); }

#define I18N_COOKIE "hemma_lang"
#define N_(s) (s) /* marks text that's translated later, where it's shown */
/* One English text that needs other words by context ("until %s": Spanish "hasta las 17:00" for a time,
   "hasta el lunes" for a day): TC("time", "until %s") looks up "time\x04until %s" - English shows what
   follows the \x04. NC() marks one for later, like N_(). The table entry's English is
   {"time\x04" "until %s", ...} (two literals: a hex escape would swallow the letters after it). */
#define TC(ctx, s) T(ctx "\x04" s)
#define NC(ctx, s) (ctx "\x04" s)

/* (format_arg: the compiler checks a T("...") handed to a printf-like function as if it were the literal) */
const char *T(const char *en) __attribute__((format_arg(1)));     /* in the language in use (lang_now) */
const char *TN(long long n, const char *one, const char *other) /* T(n == 1 ? one : other) */
    __attribute__((format_arg(2))) __attribute__((format_arg(3)));
const char *T_in(Lang lang, const char *en) __attribute__((format_arg(2)));

Lang i18n_lang(void);       /* the language in use on this thread (lang_now) */
Lang i18n_use(Lang lang);   /* returns the one before */
const char *i18n_code(Lang l);  /* "en" "sv" "es" */
const char *i18n_name(Lang l);  /* "English" "Svenska" "Español" - each in itself */
bool i18n_parse(const char *code, Lang *out);    /* "sv", "sv-SE", "es-419", "EN-gb" */
int i18n_accept(const char *accept_language);    /* the browser's best of ours, -1 = none of them */
char i18n_decimal(void);                         /* '.' in English, ',' in Swedish and Spanish */

/* A text to be shown later in whoever's language: the English format and its values, packed into
   out ("fmt" \x1e "value" \x1f "value" ...; %s %d %i %u %ld %lld %lu %llu %c and %% - widths ok);
   false when it doesn't fit (out then holds the English text, shown as it is). i18n_unpack
   appends the text in the language in use: the translated format with the values put back, or a
   plain text (no \x1e) as it is. Values are text either way: they're never translated. */
bool i18n_pack(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool i18n_vpack(char *out, size_t cap, const char *fmt, va_list ap);
/* One more sentence after what out holds (shown with a space between); false = it didn't fit (out
   as it was). A number formatted with the ' flag ("%'lld rows") is grouped by the reader's language
   when shown: 2,944 · 2 944 · 2.944 (i18n_group). */
bool i18n_pack_add(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void i18n_group(Buf *b, const char *digits, size_t n);
void i18n_unpack(Buf *b, const char *packed);
void i18n_unpack_to(char *out, size_t cap, const char *packed);

/* checks and tests */
const Tr *i18n_find(const char *en);
int i18n_specs(const char *fmt, char out[][12], int max); /* the format specifiers, in order */
/* Every table's problems as lines ("book: 'Booked %s': Swedish keeps %s, not %d"): a missing or
   empty translation, specifiers or inline tags that differ from the English, one English with two
   translations. 0 = none. */
int i18n_check(Buf *report);
int i18n_count(void); /* entries in every table */

#endif

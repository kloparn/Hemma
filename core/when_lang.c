/* core/when.c's words in Swedish and Spanish (issue #46; core/i18n.h says how). Its day words -
   Today/today, Tomorrow/tomorrow, Yesterday/yesterday - are core/ui_lang.c's. */
#include "i18n.h"

static const Tr TR[] = {
    /* why a quick-add text was refused (When.refused: book's and plan's quick-add bars) - at most 63 bytes */
    {"Pick a year from 1970 to 2200", "Välj ett år från 1970 till 2200", "Elige un año entre 1970 y 2200"},
    {"There's no %d %s", "Det finns ingen %d %s", "No existe el %d de %s"},
    {"There's no %d %s %d", "Det finns ingen %d %s %d", "No existe el %d de %s de %d"},
    {"The end date is before the start date", "Slutdatumet är före startdatumet",
     "La fecha de fin es anterior a la de inicio"},

    /* when_describe: "Sat 17 Oct, all day" */
    {"%s, all day", "%s, heldag", "%s, todo el día"},
};
TRANSLATIONS(when, TR)

/* modules/home's words in Swedish and Spanish (issue #46; core/i18n.h says how). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the dashboard */
    {"Home", "Hem", "Inicio"},
    {"Good night, %s", "God natt, %s", "Buenas noches, %s"},
    {"Good morning, %s", "God morgon, %s", "Buenos días, %s"},
    {"Hello, %s", "Hej, %s", "Hola, %s"},
    {"Good evening, %s", "God kväll, %s", "Buenas tardes, %s"},
    {"Good night", "God natt", "Buenas noches"},
    {"Good morning", "God morgon", "Buenos días"},
    {"Hello", "Hej", "Hola"},
    {"Good evening", "God kväll", "Buenas tardes"},
    {"%s %d %s · week %d", "%s %d %s · vecka %d", "%s %d de %s · semana %d"},
    {"Wall display", "Väggskärm", "Pantalla de pared"},
    {"Wall", "Vägg", "Pared"},

    /* who's where (the strip of people) */
    {"home", "hemma", "en casa"},
    {"away", "borta", "fuera"},
    {"on %s", "på %s", "en %s"},

    /* Latest (the card, and the wall's block) */
    {"Latest", "Senaste", "Novedades"},
    {"Things people do show up here.", "Det folk gör syns här.", "Lo que hace la gente aparece aquí."},
};
TRANSLATIONS(home, TR)

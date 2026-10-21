/* core/store.c's words for people (a rescued database, a backup put back) in Swedish and Spanish
   (issue #46; core/i18n.h says how). The log keeps them in English. */
#include "i18n.h"

static const Tr TR[] = {
    /* a rescue (store_rescue_packed) */
    {"%s %s: Hemma's data was damaged (it isn't a database file any more). Nothing was deleted: the damaged file is "
     "kept as %s.",
     "%s %s: Hemmas data var skadad (det är inte längre en databasfil). Inget raderades: den skadade filen finns kvar "
     "som %s.",
     "%s %s: los datos de Hemma estaban dañados (ya no es un archivo de base de datos). No se borró nada: el archivo "
     "dañado se guarda como %s."},
    {"%s %s: Hemma's data was damaged (parts of it couldn't be read). Nothing was deleted: the damaged file is kept "
     "as %s.",
     "%s %s: Hemmas data var skadad (delar av den gick inte att läsa). Inget raderades: den skadade filen finns kvar "
     "som %s.",
     "%s %s: los datos de Hemma estaban dañados (no se podían leer algunas partes). No se borró nada: el archivo "
     "dañado se guarda como %s."},
    {"%'lld rows were kept; fewer than before in %s.", "%'lld rader behölls; färre än förut i %s.",
     "Se conservaron %'lld filas; menos que antes en %s."},
    {"%'lld rows were kept.", "%'lld rader behölls.", "Se conservaron %'lld filas."},
    {"%'lld of %'lld rows were kept - %s.", "%'lld av %'lld rader behölls - %s.", "Se conservaron %'lld de %'lld filas - %s."},
    {"All %'lld rows were kept.", "Alla %'lld rader behölls.", "Se conservaron las %'lld filas."},
    {"%'lld more couldn't be put back in their place: they're in the table lost_and_found.",
     "%'lld till gick inte att lägga tillbaka på sin plats: de finns i tabellen lost_and_found.",
     "Otras %'lld no se pudieron devolver a su sitio: están en la tabla lost_and_found."},

    /* a backup put back (store_restore_packed) */
    {"%s %s: the backup %s was NOT put back - %s. Hemma carried on with the data it had (the backup is kept as "
     "hemma.db.restore-refused).",
     "%s %s: säkerhetskopian %s lades INTE tillbaka - %s. Hemma fortsatte med de data den hade (säkerhetskopian finns "
     "kvar som hemma.db.restore-refused).",
     "%s %s: la copia de seguridad %s NO se restauró - %s. Hemma siguió con los datos que tenía (la copia se guarda "
     "como hemma.db.restore-refused)."},
    {"%s %s: the backup %s was put back (%'lld rows).", "%s %s: säkerhetskopian %s lades tillbaka (%'lld rader).",
     "%s %s: se restauró la copia de seguridad %s (%'lld filas)."},
    {"What was there before is kept as %.200s.", "Det som fanns där innan finns kvar som %.200s.",
     "Lo que había antes se guarda como %.200s."},
};
TRANSLATIONS(store, TR)

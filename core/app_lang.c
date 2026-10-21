/* core/app.c's words in Swedish and Spanish (issue #46; core/i18n.h says how). */
#include "i18n.h"

static const Tr TR[] = {
    /* people and their devices */
    {"%s (moved out)", "%s (flyttat)", "%s (se ha mudado)"},
    {"%s on %s", "%s på %s", "%s en %s"},
    {"A browser on %s", "En webbläsare på %s", "Un navegador en %s"},
    {"A browser", "En webbläsare", "Un navegador"},

    /* Undo */
    {"Put back", "Återställt", "Restaurado"},
    {"Nothing left to put back", "Det finns inget kvar att återställa", "No queda nada que restaurar"},
    {"Too late to undo that", "För sent att ångra det", "Demasiado tarde para deshacerlo"},

    /* someone leaves (the remove dialog) */
    {"Their sign-in goes (signed in on %d device now)", "Inloggningen tas bort (inloggad på %d enhet nu)",
     "Su acceso se elimina (con la sesión abierta en %d dispositivo ahora)"},
    {"Their sign-in goes (signed in on %d devices now)", "Inloggningen tas bort (inloggad på %d enheter nu)",
     "Su acceso se elimina (con la sesión abierta en %d dispositivos ahora)"},
    {"Their sign-in goes", "Inloggningen tas bort", "Su acceso se elimina"},
    {"%d line in Latest about what they did goes", "%d rad i Senaste om vad hen har gjort tas bort",
     "Se elimina %d línea de Novedades sobre lo que hizo"},
    {"%d lines in Latest about what they did go", "%d rader i Senaste om vad hen har gjort tas bort",
     "Se eliminan %d líneas de Novedades sobre lo que hizo"},

    /* pages that aren't there */
    {"Not found", "Hittades inte", "No encontrado"},
    {"Nothing here", "Här finns inget", "Aquí no hay nada"},
    {"That page doesn't exist (anymore).", "Sidan finns inte (längre).", "Esa página no existe (o ya no existe)."},

    /* the database in trouble */
    {"Saving failed: the database file is damaged. Recent changes are only in memory. Restart Hemma - it rescues "
     "everything it can still read.",
     "Det gick inte att spara: databasfilen är skadad. De senaste ändringarna finns bara i minnet. Starta om Hemma - "
     "då räddas allt som fortfarande går att läsa.",
     "No se pudo guardar: el archivo de la base de datos está dañado. Los últimos cambios solo están en la memoria. "
     "Reinicia Hemma - rescatará todo lo que todavía pueda leer."},
    {"Saving to disk failed - the disk may be full. Recent changes are only in memory and will be lost on restart; see "
     "the server log.",
     "Det gick inte att spara till disken - den kan vara full. De senaste ändringarna finns bara i minnet och "
     "försvinner vid omstart; se serverns logg.",
     "No se pudo guardar en el disco - puede que esté lleno. Los últimos cambios solo están en la memoria y se "
     "perderán al reiniciar; mira el registro del servidor."},
    {"The database was damaged, and Hemma rescued what it could still read. <a href=\"/system\">See what happened</a>",
     "Databasen var skadad, och Hemma räddade det som fortfarande gick att läsa. <a href=\"/system\">Se vad som "
     "hände</a>",
     "La base de datos estaba dañada y Hemma rescató lo que todavía podía leer. <a href=\"/system\">Mira qué "
     "pasó</a>"},
    {"The database was damaged, and Hemma rescued what it could still read - but no admin account came back. Whoever "
     "runs the Hemma server can bring one back (<b>Locked out</b> in docs/accounts.md).",
     "Databasen var skadad, och Hemma räddade det som fortfarande gick att läsa - men inget administratörskonto kom "
     "tillbaka. Den som sköter Hemma-servern kan få tillbaka ett (<b>Locked out</b> i docs/accounts.md).",
     "La base de datos estaba dañada y Hemma rescató lo que todavía podía leer - pero no volvió ninguna cuenta de "
     "administrador. Quien gestione el servidor de Hemma puede recuperar una (<b>Locked out</b> en "
     "docs/accounts.md)."},

    /* the calendar feed (core/feed.c) and the time zone (core/zone.c) */
    {"%s for %s - %s", "%s åt %s - %s", "%s para %s - %s"},
    {"%s for you - %s", "%s åt dig - %s", "%s para ti - %s"},
    {"UTC%.*s in January, UTC%s in July", "UTC%.*s i januari, UTC%s i juli", "UTC%.*s en enero, UTC%s en julio"},
    {"UTC%s all year", "UTC%s hela året", "UTC%s todo el año"},

    /* actions refused */
    {"This screen is view-only", "Den här skärmen kan bara visa saker", "Esta pantalla es solo para ver"},
    {"Sign in first", "Logga in först", "Inicia sesión primero"},
    {"Ask a grown-up to do that", "Be en vuxen göra det", "Pídele a un adulto que lo haga"},
    {"That isn't open to guests", "Det är inte öppet för gäster", "Eso no está abierto a los invitados"},
    {"Unknown action \"%s\"", "Okänd åtgärd \"%s\"", "Acción desconocida \"%s\""},
    {"Unknown action", "Okänd åtgärd", "Acción desconocida"},
};
TRANSLATIONS(app, TR)

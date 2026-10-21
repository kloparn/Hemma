/* core/ui.c's words in Swedish and Spanish (issue #46; core/i18n.h says how). */
#include "i18n.h"

static const Tr TR[] = {
    /* common words - modules may use these without their own entry (and must agree if they add one) */
    {"Save", "Spara", "Guardar"},
    {"Cancel", "Avbryt", "Cancelar"},
    {"Delete", "Ta bort", "Eliminar"},
    {"Remove", "Ta bort", "Quitar"},
    {"Edit", "Ändra", "Editar"},
    {"Add", "Lägg till", "Añadir"},
    {"Close", "Stäng", "Cerrar"},
    {"Back", "Tillbaka", "Atrás"},
    {"Done", "Klar", "Hecho"},
    {"Open", "Öppna", "Abrir"},
    {"Settings", "Inställningar", "Ajustes"},
    {"Name", "Namn", "Nombre"},
    {"Search", "Sök", "Buscar"},
    {"Show", "Visa", "Mostrar"},
    {"More", "Mer", "Más"},
    {"Yes", "Ja", "Sí"},
    {"No", "Nej", "No"},
    {"New", "Ny", "Nuevo"},
    {"Saved", "Sparat", "Guardado"},
    {"Got it", "Uppfattat", "Entendido"},
    {"Language", "Språk", "Idioma"},
    {"Automatic", "Automatiskt", "Automático"},

    /* people, cards, the shell */
    {"Someone", "Någon", "Alguien"},
    {"See all", "Visa alla", "Ver todo"},
    {"an admin", "en administratör", "un administrador"},
    {"%s (admin)", "%s (administratör)", "%s (administrador)"},
    {"%s or %s (admins)", "%s eller %s (administratörer)", "%s o %s (administradores)"},
    {"%s, %s or %s (admins)", "%s, %s eller %s (administratörer)", "%s, %s o %s (administradores)"},
    {"Everything", "Allt", "Todo"},
    {"Skip to content", "Hoppa till innehållet", "Saltar al contenido"},
    {"Here now", "Här nu", "Aquí ahora"},
    {"A demo with a made-up household: try anything, nothing here is real. The next <code>hemma --demo</code> "
     "starts afresh.",
     "En demo med ett påhittat hushåll: prova vad du vill, inget här är på riktigt. Nästa <code>hemma --demo</code> "
     "börjar från början.",
     "Una demo con un hogar inventado: prueba lo que quieras, nada aquí es real. El próximo <code>hemma --demo</code> "
     "empieza de cero."},

    /* the colours to pick from */
    {"Colour", "Färg", "Color"},
    {"Red", "Röd", "Rojo"},
    {"Orange", "Orange", "Naranja"},
    {"Amber", "Bärnsten", "Ámbar"},
    {"Lime", "Limegrön", "Lima"},
    {"Green", "Grön", "Verde"},
    {"Teal", "Blågrön", "Verde azulado"},
    {"Sky", "Himmelsblå", "Celeste"},
    {"Blue", "Blå", "Azul"},
    {"Indigo", "Indigo", "Índigo"},
    {"Purple", "Lila", "Morado"},
    {"Magenta", "Magenta", "Magenta"},
    {"Pink", "Rosa", "Rosa"},

    /* the player (ui_player) */
    {"Full screen", "Helskärm", "Pantalla completa"},
    {"Full screen (F)", "Helskärm (F)", "Pantalla completa (F)"},
    {"This video won't play in this browser.", "Den här videon går inte att spela i den här webbläsaren.",
     "Este vídeo no se puede reproducir en este navegador."},
    {"Where in the video", "Var i videon", "Posición en el vídeo"},
    {"Live", "Direkt", "En directo"},
    {"Back 10 seconds (Left)", "10 sekunder bakåt (vänsterpil)", "10 segundos atrás (flecha izquierda)"},
    {"Forward 10 seconds (Right)", "10 sekunder framåt (högerpil)", "10 segundos adelante (flecha derecha)"},
    {"Subtitles", "Undertexter", "Subtítulos"},
    {"Off", "Av", "Desactivado"},
    {"Mute (M)", "Stäng av ljudet (M)", "Silenciar (M)"},
    {"Sound (M)", "Ljud (M)", "Sonido (M)"},
    {"Sound off", "Ljudet av", "Sin sonido"},
    {"Volume", "Volym", "Volumen"},

    /* time */
    {"just now", "nyss", "ahora mismo"},
    {"%lld min ago", "för %lld min sedan", "hace %lld min"},
    {"%lld h ago", "för %lld h sedan", "hace %lld h"},
    {"all day", "heldag", "todo el día"},

    /* days in words (ui_day, ui_day_inline) */
    {"Today", "Idag", "Hoy"},
    {"today", "idag", "hoy"},
    {"Tomorrow", "Imorgon", "Mañana"},
    {"tomorrow", "imorgon", "mañana"},
    {"Yesterday", "Igår", "Ayer"},
    {"yesterday", "igår", "ayer"},

    /* wire.js's own (WIRE_TEXTS) */
    {"Photos need the connection to Hemma - wait a moment and send it again",
     "Foton behöver anslutningen till Hemma - vänta en stund och skicka igen",
     "Las fotos necesitan la conexión con Hemma - espera un momento y vuelve a enviarla"},
    {"That doesn't look like a photo", "Det ser inte ut som ett foto", "Eso no parece una foto"},
    {"The photo couldn't be read", "Fotot gick inte att läsa", "No se pudo leer la foto"},
    {"That photo is too big to send - try another", "Fotot är för stort för att skicka - prova ett annat",
     "Esa foto es demasiado grande para enviarla - prueba con otra"},
    {"Unexpected answer (%d)", "Oväntat svar (%d)", "Respuesta inesperada (%d)"},
    {"An upload is already running", "En uppladdning pågår redan", "Ya hay una subida en curso"},
    {"Choose a folder or some files first", "Välj en mapp eller några filer först",
     "Elige primero una carpeta o algunos archivos"},
    {"The upload stopped", "Uppladdningen stannade", "La subida se detuvo"},
    {"The converter stopped", "Konverteraren stannade", "El conversor se detuvo"},
    {"The upload was stopped", "Uppladdningen stoppades", "Se detuvo la subida"},
    {"The connection keeps dropping", "Anslutningen bryts hela tiden", "La conexión se corta una y otra vez"},
    {"The browser lost the copy it made - is its disk full? Make room and start the upload again: what was sent stays",
     "Webbläsaren tappade kopian den gjorde - är disken full? Gör plats och starta uppladdningen igen: det som redan "
     "skickats finns kvar",
     "El navegador perdió la copia que hizo - ¿está lleno su disco? Libera espacio y vuelve a empezar la subida: lo "
     "ya enviado se conserva"},
    {"Undo", "Ångra", "Deshacer"},
    {"+%d more", "+%d till", "+%d más"},
    {"Didn't fit: %s", "Fick inte plats: %s", "No cupo: %s"},
    {"Play", "Spela", "Reproducir"},
    {"Pause", "Pausa", "Pausa"},
    {"Play (Space)", "Spela (mellanslag)", "Reproducir (espacio)"},
    {"Pause (Space)", "Pausa (mellanslag)", "Pausa (espacio)"},
    {"%s of %s", "%s av %s", "%s de %s"},
};
TRANSLATIONS(ui, TR)

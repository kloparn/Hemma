/* Audiobooks' words in Swedish and Spanish (modules/media/books.c, and the few media.c says of them; 1.6). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the tab, the list */
    {"Audiobooks", "Ljudböcker", "Audiolibros"},
    {"Audiobook", "Ljudbok", "Audiolibro"},
    {"%d audiobook", "%d ljudbok", "%d audiolibro"},
    {"%d audiobooks", "%d ljudböcker", "%d audiolibros"},
    {"Books", "Böcker", "Libros"},
    {"Search audiobooks…", "Sök ljudböcker…", "Buscar audiolibros…"},
    {"No audiobooks yet", "Inga ljudböcker än", "Todavía no hay audiolibros"},
    {"An .m4b file anywhere in the library is an audiobook - and so is a folder of .mp3 files inside a folder called "
     "Audiobooks.",
     "En .m4b-fil var som helst i biblioteket är en ljudbok - och det är också en mapp med .mp3-filer i en mapp som "
     "heter Ljudböcker.",
     "Un archivo .m4b en cualquier parte de la biblioteca es un audiolibro, y también lo es una carpeta de archivos .mp3 "
     "dentro de una carpeta llamada Audiolibros."},
    {"Add audiobooks", "Lägg till ljudböcker", "Añadir audiolibros"},
    {"No videos yet", "Inga videor än", "Todavía no hay vídeos"},
    {"Your audiobooks are under Audiobooks.", "Dina ljudböcker finns under Ljudböcker.", "Tus audiolibros están en Audiolibros."},
    /* Continue listening */
    {"Continue listening", "Fortsätt lyssna", "Seguir escuchando"},
    {"Remove from Continue listening", "Ta bort från Fortsätt lyssna", "Quitar de Seguir escuchando"},
    {"Remove %s from Continue listening", "Ta bort %s från Fortsätt lyssna", "Quitar %s de Seguir escuchando"},
    {"Removed %s from Continue listening", "Tog bort %s från Fortsätt lyssna", "Se quitó %s de Seguir escuchando"},
    /* a book's page */
    {"This audiobook isn't in the library", "Den här ljudboken finns inte i biblioteket", "Este audiolibro no está en la biblioteca"},
    {"read by %s", "uppläst av %s", "leído por %s"},
    {"%s · book %d", "%s · bok %d", "%s · libro %d"},
    {"Finished listening", "Färdiglyssnad", "Escuchado entero"},
    {"Mark as finished", "Markera som färdiglyssnad", "Marcar como escuchado"},
    {"Marked as finished", "Markerad som färdiglyssnad", "Marcado como escuchado"},
    {"Marked as not finished", "Markerad som inte färdiglyssnad", "Marcado como no escuchado"},
    {"About this book", "Om boken", "Sobre este libro"},
    {"Chapters", "Kapitel", "Capítulos"},
    {"Part %d", "Del %d", "Parte %d"},
    {"Next in the series:", "Nästa i serien:", "Siguiente en la serie:"},
    /* the player */
    {"Where in the book", "Var i boken", "Dónde en el libro"},
    {"Previous chapter", "Föregående kapitel", "Capítulo anterior"},
    {"Next chapter", "Nästa kapitel", "Capítulo siguiente"},
    {"Back 15 seconds", "15 sekunder bakåt", "Atrás 15 segundos"},
    {"Forward 30 seconds", "30 sekunder framåt", "Adelante 30 segundos"},
    {"Sleep timer", "Insomningstimer", "Temporizador"},
    {"%d minutes", "%d minuter", "%d minutos"},
    {"End of this chapter", "Slutet av kapitlet", "Final de este capítulo"},
    {"This book won't play in this browser.", "Den här boken går inte att spela i den här webbläsaren.",
     "Este libro no se puede reproducir en este navegador."},
};
TRANSLATIONS(media_books, TR)

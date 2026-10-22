/* Downloads' words in Swedish and Spanish (modules/media/download.c and stream.c; core/i18n.h says how). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the buttons: the watch page, the series page */
    {"Download (%s)", "Ladda ner (%s)", "Descargar (%s)"},
    {"Download season %d (%s)", "Ladda ner säsong %d (%s)", "Descargar la temporada %d (%s)"},
    {"Download the specials (%s)", "Ladda ner specialavsnitten (%s)", "Descargar los especiales (%s)"},
    {"Download all (%s)", "Ladda ner allt (%s)", "Descargar todo (%s)"},
    {"Downloads slow down while someone is watching.", "Nedladdningar går långsammare medan någon tittar.",
     "Las descargas van más despacio mientras alguien está viendo algo."},
    /* what a download is answered with when it can't go */
    {"Only grown-ups can download videos.", "Bara vuxna kan ladda ner videor.", "Solo los adultos pueden descargar vídeos."},
    {"That season isn't in the library", "Den säsongen finns inte i biblioteket", "Esa temporada no está en la biblioteca"},
    {"Two seasons are being downloaded already - try again when one of them is done.",
     "Två säsonger laddas redan ner - försök igen när en av dem är klar.",
     "Ya se están descargando dos temporadas - vuelve a intentarlo cuando termine una de ellas."},
};
TRANSLATIONS(media_download, TR)

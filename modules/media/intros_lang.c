/* modules/media/intros.c's words in Swedish and Spanish (issue #46; core/i18n.h says how): a series' intros, found
   by themselves (Library settings), and an episode's intro on Manage (2.9). Swedish "ett intro" (introt, intron),
   Spanish "la intro" (las intros). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the card */
    {"Skip intros", "Hoppa över intron", "Saltar intros"},
    {"Hemma listens to the start of every episode, in the background while nobody watches, and finds the intro the "
     "episodes of a season share. The player then offers Skip intro. You can fix one on the series' Manage page.",
     "Hemma lyssnar på början av varje avsnitt, i bakgrunden när ingen tittar, och hittar introt som avsnitten i en "
     "säsong delar. Spelaren erbjuder sedan Hoppa över intro. Du kan rätta ett på seriens Hantera-sida.",
     "Hemma escucha el comienzo de cada episodio, en segundo plano mientras nadie está viendo nada, y encuentra la intro "
     "que comparten los episodios de una temporada. Así el reproductor ofrece Saltar intro. Puedes corregir una en la "
     "página Gestionar de la serie."},
    {"Listening for intros: %s of %s episodes done (waits while someone watches)",
     "Lyssnar efter intron: %s av %s avsnitt klara (väntar medan någon tittar)",
     "Buscando intros: %s de %s episodios hechos (espera mientras alguien está viendo algo)"},
    {"Listening for intros: %s of %s episodes done - waiting while someone watches",
     "Lyssnar efter intron: %s av %s avsnitt klara - väntar medan någon tittar",
     "Buscando intros: %s de %s episodios hechos - esperando mientras alguien está viendo algo"},
    {"Listening for intros: %s of %s episodes done - waiting for the conversions to finish",
     "Lyssnar efter intron: %s av %s avsnitt klara - väntar tills konverteringarna är klara",
     "Buscando intros: %s de %s episodios hechos - esperando a que terminen las conversiones"},
    {"Listening for intros: %s of %s episodes done - waiting for the subtitles to be taken out",
     "Lyssnar efter intron: %s av %s avsnitt klara - väntar tills undertexterna är uttagna",
     "Buscando intros: %s de %s episodios hechos - esperando a que se saquen los subtítulos"},
    {"Listening for intros: %s of %s episodes done - waiting for a library folder that's missing",
     "Lyssnar efter intron: %s av %s avsnitt klara - väntar på en biblioteksmapp som saknas",
     "Buscando intros: %s de %s episodios hechos - esperando una carpeta de la biblioteca que falta"},
    {"An intro found in %s episode so far", "Ett intro hittat i %s avsnitt hittills", "Una intro encontrada en %s episodio hasta ahora"},
    {"Intros found in %s episodes so far", "Intron hittade i %s avsnitt hittills", "Intros encontradas en %s episodios hasta ahora"},
    {"Switched off - %s of %s episodes done so far.", "Avstängt - %s av %s avsnitt klara hittills.",
     "Desactivado - %s de %s episodios hechos hasta ahora."},
    {"An intro found in %s episode", "Ett intro hittat i %s avsnitt", "Una intro encontrada en %s episodio"},
    {"Intros found in %s episodes", "Intron hittade i %s avsnitt", "Intros encontradas en %s episodios"},
    {"No intros found.", "Inga intron hittades.", "No se encontraron intros."},
    {"%s episode's sound can't be listened to yet", "Ljudet i %s avsnitt går inte att lyssna på än",
     "El sonido de %s episodio aún no se puede escuchar"},
    {"%s episodes' sound can't be listened to yet", "Ljudet i %s avsnitt går inte att lyssna på än",
     "El sonido de %s episodios aún no se puede escuchar"},
    {"Find intros by itself", "Hitta intron av sig själv", "Buscar las intros automáticamente"},
    /* the switch's answers */
    {"Hemma finds the intros by itself", "Hemma hittar introna av sig själv", "Hemma busca las intros automáticamente"},
    {"Hemma stops looking for intros", "Hemma slutar leta efter intron", "Hemma deja de buscar intros"},
    /* Manage: an episode's row and its dialog */
    {"Intro", "Intro", "Intro"},
    {"Intro %s–%s", "Intro %s–%s", "Intro %s–%s"},
    {"No intro", "Inget intro", "Sin intro"},
    {"The intro", "Introt", "La intro"},
    {"Hemma hasn't listened to this episode yet - it does when nobody watches.",
     "Hemma har inte lyssnat på det här avsnittet än - det gör den när ingen tittar.",
     "Hemma aún no ha escuchado este episodio - lo hace cuando nadie está viendo nada."},
    {"You set this intro by hand.", "Du har ställt in det här introt för hand.", "Pusiste esta intro a mano."},
    {"You said this episode has no intro.", "Du har sagt att det här avsnittet inte har något intro.", "Dijiste que este episodio no tiene intro."},
    {"Hemma found this intro by itself: the stretch this season's episodes share.",
     "Hemma hittade introt av sig själv: biten som säsongens avsnitt delar.",
     "Hemma encontró esta intro automáticamente: el tramo que comparten los episodios de la temporada."},
    {"Hemma found no intro in this episode.", "Hemma hittade inget intro i det här avsnittet.", "Hemma no encontró ninguna intro en este episodio."},
    {"Hemma couldn't listen to this episode's sound.", "Hemma kunde inte lyssna på ljudet i det här avsnittet.",
     "Hemma no pudo escuchar el sonido de este episodio."},
    {"Starts at", "Börjar vid", "Empieza en"},
    {"Ends at", "Slutar vid", "Termina en"},
    {"It has no intro", "Det har inget intro", "No tiene intro"},
    {"Let Hemma find it", "Låt Hemma hitta det", "Que Hemma la busque"},
    /* the dialog's answers */
    {"That episode isn't in the library any more", "Det avsnittet finns inte i biblioteket längre", "Ese episodio ya no está en la biblioteca"},
    {"Hemma finds this episode's intro again when nobody watches", "Hemma hittar avsnittets intro igen när ingen tittar",
     "Hemma vuelve a buscar la intro de este episodio cuando nadie esté viendo nada"},
    {"No Skip intro for this episode", "Ingen Hoppa över intro för det här avsnittet", "Sin Saltar intro para este episodio"},
    {"Write it as minutes:seconds, like 1:02", "Skriv det som minuter:sekunder, till exempel 1:02", "Escríbelo como minutos:segundos, por ejemplo 1:02"},
    {"Write it as minutes:seconds, like 1:34", "Skriv det som minuter:sekunder, till exempel 1:34", "Escríbelo como minutos:segundos, por ejemplo 1:34"},
    {"It ends before it starts - or right after", "Det slutar innan det börjar - eller direkt efter", "Termina antes de empezar - o justo después"},
    {"That's past the end of the episode", "Det är efter avsnittets slut", "Eso es después del final del episodio"},
    {"Skip intro goes from %s to %s", "Hoppa över intro går från %s till %s", "Saltar intro va de %s a %s"},
};
TRANSLATIONS(intros, TR)

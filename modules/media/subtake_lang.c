/* modules/media/subtake.c's words in Swedish and Spanish (issue #46; core/i18n.h says how): the subtitles
   inside the library's videos, taken out by themselves (Library settings). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the card */
    {"Subtitles inside videos", "Undertexter inuti videor", "Subtítulos dentro de los vídeos"},
    {"Many MKV videos carry their subtitles inside the file, where only some players find them. Hemma takes them "
     "out once, in the background, and saves them next to each video - so they show here and in other players too.",
     "Många MKV-videor har sina undertexter inuti filen, där bara vissa spelare hittar dem. Hemma tar ut dem en gång, "
     "i bakgrunden, och sparar dem bredvid varje video - så att de syns här och i andra spelare också.",
     "Muchos vídeos MKV llevan los subtítulos dentro del archivo, donde solo algunos reproductores los encuentran. "
     "Hemma los saca una vez, en segundo plano, y los guarda junto a cada vídeo - así se ven aquí y también en otros "
     "reproductores."},
    {"Taking subtitles out of videos: %s of %s checked (waits while someone watches)",
     "Tar ut undertexter ur videor: %s av %s genomgångna (väntar medan någon tittar)",
     "Sacando los subtítulos de los vídeos: %s de %s revisados (espera mientras alguien está viendo algo)"},
    {"Taking subtitles out of videos: %s of %s checked - waiting while someone watches",
     "Tar ut undertexter ur videor: %s av %s genomgångna - väntar medan någon tittar",
     "Sacando los subtítulos de los vídeos: %s de %s revisados - esperando mientras alguien está viendo algo"},
    {"Taking subtitles out of videos: %s of %s checked - waiting for the conversions to finish",
     "Tar ut undertexter ur videor: %s av %s genomgångna - väntar tills konverteringarna är klara",
     "Sacando los subtítulos de los vídeos: %s de %s revisados - esperando a que terminen las conversiones"},
    {"Taking subtitles out of videos: %s of %s checked - waiting for a library folder that's missing",
     "Tar ut undertexter ur videor: %s av %s genomgångna - väntar på en biblioteksmapp som saknas",
     "Sacando los subtítulos de los vídeos: %s de %s revisados - esperando una carpeta de la biblioteca que falta"},
    {"Subtitles found in %s video so far", "Undertexter hittade i %s video hittills", "Subtítulos encontrados en %s vídeo hasta ahora"},
    {"Subtitles found in %s videos so far", "Undertexter hittade i %s videor hittills",
     "Subtítulos encontrados en %s vídeos hasta ahora"},
    {"Subtitles found in %s video", "Undertexter hittade i %s video", "Subtítulos encontrados en %s vídeo"},
    {"Subtitles found in %s videos", "Undertexter hittade i %s videor", "Subtítulos encontrados en %s vídeos"},
    {"Switched off - %s of %s videos checked so far.", "Avstängt - %s av %s videor genomgångna hittills.",
     "Desactivado - %s de %s vídeos revisados hasta ahora."},
    {"No subtitles found inside the videos.", "Inga undertexter hittades inuti videorna.",
     "No se encontraron subtítulos dentro de los vídeos."},
    {"Take subtitles out of videos by themselves", "Ta ut undertexter ur videor av sig själv",
     "Sacar los subtítulos de los vídeos automáticamente"},
    /* the switch's answers */
    {"Hemma takes the subtitles out of videos by itself", "Hemma tar ut undertexterna ur videor av sig själv",
     "Hemma saca los subtítulos de los vídeos automáticamente"},
    {"Subtitles stay inside the videos", "Undertexterna stannar inuti videorna", "Los subtítulos se quedan dentro de los vídeos"},
};
TRANSLATIONS(media_subtake, TR)

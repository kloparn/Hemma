/* modules/media's words in Swedish and Spanish (issue #46; core/i18n.h says how) - Watch's pages, Library
   settings, the TV's tiles, the System page's checks and the probe's verdicts. upload.c, convert.c and
   drives.c keep theirs in their own tables. */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the section, its tabs and header */
    {"Watch", "Titta", "Ver"},
    {"Home", "Hem", "Inicio"},
    {"Movies", "Filmer", "Películas"},
    {"Series", "Serier", "Series"},
    {"Live TV", "Live-tv", "TV en directo"},
    {"Folders", "Mappar", "Carpetas"},
    {"Library", "Bibliotek", "Biblioteca"},
    {"Library settings", "Biblioteksinställningar", "Ajustes de la biblioteca"},
    {"Scanning · %d file", "Söker igenom · %d fil", "Examinando · %d archivo"},
    {"Scanning · %d files", "Söker igenom · %d filer", "Examinando · %d archivos"},
    {"Search movies and series…", "Sök filmer och serier…", "Buscar películas y series…"},
    {"Search the library", "Sök i biblioteket", "Buscar en la biblioteca"},
    {"See all", "Visa alla", "Ver todo"},

    /* posters, cards and counts */
    {"Watched", "Sett", "Visto"},
    {"Video", "Video", "Vídeo"},
    {"Episode %d", "Avsnitt %d", "Episodio %d"},
    {"%d episode", "%d avsnitt", "%d episodio"},
    {"%d episodes", "%d avsnitt", "%d episodios"},
    {"%d seasons", "%d säsonger", "%d temporadas"},
    {"%d new episode", "%d nytt avsnitt", "%d episodio nuevo"},
    {"%d new episodes", "%d nya avsnitt", "%d episodios nuevos"},
    {"%d movie", "%d film", "%d película"},
    {"%d movies", "%d filmer", "%d películas"},
    {"1 series", "1 serie", "1 serie"},
    {"%d series", "%d serier", "%d series"},
    {"%d other video", "%d annan video", "%d otro vídeo"},
    {"%d other videos", "%d andra videor", "%d otros vídeos"},
    {"%d video", "%d video", "%d vídeo"},
    {"%d videos", "%d videor", "%d vídeos"},

    /* a ripped disc's extras, named in the reader's language (media_title) */
    {"season %d, disc %d", "säsong %d, skiva %d", "temporada %d, disco %d"},
    {"season %d", "säsong %d", "temporada %d"},
    {"specials, disc %d", "specialavsnitt, skiva %d", "especiales, disco %d"},
    {"specials", "specialavsnitt", "especiales"},
    {"%s – %s: all episodes", "%s – %s: alla avsnitt", "%s – %s: todos los episodios"},
    {"%s – %s: extra (title %d)", "%s – %s: extramaterial (titel %d)", "%s – %s: extra (título %d)"},
    {"%s – %s: extra", "%s – %s: extramaterial", "%s – %s: extra"},

    /* a series' or film's picture */
    {"Picture", "Bild", "Imagen"},
    {"Change the picture", "Byt bild", "Cambiar la imagen"},
    {"Choose a picture first", "Välj en bild först", "Elige primero una imagen"},
    {"A picture from this device", "En bild från den här enheten", "Una imagen de este dispositivo"},
    {"JPEG, PNG or WebP - a tall poster (2:3) fits best. It's saved in the library folder as "
     "<code class=\"media-code\">%s.jpg</code> (or .png/.webp), where other players find it too.",
     "JPEG, PNG eller WebP - en hög affisch (2:3) passar bäst. Den sparas i biblioteksmappen som "
     "<code class=\"media-code\">%s.jpg</code> (eller .png/.webp), där andra spelare också hittar den.",
     "JPEG, PNG o WebP - un póster vertical (2:3) queda mejor. Se guarda en la carpeta de la biblioteca como "
     "<code class=\"media-code\">%s.jpg</code> (o .png/.webp), donde otros reproductores también la encuentran."},
    {"Use this picture", "Använd den här bilden", "Usar esta imagen"},
    {"Remove this picture? Its file is deleted from the library folder.",
     "Ta bort bilden? Filen raderas från biblioteksmappen.",
     "¿Quitar esta imagen? Su archivo se borra de la carpeta de la biblioteca."},
    {"Remove picture", "Ta bort bilden", "Quitar la imagen"},

    /* folders to add with one tap */
    {"Use <code class=\"media-code\">%s</code>", "Använd <code class=\"media-code\">%s</code>",
     "Usar <code class=\"media-code\">%s</code>"},
    {"Add %s", "Lägg till %s", "Añadir %s"},
    {"from the installer", "från installationen", "del instalador"},
    {"and %d more", "och %d till", "y %d más"},

    /* an empty library */
    {"No movies yet", "Inga filmer än", "Aún no hay películas"},
    {"Movies and series from the drive at home will show up here.",
     "Filmer och serier från disken hemma dyker upp här.",
     "Las películas y series del disco de casa aparecerán aquí."},
    {"Ask %r to add the movie folders.", "Be %r att lägga till filmmapparna.",
     "Pide a %r que añada las carpetas de películas."},
    {"Point Hemma at your movies", "Visa Hemma var filmerna finns", "Dile a Hemma dónde están tus películas"},
    {"Tell it which folders hold your movies and series — the drive on the Pi, a mounted share, or a folder on this "
     "computer.",
     "Berätta vilka mappar som har dina filmer och serier — disken på Pi:n, en monterad nätverksdelning eller en mapp "
     "på den här datorn.",
     "Dile qué carpetas tienen tus películas y series — el disco de la Pi, una carpeta compartida montada o una carpeta "
     "de este ordenador."},
    {"Another folder", "En annan mapp", "Otra carpeta"},
    {"Choose folders", "Välj mappar", "Elegir carpetas"},
    {"Looking through your library…", "Går igenom ditt bibliotek…", "Revisando tu biblioteca…"},
    {"Movies and series show up here as soon as the scan finishes.",
     "Filmer och serier dyker upp här så fort genomsökningen är klar.",
     "Las películas y series aparecen aquí en cuanto termine el análisis."},
    {"No videos found yet", "Inga videor hittade än", "Aún no se han encontrado vídeos"},
    {"Hemma looked in your folders but found no video files. Check the folders in Library settings.",
     "Hemma letade i dina mappar men hittade inga videofiler. Kolla mapparna i Biblioteksinställningar.",
     "Hemma buscó en tus carpetas pero no encontró archivos de vídeo. Revisa las carpetas en Ajustes de la biblioteca."},
    {"Nothing matches", "Inget matchar", "Nada coincide"},
    {"Try another word — titles, episode names and folders are searched.",
     "Prova ett annat ord — titlar, avsnittsnamn och mappar genomsöks.",
     "Prueba otra palabra — se buscan títulos, nombres de episodios y carpetas."},

    /* Watch's home and lists */
    {"Continue watching", "Fortsätt titta", "Seguir viendo"},
    {"On TV", "På TV", "En la tele"},
    {"Guide", "Tablå", "Guía"},
    {"Recently added", "Nyligen tillagt", "Añadido recientemente"},
    {"Other videos", "Andra videor", "Otros vídeos"},
    {"A–Z", "A–Ö", "A–Z"},
    {"Newest", "Nyast", "Más recientes"},
    {"Year", "År", "Año"},
    {"Files with a year in the name — like \"Inception (2010).mkv\" — become movies.",
     "Filer med ett årtal i namnet — som \"Inception (2010).mkv\" — blir filmer.",
     "Los archivos con un año en el nombre — como \"Inception (2010).mkv\" — se convierten en películas."},
    {"No series yet", "Inga serier än", "Aún no hay series"},
    {"Episodes named like \"Show.S01E02.mkv\" are grouped into series.",
     "Avsnitt med namn som \"Show.S01E02.mkv\" samlas i serier.",
     "Los episodios con nombres como \"Show.S01E02.mkv\" se agrupan en series."},

    /* a series */
    {"Mark as not watched", "Markera som inte sett", "Marcar como no visto"},
    {"Mark as watched", "Markera som sett", "Marcar como visto"},
    {"This series isn't in the library", "Den här serien finns inte i biblioteket", "Esta serie no está en la biblioteca"},
    {"It may have been moved or renamed.", "Den kan ha flyttats eller bytt namn.",
     "Puede que se haya movido o cambiado de nombre."},
    {"%d watched", "%d sedda", "%d vistos"},
    {"Resume", "Fortsätt", "Continuar"},
    {"Next up", "Härnäst", "Siguiente"},
    {"Start watching", "Börja titta", "Empezar a ver"},
    {"Season %d", "Säsong %d", "Temporada %d"},
    {"disc %d", "skiva %d", "disco %d"},
    {"needs conversion", "behöver konverteras", "necesita conversión"},
    {"Extras", "Extramaterial", "Extras"},
    {"All episodes in one", "Alla avsnitt i ett", "Todos los episodios en uno"},
    {"Extra · title %d", "Extramaterial · titel %d", "Extra · título %d"},
    {"Extra", "Extramaterial", "Extra"},
    {"Disc %d", "Skiva %d", "Disco %d"},

    /* Folders */
    {"Not for children: <a href=\"/media/browse?p=%d:%u\">%s</a> is marked, and this is in it.",
     "Inte för barn: <a href=\"/media/browse?p=%d:%u\">%s</a> är markerad, och det här ligger i den.",
     "No apto para niños: <a href=\"/media/browse?p=%d:%u\">%s</a> está marcada, y esto está dentro."},
    {"Not for children", "Inte för barn", "No apto para niños"},
    {"- this folder and everything in it stays out of a child's Watch",
     "- den här mappen och allt i den hålls borta från ett barns Titta",
     "- esta carpeta y todo lo que contiene queda fuera de Ver para los niños"},
    {"- the whole library folder and everything in it stays out of a child's Watch",
     "- hela biblioteksmappen och allt i den hålls borta från ett barns Titta",
     "- toda la carpeta de la biblioteca y lo que contiene queda fuera de Ver para los niños"},
    {"not for children", "inte för barn", "no apto para niños"},
    {"Nothing to play here", "Inget att spela här", "Nada que reproducir aquí"},

    /* removing a film */
    {"Remove from the library", "Ta bort från biblioteket", "Quitar de la biblioteca"},
    {"<b>%s</b> (%d) is deleted from the disk - for everyone in the household, and it can't be undone.",
     "<b>%s</b> (%d) raderas från disken - för alla i hushållet, och det går inte att ångra.",
     "<b>%s</b> (%d) se borra del disco - para todo el hogar, y no se puede deshacer."},
    {"<b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone.",
     "<b>%s</b> raderas från disken - för alla i hushållet, och det går inte att ångra.",
     "<b>%s</b> se borra del disco - para todo el hogar, y no se puede deshacer."},
    {"Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d file, %s.",
     "Mappen <code class=\"media-code\">%s</code> försvinner med allt i den: %d fil, %s.",
     "Su carpeta <code class=\"media-code\">%s</code> se va con todo lo que contiene: %d archivo, %s."},
    {"Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d files, %s.",
     "Mappen <code class=\"media-code\">%s</code> försvinner med allt i den: %d filer, %s.",
     "Su carpeta <code class=\"media-code\">%s</code> se va con todo lo que contiene: %d archivos, %s."},
    {"%d file, %s:", "%d fil, %s:", "%d archivo, %s:"},
    {"%d files, %s:", "%d filer, %s:", "%d archivos, %s:"},
    {"Keep it", "Behåll den", "Conservar"},
    {"Delete from the disk", "Radera från disken", "Borrar del disco"},

    /* the watch page and its player */
    {"Subtitles", "Undertexter", "Subtítulos"},
    /* which of two subtitles in one language ("English (for the hard of hearing)": SDH) */
    {"%s (for the hard of hearing)", "%s (för hörselskadade)", "%s (para personas sordas)"},
    {"%s (foreign speech only)", "%s (bara främmande språk)", "%s (solo otros idiomas)"},
    {"%s (for the hard of hearing, foreign speech only)", "%s (för hörselskadade, bara främmande språk)",
     "%s (para personas sordas, solo otros idiomas)"},
    {"This video is HEVC (x265), which this browser can't play. Most phones, computers and iPads can.",
     "Den här videon är HEVC (x265), som den här webbläsaren inte kan spela. De flesta telefoner, datorer och iPads kan.",
     "Este vídeo es HEVC (x265), que este navegador no puede reproducir. La mayoría de móviles, ordenadores e iPads sí."},
    {"This video is AV1, which this browser can't play. Chrome, Edge, Firefox and Android phones can, and the newest "
     "iPhones and Macs.",
     "Den här videon är AV1, som den här webbläsaren inte kan spela. Chrome, Edge, Firefox och Android-telefoner kan, "
     "liksom de nyaste iPhone- och Mac-modellerna.",
     "Este vídeo es AV1, que este navegador no puede reproducir. Chrome, Edge, Firefox y los móviles Android sí, y "
     "también los iPhone y Mac más nuevos."},
    {"Next episode in <span data-count>%d</span> s", "Nästa avsnitt om <span data-count>%d</span> s",
     "Siguiente episodio en <span data-count>%d</span> s"},
    {"Play now", "Spela nu", "Reproducir ahora"},
    {"Hide", "Dölj", "Ocultar"},
    {"This video isn't in the library", "Den här videon finns inte i biblioteket", "Este vídeo no está en la biblioteca"},
    {"It may have been moved, renamed or deleted.", "Den kan ha flyttats, bytt namn eller raderats.",
     "Puede que se haya movido, cambiado de nombre o borrado."},
    {"Previous episode", "Föregående avsnitt", "Episodio anterior"},
    {"Previous", "Föregående", "Anterior"},
    {"Next episode", "Nästa avsnitt", "Siguiente episodio"},
    {"Next", "Nästa", "Siguiente"},
    {"Mark watched", "Markera som sett", "Marcar como visto"},
    {"Delete it from the library", "Radera den från biblioteket", "Borrarlo de la biblioteca"},
    {"Plays on newer devices", "Spelas på nyare enheter", "Se reproduce en dispositivos más nuevos"},
    {"Plays in browsers", "Spelas i webbläsare", "Se reproduce en navegadores"},
    {"May not play everywhere", "Spelas kanske inte överallt", "Puede que no se reproduzca en todas partes"},
    {"Needs conversion", "Behöver konverteras", "Necesita conversión"},
    {"Unknown", "Okänd", "Desconocido"},
    {"Converted copy", "Konverterad kopia", "Copia convertida"},
    {"Another version", "En annan version", "Otra versión"},
    {"HEVC video: Safari on iPhone, iPad and Mac, and Chrome or Edge on most computers and phones from the last few years",
     "HEVC-video: Safari på iPhone, iPad och Mac, och Chrome eller Edge på de flesta datorer och telefoner från de "
     "senaste åren",
     "Vídeo HEVC: Safari en iPhone, iPad y Mac, y Chrome o Edge en la mayoría de ordenadores y móviles de los últimos "
     "años"},
    {"AV1 video: Chrome, Edge and Firefox on computers and phones, most TVs from the last few years, and the newest "
     "iPhones, iPads and Macs",
     "AV1-video: Chrome, Edge och Firefox på datorer och telefoner, de flesta TV-apparater från de senaste åren och de "
     "nyaste iPhone-, iPad- och Mac-modellerna",
     "Vídeo AV1: Chrome, Edge y Firefox en ordenadores y móviles, la mayoría de las teles de los últimos años y los "
     "iPhone, iPad y Mac más nuevos"},
    {"File details", "Fildetaljer", "Detalles del archivo"},
    {"%s file", "Fil: %s", "Archivo: %s"},
    {"%s video", "Video: %s", "Vídeo: %s"},
    {"%s sound", "Ljud: %s", "Sonido: %s"},
    {"Being made playable everywhere - see the library settings.",
     "Görs spelbar överallt - se biblioteksinställningarna.",
     "Se está haciendo reproducible en todas partes - mira los ajustes de la biblioteca."},
    {"Its DTS sound plays in no browser, and Safari can't open MKV files: a copy with AAC sound fixes both - quick, the "
     "picture isn't re-encoded. HEVC still needs a device that can decode it (most from the last few years).",
     "DTS-ljudet spelas inte i någon webbläsare, och Safari kan inte öppna MKV-filer: en kopia med AAC-ljud fixar båda "
     "- snabbt, bilden kodas inte om. HEVC behöver fortfarande en enhet som kan avkoda det (de flesta från de senaste "
     "åren).",
     "Su sonido DTS no se oye en ningún navegador, y Safari no puede abrir archivos MKV: una copia con sonido AAC "
     "arregla las dos cosas - rápido, la imagen no se recodifica. HEVC sigue necesitando un dispositivo que pueda "
     "decodificarlo (la mayoría de los últimos años)."},
    {"Its Dolby sound plays silently in Chrome and Firefox, and Safari can't open MKV files: a copy with AAC sound "
     "fixes both - quick, the picture isn't re-encoded. HEVC still needs a device that can decode it (most from the "
     "last few years).",
     "Dolby-ljudet blir tyst i Chrome och Firefox, och Safari kan inte öppna MKV-filer: en kopia med AAC-ljud fixar "
     "båda - snabbt, bilden kodas inte om. HEVC behöver fortfarande en enhet som kan avkoda det (de flesta från de "
     "senaste åren).",
     "Su sonido Dolby no se oye en Chrome ni en Firefox, y Safari no puede abrir archivos MKV: una copia con sonido "
     "AAC arregla las dos cosas - rápido, la imagen no se recodifica. HEVC sigue necesitando un dispositivo que pueda "
     "decodificarlo (la mayoría de los últimos años)."},
    {"Safari (iPhone, iPad, Mac) can't open MKV files: a new container fixes that - quick, the picture isn't "
     "re-encoded. HEVC still needs a device that can decode it (most from the last few years).",
     "Safari (iPhone, iPad, Mac) kan inte öppna MKV-filer: en ny behållare fixar det - snabbt, bilden kodas inte om. "
     "HEVC behöver fortfarande en enhet som kan avkoda det (de flesta från de senaste åren).",
     "Safari (iPhone, iPad, Mac) no puede abrir archivos MKV: un contenedor nuevo lo arregla - rápido, la imagen no se "
     "recodifica. HEVC sigue necesitando un dispositivo que pueda decodificarlo (la mayoría de los últimos años)."},
    {"Its DTS sound plays in no browser, and Safari can't open MKV files: a copy with AAC sound fixes both - quick, the "
     "picture isn't re-encoded. AV1 still needs a device that can decode it (Chrome, Edge and Firefox can; of Apple's, "
     "only the newest).",
     "DTS-ljudet spelas inte i någon webbläsare, och Safari kan inte öppna MKV-filer: en kopia med AAC-ljud fixar båda "
     "- snabbt, bilden kodas inte om. AV1 behöver fortfarande en enhet som kan avkoda det (Chrome, Edge och Firefox kan; "
     "av Apples bara de nyaste).",
     "Su sonido DTS no se oye en ningún navegador, y Safari no puede abrir archivos MKV: una copia con sonido AAC "
     "arregla las dos cosas - rápido, la imagen no se recodifica. AV1 sigue necesitando un dispositivo que pueda "
     "decodificarlo (Chrome, Edge y Firefox pueden; de los de Apple, solo los más nuevos)."},
    {"Its Dolby sound plays silently in Chrome and Firefox, and Safari can't open MKV files: a copy with AAC sound "
     "fixes both - quick, the picture isn't re-encoded. AV1 still needs a device that can decode it (Chrome, Edge and "
     "Firefox can; of Apple's, only the newest).",
     "Dolby-ljudet blir tyst i Chrome och Firefox, och Safari kan inte öppna MKV-filer: en kopia med AAC-ljud fixar "
     "båda - snabbt, bilden kodas inte om. AV1 behöver fortfarande en enhet som kan avkoda det (Chrome, Edge och "
     "Firefox kan; av Apples bara de nyaste).",
     "Su sonido Dolby no se oye en Chrome ni en Firefox, y Safari no puede abrir archivos MKV: una copia con sonido "
     "AAC arregla las dos cosas - rápido, la imagen no se recodifica. AV1 sigue necesitando un dispositivo que pueda "
     "decodificarlo (Chrome, Edge y Firefox pueden; de los de Apple, solo los más nuevos)."},
    {"Safari (iPhone, iPad, Mac) can't open MKV files: a new container fixes that - quick, the picture isn't "
     "re-encoded. AV1 still needs a device that can decode it (Chrome, Edge and Firefox can; of Apple's, only the "
     "newest).",
     "Safari (iPhone, iPad, Mac) kan inte öppna MKV-filer: en ny behållare fixar det - snabbt, bilden kodas inte om. "
     "AV1 behöver fortfarande en enhet som kan avkoda det (Chrome, Edge och Firefox kan; av Apples bara de nyaste).",
     "Safari (iPhone, iPad, Mac) no puede abrir archivos MKV: un contenedor nuevo lo arregla - rápido, la imagen no se "
     "recodifica. AV1 sigue necesitando un dispositivo que pueda decodificarlo (Chrome, Edge y Firefox pueden; de los "
     "de Apple, solo los más nuevos)."},
    {"A new container makes this play on iPhones and every TV too - quick, nothing is re-encoded.",
     "En ny behållare gör att den spelas på iPhones och alla TV-apparater också - snabbt, inget kodas om.",
     "Un contenedor nuevo hace que se reproduzca también en iPhones y en cualquier tele - rápido, no se recodifica "
     "nada."},
    {"Hemma can convert this once into a copy that plays on every browser, iPhone and TV.",
     "Hemma kan konvertera den en gång till en kopia som spelas i alla webbläsare, iPhones och TV-apparater.",
     "Hemma puede convertirlo una vez en una copia que se reproduce en cualquier navegador, iPhone y tele."},
    {"Fix the sound", "Fixa ljudet", "Arreglar el sonido"},
    {"Make it play in Safari too", "Få den att spelas i Safari också", "Hacer que se reproduzca también en Safari"},
    {"Make it play on more devices", "Få den att spelas på fler enheter", "Hacer que se reproduzca en más dispositivos"},
    {"Make it play everywhere", "Få den att spelas överallt", "Hacer que se reproduzca en todas partes"},
    {"Hemma can make a copy of this that plays everywhere: ask %r.",
     "Hemma kan göra en kopia av den som spelas överallt: fråga %r.",
     "Hemma puede hacer una copia que se reproduzca en todas partes: pregunta a %r."},
    {"This format can't be converted by Hemma yet.", "Hemma kan inte konvertera det här formatet än.",
     "Hemma todavía no puede convertir este formato."},
    {"More episodes", "Fler avsnitt", "Más episodios"},

    /* Library settings */
    {"Where your movies and series live", "Där dina filmer och serier finns", "Donde están tus películas y series"},
    {"One folder per line", "En mapp per rad", "Una carpeta por línea"},
    {"/mnt/media&#10;D:\\Movies", "/mnt/media&#10;D:\\Filmer", "/mnt/media&#10;D:\\Películas"}, /* (an example: one per line) */
    {"On the Raspberry Pi that's where the drive is mounted, e.g. <code class=\"media-code\">/mnt/media</code>. A "
     "network share works too once it's mounted.",
     "På en Raspberry Pi är det där disken är monterad, t.ex. <code class=\"media-code\">/mnt/media</code>. En "
     "nätverksdelning fungerar också när den är monterad.",
     "En la Raspberry Pi es donde está montado el disco, p. ej. <code class=\"media-code\">/mnt/media</code>. Una "
     "carpeta compartida en red también sirve una vez montada."},
    {"Save & scan", "Spara och sök igenom", "Guardar y examinar"},
    {"Found on this machine - tap one to add it:", "Hittade på den här datorn - tryck på en för att lägga till den:",
     "Encontradas en este equipo - toca una para añadirla:"},
    {"No folders yet.", "Inga mappar än.", "Aún no hay carpetas."},
    {"An admin chooses which folders are in the library: ask %r.",
     "En administratör väljer vilka mappar som finns i biblioteket: fråga %r.",
     "Un administrador elige qué carpetas hay en la biblioteca: pregunta a %r."},
    {"Add videos", "Lägg till videor", "Añadir vídeos"},
    {"Upload", "Ladda upp", "Subir"},
    {"Send movies and series from a laptop or PC to the library - whole folders at a time, picking up where it left "
     "off if the connection drops.",
     "Skicka filmer och serier från en bärbar dator eller PC till biblioteket - hela mappar åt gången, och den "
     "fortsätter där den slutade om anslutningen bryts.",
     "Envía películas y series desde un portátil o PC a la biblioteca - carpetas enteras de una vez, y sigue por donde "
     "iba si se corta la conexión."},
    {"Status", "Översikt", "Estado"},
    {"Episodes", "Avsnitt", "Episodios"},
    {"Play everywhere", "Spela överallt", "Reproducir en todas partes"},
    {"%d of %d", "%d av %d", "%d de %d"},
    {"Scanning now — %d file looked at so far.", "Söker igenom nu — %d fil genomgången hittills.",
     "Examinando ahora — %d archivo revisado hasta ahora."},
    {"Scanning now — %d files looked at so far.", "Söker igenom nu — %d filer genomgångna hittills.",
     "Examinando ahora — %d archivos revisados hasta ahora."},
    {"Last scan %r, took %d ms (%d new or changed file read). Hemma rescans every 30 minutes.",
     "Senaste genomsökning %r, tog %d ms (%d ny eller ändrad fil läst). Hemma söker igenom igen var 30:e minut.",
     "Último análisis %r, tardó %d ms (%d archivo nuevo o cambiado leído). Hemma vuelve a examinar cada 30 minutos."},
    {"Last scan %r, took %d ms (%d new or changed files read). Hemma rescans every 30 minutes.",
     "Senaste genomsökning %r, tog %d ms (%d nya eller ändrade filer lästa). Hemma söker igenom igen var 30:e minut.",
     "Último análisis %r, tardó %d ms (%d archivos nuevos o cambiados leídos). Hemma vuelve a examinar cada 30 "
     "minutos."},
    {"Showing the library as it was last time; a fresh scan starts on its own.",
     "Visar biblioteket som det var förra gången; en ny genomsökning startar av sig själv.",
     "Se muestra la biblioteca como estaba la última vez; un análisis nuevo empieza solo."},
    {"Rescan now", "Sök igenom igen nu", "Volver a examinar ahora"},
    {"Admins can always rescan. Who else may:", "Administratörer kan alltid söka igenom igen. Vem mer får:",
     "Los administradores siempre pueden volver a examinar. Quién más puede:"},
    {"Left out", "Utelämnade", "Excluidos"},
    {"%d video is left out of the library: by its name or folder, it looks like trailers, extras or samples (a short "
     "preview clip). A video named like one stays in when it runs long, or when Hemma can't tell how long it runs. "
     "One that belongs in? Rename it, and rescan.",
     "%d video är utelämnad från biblioteket: på namnet eller mappen ser den ut som trailers, extramaterial eller "
     "samples (ett kort förhandsklipp). En video med ett sådant namn får vara med när den är lång, eller när Hemma inte "
     "kan se hur lång den är. Ska den vara med? Byt namn på den och sök igenom igen.",
     "%d vídeo queda fuera de la biblioteca: por su nombre o carpeta, parece un tráiler, un extra o una muestra (un "
     "clip corto de avance). Un vídeo con un nombre así se queda si dura mucho, o si Hemma no sabe cuánto dura. "
     "¿Debería estar? Cámbiale el nombre y vuelve a examinar."},
    {"%d videos are left out of the library: by their name or folder, they look like trailers, extras or samples (a "
     "short preview clip). A video named like one stays in when it runs long, or when Hemma can't tell how long it "
     "runs. One that belongs in? Rename it, and rescan.",
     "%d videor är utelämnade från biblioteket: på namnet eller mappen ser de ut som trailers, extramaterial eller "
     "samples (ett kort förhandsklipp). En video med ett sådant namn får vara med när den är lång, eller när Hemma inte "
     "kan se hur lång den är. Ska någon vara med? Byt namn på den och sök igenom igen.",
     "%d vídeos quedan fuera de la biblioteca: por su nombre o carpeta, parecen tráileres, extras o muestras (un clip "
     "corto de avance). Un vídeo con un nombre así se queda si dura mucho, o si Hemma no sabe cuánto dura. ¿Alguno "
     "debería estar? Cámbiale el nombre y vuelve a examinar."},
    {"Which ones", "Vilka", "Cuáles"},
    {"starts with \"sample\" and runs under 10 minutes: a preview clip",
     "börjar med \"sample\" och är kortare än 10 minuter: ett förhandsklipp",
     "empieza por \"sample\" y dura menos de 10 minutos: un clip de avance"},
    {"named like a trailer or extra (\"-trailer\", \".sample\") and runs under 40 minutes",
     "heter som en trailer eller extramaterial (\"-trailer\", \".sample\") och är kortare än 40 minuter",
     "se llama como un tráiler o un extra (\"-trailer\", \".sample\") y dura menos de 40 minutos"},
    {"in a folder for extras (Extras, Trailers, Featurettes...)",
     "i en mapp för extramaterial (Extras, Trailers, Featurettes...)",
     "en una carpeta de extras (Extras, Trailers, Featurettes...)"},
    {"Unfinished uploads", "Ofärdiga uppladdningar", "Subidas sin terminar"},
    {"%d upload never finished, and nobody has touched it for over a week: %s on the disk.",
     "%d uppladdning blev aldrig klar, och ingen har rört den på över en vecka: %s på disken.",
     "%d subida nunca terminó, y nadie la ha tocado en más de una semana: %s en el disco."},
    {"%d uploads never finished, and nobody has touched them for over a week: %s on the disk.",
     "%d uppladdningar blev aldrig klara, och ingen har rört dem på över en vecka: %s på disken.",
     "%d subidas nunca terminaron, y nadie las ha tocado en más de una semana: %s en el disco."},
    {"%s, last written %s", "%s, senast ändrad %s", "%s, última escritura %s"},
    {"Remove these unfinished uploads? They can't be resumed after.",
     "Ta bort de ofärdiga uppladdningarna? De går inte att återuppta efteråt.",
     "¿Quitar estas subidas sin terminar? Después no se podrán reanudar."},
    {"Remove them", "Ta bort dem", "Quitarlas"},
    {"Removed %d unfinished upload (%s) - %d more show here after the scan",
     "Tog bort %d ofärdig uppladdning (%s) - %d till visas här efter genomsökningen",
     "Se quitó %d subida sin terminar (%s) - %d más aparecen aquí después del análisis"},
    {"Removed %d unfinished uploads (%s) - %d more show here after the scan",
     "Tog bort %d ofärdiga uppladdningar (%s) - %d till visas här efter genomsökningen",
     "Se quitaron %d subidas sin terminar (%s) - %d más aparecen aquí después del análisis"},
    {"Removed %d unfinished upload: %s free again", "Tog bort %d ofärdig uppladdning: %s ledigt igen",
     "Se quitó %d subida sin terminar: %s libres otra vez"},
    {"Removed %d unfinished uploads: %s free again", "Tog bort %d ofärdiga uppladdningar: %s ledigt igen",
     "Se quitaron %d subidas sin terminar: %s libres otra vez"},
    {"Watch history without its video", "Tittarhistorik utan sin video", "Historial sin su vídeo"},
    {"The household watched %d video that isn't in the library any more, and it couldn't be found under another name "
     "or folder.",
     "Hushållet har tittat på %d video som inte finns i biblioteket längre, och den gick inte att hitta under ett annat "
     "namn eller i en annan mapp.",
     "El hogar vio %d vídeo que ya no está en la biblioteca, y no se encontró con otro nombre ni en otra carpeta."},
    {"The household watched %d videos that aren't in the library any more, and they couldn't be found under another "
     "name or folder.",
     "Hushållet har tittat på %d videor som inte finns i biblioteket längre, och de gick inte att hitta under ett annat "
     "namn eller i en annan mapp.",
     "El hogar vio %d vídeos que ya no están en la biblioteca, y no se encontraron con otro nombre ni en otra carpeta."},
    {"A library folder is missing or empty right now: they may come back with its disk.",
     "En biblioteksmapp saknas eller är tom just nu: de kan komma tillbaka med sin disk.",
     "Ahora falta una carpeta de la biblioteca o está vacía: pueden volver con su disco."},
    {"Forget this watch history? Continue watching and watched ticks for these videos go.",
     "Glömma den här tittarhistoriken? Fortsätt titta och sett-markeringar för de här videorna försvinner.",
     "¿Olvidar este historial? Se pierden Seguir viendo y las marcas de visto de estos vídeos."},
    {"Forget it", "Glöm den", "Olvidarlo"},
    {"Forgot the history of %d video", "Glömde historiken för %d video", "Se olvidó el historial de %d vídeo"},
    {"Forgot the history of %d videos", "Glömde historiken för %d videor", "Se olvidó el historial de %d vídeos"},
    {"Children see the whole library. To keep a folder out of their Watch, open it under "
     "<a href=\"/media/browse\">Folders</a> and tick <i>Not for children</i>.",
     "Barn ser hela biblioteket. För att hålla en mapp borta från deras Titta, öppna den under "
     "<a href=\"/media/browse\">Mappar</a> och kryssa i <i>Inte för barn</i>.",
     "Los niños ven toda la biblioteca. Para dejar una carpeta fuera de su Ver, ábrela en "
     "<a href=\"/media/browse\">Carpetas</a> y marca <i>No apto para niños</i>."},
    {"Left out of a child's Watch - and of the TV while a child is picked there:",
     "Utelämnat från ett barns Titta - och från TV:n medan ett barn är valt där:",
     "Fuera de Ver para los niños - y de la tele mientras haya un niño elegido en ella:"},
    {"Open %s to children", "Öppna %s för barn", "Abrir %s a los niños"},
    {"Open to children", "Öppna för barn", "Abrir a los niños"},
    {"Playback", "Uppspelning", "Reproducción"},
    {"Subtitles on by default", "Undertexter på som standard", "Subtítulos activados por defecto"},
    {"Off", "Av", "Desactivado"},
    {"Just for %s. Subtitle files next to a video (<code class=\"media-code\">Movie.sv.srt</code>) are picked up "
     "automatically.",
     "Bara för %s. Undertextfiler bredvid en video (<code class=\"media-code\">Movie.sv.srt</code>) hittas "
     "automatiskt.",
     "Solo para %s. Los archivos de subtítulos junto a un vídeo (<code class=\"media-code\">Movie.sv.srt</code>) se "
     "detectan automáticamente."},
    {"What plays where", "Vad spelas var", "Qué se reproduce dónde"},
    {"Most videos play straight away. Some - DVD rips especially - need converting first: one button (<i>Play "
     "everywhere</i> above) makes a copy that plays on every phone, computer and TV.",
     "De flesta videor spelas direkt. Vissa - särskilt DVD-rippar - behöver konverteras först: en knapp (<i>Spela "
     "överallt</i> ovan) gör en kopia som spelas på alla telefoner, datorer och TV-apparater.",
     "La mayoría de los vídeos se reproducen enseguida. Algunos - sobre todo los ripeos de DVD - hay que convertirlos "
     "primero: un botón (<i>Reproducir en todas partes</i> arriba) hace una copia que se reproduce en cualquier móvil, "
     "ordenador y tele."},
    {"On every screen.", "På alla skärmar.", "En todas las pantallas."},
    {"On computers and most phones, but a TV's browser or an iPhone may refuse it. <i>Plays on newer devices</i>: "
     "most from the last few years.",
     "På datorer och de flesta telefoner, men en TV:s webbläsare eller en iPhone kan vägra. <i>Spelas på nyare "
     "enheter</i>: de flesta från de senaste åren.",
     "En ordenadores y la mayoría de móviles, pero el navegador de una tele o un iPhone pueden rechazarlo. <i>Se "
     "reproduce en dispositivos más nuevos</i>: la mayoría de los últimos años."},
    {"Nowhere until Hemma has made its copy.", "Ingenstans förrän Hemma har gjort sin kopia.",
     "En ningún sitio hasta que Hemma haya hecho su copia."},
    {"Which formats are which", "Vilka format som är vilka", "Qué formatos son cuáles"},
    {"<b>Plays in browsers:</b> MP4 or WebM with H.264/VP9 video and AAC/MP3/Opus sound.",
     "<b>Spelas i webbläsare:</b> MP4 eller WebM med H.264/VP9-video och AAC/MP3/Opus-ljud.",
     "<b>Se reproduce en navegadores:</b> MP4 o WebM con vídeo H.264/VP9 y sonido AAC/MP3/Opus."},
    {"<b>May not play everywhere:</b> MKV, HEVC, AV1, or Dolby sound (Dolby Digital and Digital Plus: AC-3, E-AC-3) — "
     "fine in Chrome/Edge or Safari, not all of them; Dolby sound plays silently in Chrome and Firefox. An HEVC or AV1 "
     "video converted here (<i>Plays on newer devices</i>) plays on most devices from the last few years.",
     "<b>Spelas kanske inte överallt:</b> MKV, HEVC, AV1 eller Dolby-ljud (Dolby Digital och Digital Plus: AC-3, "
     "E-AC-3) — fungerar i Chrome/Edge eller Safari, inte i alla; Dolby-ljud blir tyst i Chrome och Firefox. En HEVC- "
     "eller AV1-video som konverterats här (<i>Spelas på nyare enheter</i>) spelas på de flesta enheter från de "
     "senaste åren.",
     "<b>Puede que no se reproduzca en todas partes:</b> MKV, HEVC, AV1 o sonido Dolby (Dolby Digital y Digital Plus: "
     "AC-3, E-AC-3) — funciona en Chrome/Edge o Safari, no en todos; el sonido Dolby no se oye en Chrome ni en Firefox. "
     "Un vídeo HEVC o AV1 convertido aquí (<i>Se reproduce en dispositivos más nuevos</i>) se reproduce en la mayoría "
     "de dispositivos de los últimos años."},
    {"<b>Needs conversion:</b> MPEG-2 (DVD rips), AVI/XviD, DTS sound. Hemma converts MKV files with MPEG-2, H.264, "
     "HEVC or AV1 video and Dolby, DTS or AAC sound itself, and keeps their subtitles next to them.",
     "<b>Behöver konverteras:</b> MPEG-2 (DVD-rippar), AVI/XviD, DTS-ljud. Hemma konverterar själv MKV-filer med "
     "MPEG-2-, H.264-, HEVC- eller AV1-video och Dolby-, DTS- eller AAC-ljud, och sparar deras undertexter bredvid dem.",
     "<b>Necesita conversión:</b> MPEG-2 (ripeos de DVD), AVI/XviD, sonido DTS. Hemma convierte por sí mismo archivos "
     "MKV con vídeo MPEG-2, H.264, HEVC o AV1 y sonido Dolby, DTS o AAC, y guarda sus subtítulos a su lado."},
    {"Nothing here", "Här finns inget", "Aquí no hay nada"},
    {"That page doesn't exist.", "Den sidan finns inte.", "Esa página no existe."},

    /* Continue watching's x, the home card */
    {"Remove from Continue watching", "Ta bort från Fortsätt titta", "Quitar de Seguir viendo"},
    {"Remove %s from Continue watching", "Ta bort %s från Fortsätt titta", "Quitar %s de Seguir viendo"},
    {"Set up", "Ställ in", "Configurar"},
    {"Point Hemma at the drive with your movies and series, and everyone can watch them on any screen at home — "
     "offline.",
     "Visa Hemma disken med dina filmer och serier, så kan alla titta på dem på vilken skärm som helst hemma — utan "
     "internet.",
     "Indica a Hemma el disco con tus películas y series, y todos podrán verlas en cualquier pantalla de casa — sin "
     "internet."},

    /* what actions say */
    {"That video isn't in the library any more", "Den videon finns inte i biblioteket längre",
     "Ese vídeo ya no está en la biblioteca"},
    {"Marked as watched", "Markerad som sedd", "Marcado como visto"},
    {"Marked as not watched", "Markerad som inte sedd", "Marcado como no visto"},
    {"Rescanning is for admins - ask one to rescan, or to let you do it",
     "Att söka igenom igen är för administratörer - be en att göra det, eller att låta dig göra det",
     "Volver a examinar es cosa de los administradores - pide a uno que lo haga, o que te deje hacerlo"},
    {"Library scanned", "Biblioteket genomsökt", "Biblioteca examinada"},
    {"Already scanning — hang on", "Söker redan igenom — vänta lite", "Ya se está examinando — espera un poco"},
    {"Scanning your library…", "Söker igenom ditt bibliotek…", "Examinando tu biblioteca…"},
    {"Only an admin can do that", "Bara en administratör kan göra det", "Solo un administrador puede hacer eso"},
    {"Only an admin can change pictures", "Bara en administratör kan byta bilder",
     "Solo un administrador puede cambiar las imágenes"},
    {"That isn't in the library any more", "Det finns inte i biblioteket längre", "Eso ya no está en la biblioteca"},
    {"Picture removed", "Bilden borttagen", "Imagen quitada"},
    {"There was no picture to remove", "Det fanns ingen bild att ta bort", "No había ninguna imagen que quitar"},
    {"Only an admin can remove things from the library", "Bara en administratör kan ta bort saker från biblioteket",
     "Solo un administrador puede quitar cosas de la biblioteca"},
    {"It's being converted right now - stop that first (Library settings)",
     "Den konverteras just nu - stoppa det först (Biblioteksinställningar)",
     "Se está convirtiendo ahora mismo - detenlo primero (Ajustes de la biblioteca)"},
    {"Only films and other videos can be removed here", "Bara filmer och andra videor kan tas bort här",
     "Aquí solo se pueden quitar películas y otros vídeos"},
    {"Couldn't delete it - is it playing somewhere? Try again in a moment.",
     "Kunde inte radera den - spelas den någonstans? Försök igen om en stund.",
     "No se pudo borrar - ¿se está reproduciendo en algún sitio? Vuelve a intentarlo en un momento."},
    {"removed %s from the library", "tog bort %s från biblioteket", "quitó %s de la biblioteca"},
    {"Removed %s - %d leftover file couldn't be deleted", "Tog bort %s - %d kvarlämnad fil gick inte att radera",
     "Se quitó %s - %d archivo restante no se pudo borrar"},
    {"Removed %s - %d leftover files couldn't be deleted", "Tog bort %s - %d kvarlämnade filer gick inte att radera",
     "Se quitó %s - %d archivos restantes no se pudieron borrar"},
    {"Removed %s - %s freed", "Tog bort %s - %s frigjort", "Se quitó %s - %s liberados"},
    {"Only an admin can delete originals", "Bara en administratör kan radera original",
     "Solo un administrador puede borrar originales"},
    {"Deleted %d file (%s) - %d couldn't be deleted", "Raderade %d fil (%s) - %d gick inte att radera",
     "Se borró %d archivo (%s) - %d no se pudieron borrar"},
    {"Deleted %d files (%s) - %d couldn't be deleted", "Raderade %d filer (%s) - %d gick inte att radera",
     "Se borraron %d archivos (%s) - %d no se pudieron borrar"},
    {"Deleted %d file - %s freed", "Raderade %d fil - %s frigjort", "Se borró %d archivo - %s liberados"},
    {"Deleted %d files - %s freed", "Raderade %d filer - %s frigjort", "Se borraron %d archivos - %s liberados"},
    {"Couldn't delete them (in use, or no permission?)", "Kunde inte radera dem (används de, eller saknas behörighet?)",
     "No se pudieron borrar (¿en uso, o sin permiso?)"},
    {"Nothing to delete: no video has two versions", "Inget att radera: ingen video har två versioner",
     "Nada que borrar: ningún vídeo tiene dos versiones"},
    {"Removed %s from Continue watching", "Tog bort %s från Fortsätt titta", "Se quitó %s de Seguir viendo"},
    {"Removed it from Continue watching", "Tog bort den från Fortsätt titta", "Se quitó de Seguir viendo"},
    {"Only an admin can change that", "Bara en administratör kan ändra det", "Solo un administrador puede cambiar eso"},
    {"After each conversion only the most compatible version stays",
     "Efter varje konvertering blir bara den mest kompatibla versionen kvar",
     "Tras cada conversión solo se queda la versión más compatible"},
    {"Both versions stay after converting", "Båda versionerna blir kvar efter konverteringen",
     "Las dos versiones se quedan tras convertir"},
    {"Only an admin can choose who may rescan", "Bara en administratör kan välja vem som får söka igenom igen",
     "Solo un administrador puede elegir quién puede volver a examinar"},
    {"%d member may rescan the library too", "%d medlem får också söka igenom biblioteket igen",
     "%d miembro también puede volver a examinar la biblioteca"},
    {"%d members may rescan the library too", "%d medlemmar får också söka igenom biblioteket igen",
     "%d miembros también pueden volver a examinar la biblioteca"},
    {"Only admins may rescan the library", "Bara administratörer får söka igenom biblioteket igen",
     "Solo los administradores pueden volver a examinar la biblioteca"},
    {"Can't find the folder \"%s\" on the server", "Hittar inte mappen \"%s\" på servern",
     "No se encuentra la carpeta \"%s\" en el servidor"},
    {"That's more than %d folders — point at a parent folder instead",
     "Det är fler än %d mappar — välj en överordnad mapp i stället",
     "Son más de %d carpetas — indica una carpeta superior en su lugar"},
    {"Saved — scanning %d folder", "Sparat — söker igenom %d mapp", "Guardado — examinando %d carpeta"},
    {"Saved — scanning %d folders", "Sparat — söker igenom %d mappar", "Guardado — examinando %d carpetas"},
    {"Saved — no folders", "Sparat — inga mappar", "Guardado — sin carpetas"},
    {"Only an admin can change the library folders", "Bara en administratör kan ändra biblioteksmapparna",
     "Solo un administrador puede cambiar las carpetas de la biblioteca"},
    {"%s is in the library already", "%s finns redan i biblioteket", "%s ya está en la biblioteca"},
    {"Pick a folder", "Välj en mapp", "Elige una carpeta"},
    {"That folder isn't in the library", "Den mappen finns inte i biblioteket", "Esa carpeta no está en la biblioteca"},
    {"%s is not for children now", "%s är inte för barn nu", "%s ya no es apto para niños"},
    {"%s is open to children", "%s är öppen för barn", "%s está abierto a los niños"},
    {"Subtitles: %s", "Undertexter: %s", "Subtítulos: %s"},
    {"Subtitles off by default", "Undertexter av som standard", "Subtítulos desactivados por defecto"},
    {"What they watched goes (%d video)", "Det hen har tittat på försvinner (%d video)", "Lo que vio se borra (%d vídeo)"},
    {"What they watched goes (%d videos)", "Det hen har tittat på försvinner (%d videor)",
     "Lo que vio se borra (%d vídeos)"},

    /* the storage meter above the profile */
    {"Space on the disk with the library (%s)", "Utrymme på disken med biblioteket (%s)",
     "Espacio en el disco de la biblioteca (%s)"},
    {"Storage", "Lagring", "Almacenamiento"},
    {"%s free", "%s ledigt", "%s libres"},
    {"%s used of %s", "%s använt av %s", "%s usados de %s"},

    /* Watch on the TV: rows (screens' builder names them) and tiles */
    {"A series", "En serie", "Una serie"},
    {"%s left", "%s kvar", "quedan %s"},
    {"Watching as %s", "Tittar som %s", "Viendo como %s"},
    {"All episodes", "Alla avsnitt", "Todos los episodios"},

    /* the System page's checks */
    {"Films and series", "Filmer och serier", "Películas y series"},
    {"No library folders yet.", "Inga biblioteksmappar än.", "Aún no hay carpetas en la biblioteca."},
    {"Add the folders with your videos under Watch → Library settings.",
     "Lägg till mapparna med dina videor under Titta → Biblioteksinställningar.",
     "Añade las carpetas con tus vídeos en Ver → Ajustes de la biblioteca."},
    {"Hemma can't find the library folder %s.", "Hemma hittar inte biblioteksmappen %s.",
     "Hemma no encuentra la carpeta de la biblioteca %s."},
    {"Library folder missing", "Biblioteksmapp saknas", "Falta una carpeta de la biblioteca"},
    {"Is the disk plugged in and mounted? Then rescan under Watch → Library settings.",
     "Är disken inkopplad och monterad? Sök sedan igenom igen under Titta → Biblioteksinställningar.",
     "¿Está el disco conectado y montado? Luego vuelve a examinar en Ver → Ajustes de la biblioteca."},
    {"Hemma isn't allowed to look inside %s.", "Hemma får inte titta i %s.", "Hemma no tiene permiso para mirar dentro de %s."},
    {"Library folder locked", "Biblioteksmapp låst", "Carpeta de la biblioteca bloqueada"},
    {"Let the user Hemma runs as read that folder (on a Pi: run the installer again with it).",
     "Låt användaren som Hemma körs som läsa mappen (på en Pi: kör installationen igen med den).",
     "Deja que el usuario con el que se ejecuta Hemma lea esa carpeta (en una Pi: vuelve a ejecutar el instalador con "
     "ella)."},
    {"Hemma may only read %s: uploads and converted copies can't be saved there.",
     "Hemma får bara läsa %s: uppladdningar och konverterade kopior kan inte sparas där.",
     "Hemma solo puede leer %s: ahí no se pueden guardar subidas ni copias convertidas."},
    {"Read-only library folder", "Skrivskyddad biblioteksmapp", "Carpeta de la biblioteca de solo lectura"},
    {"Fine for watching. For uploads, run the installer again with this folder (docs/raspberry-pi.md, step 7).",
     "Bra för att titta. För uppladdningar, kör installationen igen med den här mappen (docs/raspberry-pi.md, steg 7).",
     "Sirve para ver. Para subir, vuelve a ejecutar el instalador con esta carpeta (docs/raspberry-pi.md, paso 7)."},
    {"to upload or convert a video as big as its biggest (%s)",
     "för att ladda upp eller konvertera en video lika stor som dess största (%s)",
     "para subir o convertir un vídeo tan grande como el mayor que tiene (%s)"},
    {"for new videos", "för nya videor", "para vídeos nuevos"},
    {"the library", "biblioteket", "la biblioteca"},
    {"Delete or move some videos - or keep only the most compatible version of converted ones (Library settings).",
     "Radera eller flytta några videor - eller behåll bara den mest kompatibla versionen av konverterade (Biblioteksinställningar).",
     "Borra o mueve algunos vídeos - o conserva solo la versión más compatible de los convertidos (Ajustes de la "
     "biblioteca)."},
    {"%d folder", "%d mapp", "%d carpeta"},
    {"%d folders", "%d mappar", "%d carpetas"},
    {"%s, %s so far (looking through them now)", "%s, %s hittills (går igenom dem nu)",
     "%s, %s hasta ahora (revisándolos ahora)"},
    {"%s free on the fullest disk", "%s ledigt på den fullaste disken", "%s libres en el disco más lleno"},
    {"The history of %d video couldn't be matched to one in the library",
     "Historiken för %d video kunde inte kopplas till någon i biblioteket",
     "El historial de %d vídeo no se pudo asociar con ninguno de la biblioteca"},
    {"The history of %d videos couldn't be matched to one in the library",
     "Historiken för %d videor kunde inte kopplas till några i biblioteket",
     "El historial de %d vídeos no se pudo asociar con ninguno de la biblioteca"},
    {"Watch history", "Tittarhistorik", "Historial"},
    {"Deleted or replaced on purpose? Library settings can forget it.",
     "Raderade eller ersatta med flit? Biblioteksinställningar kan glömma den.",
     "¿Borrados o sustituidos a propósito? Ajustes de la biblioteca puede olvidarlo."},
    {"%d video didn't fit on the drive, so it wasn't converted.",
     "%d video fick inte plats på disken, så den konverterades inte.",
     "%d vídeo no cabía en el disco, así que no se convirtió."},
    {"%d videos didn't fit on the drive, so they weren't converted.",
     "%d videor fick inte plats på disken, så de konverterades inte.",
     "%d vídeos no cabían en el disco, así que no se convirtieron."},
    {"Not enough room to convert", "Inte tillräckligt med plats för att konvertera",
     "No hay espacio suficiente para convertir"},
    {"Make room on that drive, then convert again under Watch → Library settings.",
     "Gör plats på disken och konvertera sedan igen under Titta → Biblioteksinställningar.",
     "Libera espacio en ese disco y vuelve a convertir en Ver → Ajustes de la biblioteca."},
    {"%d conversion failed.", "%d konvertering misslyckades.", "%d conversión falló."},
    {"%d conversions failed.", "%d konverteringar misslyckades.", "%d conversiones fallaron."},
    {"Conversions failed", "Konverteringar misslyckades", "Conversiones fallidas"},
    {"Library settings lists them, each with the reason.", "Biblioteksinställningar listar dem, var och en med orsaken.",
     "Ajustes de la biblioteca las enumera, cada una con su motivo."},
    {"Built in: videos that not every phone or TV plays can get a copy that plays everywhere. Uploads convert on the "
     "sending device first.",
     "Inbyggt: videor som inte alla telefoner eller TV-apparater spelar kan få en kopia som spelas överallt. "
     "Uppladdningar konverteras först på enheten som skickar.",
     "Integrado: los vídeos que no reproducen todos los móviles o teles pueden tener una copia que se reproduce en todas "
     "partes. Las subidas se convierten primero en el dispositivo que las envía."},
    {"Built in: videos that not every phone or TV plays can get a copy that plays everywhere. This build has no "
     "converter for browsers, so uploads send videos as they are.",
     "Inbyggt: videor som inte alla telefoner eller TV-apparater spelar kan få en kopia som spelas överallt. Det här "
     "bygget har ingen konverterare för webbläsare, så uppladdningar skickar videor som de är.",
     "Integrado: los vídeos que no reproducen todos los móviles o teles pueden tener una copia que se reproduce en todas "
     "partes. Esta versión no tiene conversor para navegadores, así que las subidas envían los vídeos tal cual."},
    {"Add your films and series", "Lägg till dina filmer och serier", "Añade tus películas y series"},
    {"Point Hemma at the folders with your videos - or send them from a computer or phone.",
     "Visa Hemma mapparna med dina videor - eller skicka dem från en dator eller telefon.",
     "Indica a Hemma las carpetas con tus vídeos - o envíalos desde un ordenador o un móvil."},

    /* the probe's verdicts (probe.c; short: they fit in 95 bytes) */
    {"couldn't read the file header (incomplete or damaged?)", "kunde inte läsa filhuvudet (ofullständig eller skadad?)",
     "no se pudo leer la cabecera del archivo (¿incompleto o dañado?)"},
    {"unrecognised file format", "okänt filformat", "formato de archivo desconocido"},
    {"HEVC needs hardware support (Safari/Edge, some Chrome)", "HEVC kräver hårdvarustöd (Safari/Edge, viss Chrome)",
     "HEVC necesita soporte de hardware (Safari/Edge, algún Chrome)"},
    {"AV1 needs a newer device (Chrome, Edge, Firefox; the newest Apple devices)",
     "AV1 kräver en nyare enhet (Chrome, Edge, Firefox; de nyaste från Apple)",
     "AV1 necesita un dispositivo más nuevo (Chrome, Edge, Firefox; los Apple más nuevos)"},
    {"%s in %s doesn't play everywhere", "%s i %s spelas inte överallt", "%s en %s no se reproduce en todas partes"},
    {"%s in this container doesn't play everywhere", "%s i den här behållaren spelas inte överallt",
     "%s en este contenedor no se reproduce en todas partes"},
    {"%s video needs conversion", "%s-video behöver konverteras", "El vídeo %s necesita conversión"},
    {"Vorbis audio outside WebM doesn't play everywhere", "Vorbis-ljud utanför WebM spelas inte överallt",
     "El audio Vorbis fuera de WebM no se reproduce en todas partes"},
    {"Dolby sound: Safari/Edge only - silent in Chrome and Firefox", "Dolby-ljud: bara Safari/Edge - tyst i Chrome och Firefox",
     "Sonido Dolby: solo Safari/Edge - sin sonido en Chrome y Firefox"},
    {"DTS audio isn't supported by browsers", "DTS-ljud stöds inte av webbläsare", "Los navegadores no admiten audio DTS"},
    {"TrueHD audio isn't supported by browsers", "TrueHD-ljud stöds inte av webbläsare",
     "Los navegadores no admiten audio TrueHD"},
    {"ALAC audio: Safari only", "ALAC-ljud: bara Safari", "Audio ALAC: solo Safari"},
    {"uncompressed PCM audio may not play", "okomprimerat PCM-ljud kanske inte spelas",
     "puede que el audio PCM sin comprimir no se reproduzca"},
    {"%s audio isn't supported by browsers", "%s-ljud stöds inte av webbläsare", "Los navegadores no admiten audio %s"},
    {"MKV plays in Chrome/Edge, not Safari/iOS", "MKV spelas i Chrome/Edge, inte i Safari/iOS",
     "MKV se reproduce en Chrome/Edge, no en Safari/iOS"},
    {"AVI needs conversion", "AVI behöver konverteras", "AVI necesita conversión"},
    {"MPEG-TS needs conversion", "MPEG-TS behöver konverteras", "MPEG-TS necesita conversión"},
    {"WMV needs conversion", "WMV behöver konverteras", "WMV necesita conversión"},
    {"MPEG-PS needs conversion", "MPEG-PS behöver konverteras", "MPEG-PS necesita conversión"},
    {"unknown container", "okänd behållare", "contenedor desconocido"},
    /* the drive's room (room.c media_room_text: the end of a sentence, said when it's shown) */
    {"the drive has %s to spare (Hemma keeps %lld GB free, and %s for the uploads and conversions running now)",
     "disken har %s över (Hemma håller %lld GB ledigt, och %s för uppladdningarna och konverteringarna som pågår nu)",
     "el disco tiene %s de sobra (Hemma deja %lld GB libres, y %s para las subidas y conversiones en curso)"},
    {"the drive has %s to spare (Hemma keeps %lld GB free)", "disken har %s över (Hemma håller %lld GB ledigt)",
     "el disco tiene %s de sobra (Hemma deja %lld GB libres)"},
    /* offline in the app (2.3) */
    {"Save for offline", "Spara offline", "Guardar sin conexión"},
    {"Save season for offline", "Spara säsongen offline", "Guardar la temporada sin conexión"},
    {"Save the specials for offline", "Spara specialavsnitten offline", "Guardar los especiales sin conexión"},
    {"Offline", "Offline", "Sin conexión"},
    {"Saving for offline isn't open to guests or screens.", "Att spara offline går inte för gäster eller skärmar.",
     "Guardar sin conexión no está disponible para invitados ni pantallas."},
    /* sound languages (2.2) */
    {"Sound %d", "Ljud %d", "Sonido %d"},
    {"Which language do you want to watch in?", "Vilket språk vill du titta på?", "¿En qué idioma quieres verlo?"},
    {"This video has sound in several languages. Hemma remembers your choice for every video - the player's Language "
     "menu changes it for one.",
     "Den här videon har ljud på flera språk. Hemma kommer ihåg ditt val för alla videor - spelarens Språk-meny ändrar "
     "det för en.",
     "Este vídeo tiene sonido en varios idiomas. Hemma recuerda tu elección para todos los vídeos; el menú Idioma del "
     "reproductor la cambia para uno."},
    {"As each video comes", "Som varje video är", "Como venga cada vídeo"},
    {"Pick a language", "Välj ett språk", "Elige un idioma"},
    {"Sound: %s", "Ljud: %s", "Sonido: %s"},
    {"Each video plays in its own language", "Varje video spelas på sitt eget språk", "Cada vídeo se reproduce en su idioma"},
    {"Hemma asks again at the next video with several languages", "Hemma frågar igen vid nästa video med flera språk",
     "Hemma vuelve a preguntar en el próximo vídeo con varios idiomas"},
    {"Sound language", "Ljudspråk", "Idioma del sonido"},
    {"Ask me", "Fråga mig", "Pregúntame"},
    {"For videos with sound in several languages. A video without yours plays in its own.",
     "För videor med ljud på flera språk. En video utan ditt spelas på sitt eget.",
     "Para vídeos con sonido en varios idiomas. Un vídeo sin el tuyo se reproduce en el suyo."},
    /* films without a year (2.4) */
    {"Films without a year", "Filmer utan år", "Películas sin año"},
    {"%d film has no year in its name. The year tells films apart - The Lion King (1994) from The Lion King (2019) - "
     "and finds its categories: add it with Fix title.",
     "%d film har inget år i namnet. Året skiljer filmer åt - Lejonkungen (1994) från Lejonkungen (2019) - och hittar "
     "dess kategorier: lägg till det med Rätta titeln.",
     "%d película no tiene año en su nombre. El año distingue las películas - El rey león (1994) de El rey león "
     "(2019) - y encuentra sus categorías: añádelo con Corregir título."},
    {"%d films have no year in their names. The year tells films apart - The Lion King (1994) from The Lion King "
     "(2019) - and finds their categories: add it with Fix title.",
     "%d filmer har inget år i namnet. Året skiljer filmer åt - Lejonkungen (1994) från Lejonkungen (2019) - och "
     "hittar deras kategorier: lägg till det med Rätta titeln.",
     "%d películas no tienen año en su nombre. El año distingue las películas - El rey león (1994) de El rey león "
     "(2019) - y encuentra sus categorías: añádelo con Corregir título."},

    /* what your devices play (devices.c, 2.5) */
    {"Play on your devices", "Spelas på era enheter", "Se reproducen en tus dispositivos"},
    {"What your devices play", "Vad era enheter spelar", "Qué reproducen tus dispositivos"},
    {"The videos that don't play on every device there is, and why:",
     "Videorna som inte spelas på alla enheter som finns, och varför:",
     "Los vídeos que no se reproducen en todos los dispositivos que hay, y por qué:"},
    {"Every video plays on every device there is.", "Varje video spelas på alla enheter som finns.",
     "Todos los vídeos se reproducen en todos los dispositivos que hay."},
    {"The household's devices", "Hushållets enheter", "Los dispositivos de casa"},
    {"None yet: each phone, computer and TV says what it plays once it opens Watch.",
     "Inga än: varje telefon, dator och tv säger vad den spelar när den öppnar Titta.",
     "Ninguno aún: cada móvil, ordenador y tele dice qué reproduce cuando abre Ver."},
    {"Each one that opened Watch in the last %d day:", "Var och en som öppnat Titta det senaste %d dygnet:",
     "Cada uno que abrió Ver en el último %d día:"},
    {"Each one that opened Watch in the last %d days:", "Var och en som öppnat Titta de senaste %d dagarna:",
     "Cada uno que abrió Ver en los últimos %d días:"},
    {"plays them all", "spelar alla", "los reproduce todos"},
    {"plays %d of %d", "spelar %d av %d", "reproduce %d de %d"},
    {"not %d %s", "inte %d %s", "no %d %s"},
    {"%d of %d videos play on this device.", "%d av %d videor spelas på den här enheten.",
     "%d de %d vídeos se reproducen en este dispositivo."},
    {"%d of %d videos play on every one of these %d devices.", "%d av %d videor spelas på alla dessa %d enheter.",
     "%d de %d vídeos se reproducen en cada uno de estos %d dispositivos."},
    {"Plays on the one device of yours that has opened Watch", "Spelas på den enda av era enheter som öppnat Titta",
     "Se reproduce en el único de tus dispositivos que ha abierto Ver"},
    {"Plays on every device of yours", "Spelas på alla era enheter", "Se reproduce en todos tus dispositivos"},
    {"Doesn't play on: %s", "Spelas inte på: %s", "No se reproduce en: %s"},
    /* (what a device can't play, after "not 1250 ...") */
    {"files browsers don't open", "filer som webbläsare inte öppnar", "archivos que los navegadores no abren"},
    {"unreadable files", "oläsbara filer", "archivos ilegibles"},
    {"MKV", "MKV", "MKV"},
    {"H.264", "H.264", "H.264"},
    {"HEVC (x265)", "HEVC (x265)", "HEVC (x265)"},
    {"AV1", "AV1", "AV1"},
    {"VP9", "VP9", "VP9"},
    {"pictures browsers don't play", "bilder som webbläsare inte spelar", "imágenes que los navegadores no reproducen"},
    {"AAC sound", "AAC-ljud", "sonido AAC"},
    {"MP3 sound", "MP3-ljud", "sonido MP3"},
    {"Opus sound", "Opus-ljud", "sonido Opus"},
    {"FLAC sound", "FLAC-ljud", "sonido FLAC"},
    {"Vorbis sound", "Vorbis-ljud", "sonido Vorbis"},
    {"Dolby sound", "Dolby-ljud", "sonido Dolby"},
    {"DTS sound", "DTS-ljud", "sonido DTS"},
    {"TrueHD sound", "TrueHD-ljud", "sonido TrueHD"},
    {"sound browsers don't play", "ljud webbläsare inte spelar", "sonido que los navegadores no reproducen"},
};
TRANSLATIONS(media, TR)

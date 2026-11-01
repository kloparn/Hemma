/* modules/media/manage.c's words in Swedish and Spanish (core/i18n.h says how). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the Manage view of a series */
    {"Manage", "Hantera", "Gestionar"},
    {"Rename the series, fix episode numbers, or remove episodes, seasons or the whole series",
     "Byt namn på serien, rätta avsnittsnummer, eller ta bort avsnitt, säsonger eller hela serien",
     "Renombra la serie, corrige números de episodio, o quita episodios, temporadas o la serie entera"},
    {"Specials", "Specialavsnitt", "Especiales"},
    {"Original", "Original", "Original"},
    {"Fix number", "Rätta numret", "Corregir número"},
    {"Remove season %d (%s)", "Ta bort säsong %d (%s)", "Quitar la temporada %d (%s)"},
    {"Remove the specials (%s)", "Ta bort specialavsnitten (%s)", "Quitar los especiales (%s)"},
    {"The whole series", "Hela serien", "La serie entera"},
    {"Remove the series (%s)", "Ta bort serien (%s)", "Quitar la serie (%s)"},
    {"Removing %s…", "Tar bort %s…", "Quitando %s…"},

    /* removing an episode, a season, a series */
    {"Remove the episode", "Ta bort avsnittet", "Quitar el episodio"},
    {"Remove the extra", "Ta bort extramaterialet", "Quitar el extra"},
    {"Remove the season", "Ta bort säsongen", "Quitar la temporada"},
    {"Remove the specials", "Ta bort specialavsnitten", "Quitar los especiales"},
    {"Remove the series", "Ta bort serien", "Quitar la serie"},
    {"Season %d of <b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone.",
     "Säsong %d av <b>%s</b> raderas från disken - för alla i hushållet, och det går inte att ångra.",
     "La temporada %d de <b>%s</b> se borra del disco - para todo el hogar, y no se puede deshacer."},
    {"The specials of <b>%s</b> are deleted from the disk - for everyone in the household, and it can't be undone.",
     "Specialavsnitten av <b>%s</b> raderas från disken - för alla i hushållet, och det går inte att ångra.",
     "Los especiales de <b>%s</b> se borran del disco - para todo el hogar, y no se puede deshacer."},
    {"Its folder <code class=\"media-code\">%s</code> goes too, with what's left in it - unless another video is in there.",
     "Mappen <code class=\"media-code\">%s</code> försvinner också, med det som finns kvar i den - om inte en annan video ligger där.",
     "Su carpeta <code class=\"media-code\">%s</code> también se va, con lo que quede dentro - salvo que haya otro vídeo en ella."},
    {"Its %d folders go too, with what's left in them - unless another video is in there.",
     "Dess %d mappar försvinner också, med det som finns kvar i dem - om inte en annan video ligger där.",
     "Sus %d carpetas también se van, con lo que quede dentro - salvo que haya otro vídeo en ellas."},
    {"Hemma is still removing %s - try again in a moment", "Hemma tar fortfarande bort %s - försök igen om en stund",
     "Hemma todavía está quitando %s - vuelve a intentarlo en un momento"},

    /* fixing an episode's number */
    {"Fix the episode number", "Rätta avsnittsnumret", "Corregir el número del episodio"},
    {"Hemma renames its files to a name it reads right - like <code class=\"media-code\">%s</code> - and moves them into "
     "the season's folder.",
     "Hemma byter namn på filerna till ett namn den läser rätt - som <code class=\"media-code\">%s</code> - och flyttar dem "
     "till säsongens mapp.",
     "Hemma renombra sus archivos con un nombre que lee bien - como <code class=\"media-code\">%s</code> - y los mueve a la "
     "carpeta de la temporada."},
    {"On a ripped disc, the titles without a number of their own are numbered in disc order, after the season's highest "
     "number.",
     "På en rippad skiva numreras titlarna utan eget nummer i skivans ordning, efter säsongens högsta nummer.",
     "En un disco copiado, los títulos sin número propio se numeran en el orden del disco, después del número más alto "
     "de la temporada."},
    {"Season", "Säsong", "Temporada"},
    {"Episode", "Avsnitt", "Episodio"},
    {"Last episode, when the file holds more than one", "Sista avsnittet, när filen har fler än ett",
     "Último episodio, si el archivo tiene más de uno"},
    {"Episode title (optional)", "Avsnittets titel (frivilligt)", "Título del episodio (opcional)"},
    {"A season number: 0 for specials, else 1 and up", "Ett säsongsnummer: 0 för specialavsnitt, annars 1 och uppåt",
     "Un número de temporada: 0 para los especiales, si no 1 o más"},
    {"An episode number: 1 and up", "Ett avsnittsnummer: 1 och uppåt", "Un número de episodio: 1 o más"},
    {"The last episode comes after the first - or leave it empty", "Sista avsnittet kommer efter det första - eller lämna tomt",
     "El último episodio va después del primero - o déjalo vacío"},
    {"That title has characters a disk can't store", "Titeln har tecken som en disk inte kan spara",
     "Ese título tiene caracteres que un disco no puede guardar"},

    /* fixing a film's title */
    {"Fix title", "Rätta titeln", "Corregir título"},
    {"Fix its title and year", "Rätta titel och år", "Corrige su título y año"},
    {"Fix the title", "Rätta titeln", "Corregir el título"},
    {"Its files - the video, its copy, subtitles and picture - are renamed like <code class=\"media-code\">%s</code>, and so "
     "is a folder of its own.",
     "Filerna - videon, kopian, undertexterna och bilden - får namn som <code class=\"media-code\">%s</code>, och likaså en "
     "mapp som är dess egen.",
     "Sus archivos - el vídeo, su copia, los subtítulos y la imagen - se renombran como <code class=\"media-code\">%s</code>, "
     "y también su propia carpeta."},
    {"Title", "Titel", "Título"},
    {"Type its title - in letters a disk can store", "Skriv titeln - med tecken som en disk kan spara",
     "Escribe su título - con caracteres que un disco pueda guardar"},
    {"A year like 1993 - or leave it empty", "Ett år som 1993 - eller lämna tomt", "Un año como 1993 - o déjalo vacío"},
    {"Without a year Hemma can't tell it's a film - add the year", "Utan år kan Hemma inte se att det är en film - lägg till året",
     "Sin año Hemma no sabe que es una película - añade el año"},

    /* renaming a series (1.3) */
    {"Rename", "Byt namn", "Renombrar"},
    {"Rename the series", "Byt namn på serien", "Renombrar la serie"},
    {"Hemma reads a series' name from its folder, so its folder <code class=\"media-code\">%s</code> gets the new name. TV "
     "channels, screens and everyone's progress stay with it.",
     "Hemma läser en series namn från dess mapp, så mappen <code class=\"media-code\">%s</code> får det nya namnet. "
     "TV-kanaler, skärmar och allas framsteg följer med.",
     "Hemma lee el nombre de una serie de su carpeta, así que su carpeta <code class=\"media-code\">%s</code> recibe el "
     "nombre nuevo. Los canales de TV, las pantallas y el progreso de todos siguen con ella."},
    {"Hemma reads a series' name from its folders, so its %d folders get the new name. TV channels, screens and "
     "everyone's progress stay with it.",
     "Hemma läser en series namn från dess mappar, så dess %d mappar får det nya namnet. TV-kanaler, skärmar och allas "
     "framsteg följer med.",
     "Hemma lee el nombre de una serie de sus carpetas, así que sus %d carpetas reciben el nombre nuevo. Los canales de "
     "TV, las pantallas y el progreso de todos siguen con ella."},
    {"Year (optional)", "År (frivilligt)", "Año (opcional)"},
    {"Type its name - in letters a disk can store", "Skriv namnet - med tecken som en disk kan spara",
     "Escribe su nombre - con caracteres que un disco pueda guardar"},
    {"There's already a series called %s", "Det finns redan en serie som heter %s", "Ya hay una serie llamada %s"},
    {"Videos are being uploaded into its folder - rename it once that's done",
     "Videor laddas upp till dess mapp - byt namn när det är klart",
     "Se están subiendo vídeos a su carpeta - renómbrala cuando termine"},
    {"Hemma would still read %s as \"%s\" by its own file name - use Fix number on it first (that puts it in a season "
     "folder), then rename the series",
     "Hemma skulle fortfarande läsa %s som \"%s\" efter filens eget namn - använd Rätta numret på det först (det lägger "
     "det i en säsongsmapp), byt sedan namn på serien",
     "Hemma seguiría leyendo %s como \"%s\" por el nombre de su propio archivo - usa Corregir número en él primero (lo "
     "pone en una carpeta de temporada) y luego renombra la serie"},
    {"Some of its episodes aren't in a folder of the series' own, so Hemma can't rename it by its folder.",
     "Några av avsnitten ligger inte i en mapp som är seriens egen, så Hemma kan inte byta namn på den via mappen.",
     "Algunos de sus episodios no están en una carpeta propia de la serie, así que Hemma no puede renombrarla por su "
     "carpeta."},
    {"Its episodes are in too many folders to rename them all at once.",
     "Avsnitten ligger i för många mappar för att byta namn på alla på en gång.",
     "Sus episodios están en demasiadas carpetas para renombrarlas todas a la vez."},
    {"One of its folders is inside another, so Hemma can't rename it by its folders.",
     "En av dess mappar ligger i en annan, så Hemma kan inte byta namn på den via mapparna.",
     "Una de sus carpetas está dentro de otra, así que Hemma no puede renombrarla por sus carpetas."},
    {"Its folder holds other videos too, so Hemma can't rename it by its folder.",
     "Mappen har andra videor också, så Hemma kan inte byta namn på den via mappen.",
     "Su carpeta también tiene otros vídeos, así que Hemma no puede renombrarla por su carpeta."},

    /* fixing, either */
    {"Only an admin can fix names in the library", "Bara en administratör kan rätta namn i biblioteket",
     "Solo un administrador puede corregir nombres en la biblioteca"},
    {"That name is too long, or one a disk can't store - try a shorter title",
     "Namnet är för långt, eller ett som en disk inte kan spara - prova en kortare titel",
     "Ese nombre es demasiado largo, o uno que un disco no puede guardar - prueba un título más corto"},
    {"Hemma would read that name as \"%s\" - try other words", "Hemma skulle läsa det namnet som \"%s\" - prova andra ord",
     "Hemma leería ese nombre como \"%s\" - prueba otras palabras"},
    {"There's already a file called %s there", "Det finns redan en fil som heter %s där", "Ya hay un archivo llamado %s ahí"},
    {"There's already a folder called %s there", "Det finns redan en mapp som heter %s där", "Ya hay una carpeta llamada %s ahí"},
    {"Couldn't rename it - is it playing somewhere? Try again in a moment.",
     "Kunde inte byta namn - spelas den någonstans? Försök igen om en stund.",
     "No se pudo renombrar - ¿se está reproduciendo en algún sitio? Vuelve a intentarlo en un momento."},
    {"That's its name already", "Det heter den redan", "Ya se llama así"},
    {"Renamed it %s", "Bytte namn till %s", "Renombrado a %s"},
    {"renamed %s to %s", "bytte namn på %s till %s", "renombró %s a %s"},

    /* Library settings: what takes room */
    {"What takes room", "Det som tar plats", "Lo que ocupa espacio"},
    {"The biggest series and films on the disk, originals and their copies together.",
     "De största serierna och filmerna på disken, original och kopior tillsammans.",
     "Las series y películas más grandes del disco, originales y copias juntos."},
    {"Film", "Film", "Película"},
    /* (2.6: HEVC made H.264) */
    {"%d episode is HEVC: make it play everywhere -", "%d avsnitt är HEVC: få det att spelas överallt -",
     "%d episodio es HEVC: haz que se reproduzca en todas partes -"},
    {"%d episodes are HEVC: make them play everywhere -", "%d avsnitt är HEVC: få dem att spelas överallt -",
     "%d episodios son HEVC: haz que se reproduzcan en todas partes -"},
    {"%d episode of these is HEVC -", "%d av de här avsnitten är HEVC -", "%d de estos episodios es HEVC -"},
    {"%d episodes of these are HEVC -", "%d av de här avsnitten är HEVC -", "%d de estos episodios son HEVC -"},
};
TRANSLATIONS(media_manage, TR)

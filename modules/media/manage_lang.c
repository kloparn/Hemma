/* modules/media/manage.c's words in Swedish and Spanish (core/i18n.h says how). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the Manage view of a series */
    {"Manage", "Hantera", "Gestionar"},
    {"Fix episode numbers, or remove episodes, seasons or the whole series",
     "Rätta avsnittsnummer, eller ta bort avsnitt, säsonger eller hela serien",
     "Corrige números de episodio, o quita episodios, temporadas o la serie entera"},
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
};
TRANSLATIONS(media_manage, TR)

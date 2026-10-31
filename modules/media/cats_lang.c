/* modules/media's 1.4 words in Swedish and Spanish (core/i18n.h says how): categories (cats.c), the lists' and
   search's new words (media.c, tiles.c) and looking categories up online (lookup.c). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the starters (cats.c STARTERS) */
    {"Action", "Action", "Acción"},
    {"Adventure", "Äventyr", "Aventura"},
    {"Animated", "Animerat", "Animación"},
    {"Anime", "Anime", "Anime"},
    {"Comedy", "Komedi", "Comedia"},
    {"Crime", "Kriminal", "Crimen"},
    {"Documentary", "Dokumentär", "Documental"},
    {"Drama", "Drama", "Drama"},
    {"Family", "Familj", "Familia"},
    {"Fantasy", "Fantasy", "Fantasía"},
    {"Horror", "Skräck", "Terror"},
    {"Kids", "Barn", "Infantil"},
    {"Musical", "Musikal", "Musical"},
    {"Mystery", "Mysterium", "Misterio"},
    {"Romance", "Romantik", "Romance"},
    {"Science fiction", "Science fiction", "Ciencia ficción"},
    {"Thriller", "Thriller", "Suspense"},
    {"War", "Krig", "Bélica"},
    {"Western", "Western", "Wéstern"},
    {"Christmas", "Jul", "Navidad"},
    {"Halloween", "Halloween", "Halloween"},

    /* a title's categories, and the dialog */
    {"Categories", "Kategorier", "Categorías"},
    {"Add categories", "Lägg till kategorier", "Añadir categorías"},
    {"Tap the ones that fit %s. Everyone finds it under them on Movies and Series, and when searching.",
     "Tryck på dem som passar %s. Alla hittar den under dem på Filmer och Serier, och när de söker.",
     "Toca las que encajen con %s. Todos lo encontrarán en ellas en Películas y Series, y al buscar."},
    {"A category of your own", "En egen kategori", "Una categoría tuya"},
    {"Pokémon, Swedish, Sunday films…", "Pokémon, Svenskt, Söndagsfilmer…", "Pokémon, En sueco, Películas de domingo…"},
    {"Some were filled in from its .nfo file or looked up online. Tap one to take it off.",
     "Några kom från dess .nfo-fil eller slogs upp på nätet. Tryck på en för att ta bort den.",
     "Algunas vienen de su archivo .nfo o se buscaron en internet. Toca una para quitarla."},
    {"There's no such category", "Den kategorin finns inte", "Esa categoría no existe"},
    {"Type a name for it", "Skriv ett namn på den", "Escribe un nombre para ella"},
    {"It's in %s already", "Den finns redan i %s", "Ya está en %s"},
    {"Added to %s", "Tillagd i %s", "Añadido a %s"},
    {"A category", "En kategori", "Una categoría"},

    /* the lists and search (media.c) */
    {"Watched lately", "Senast sedda", "Vistas hace poco"},
    {"Everything", "Allt", "Todo"},
    {"Not started", "Inte påbörjade", "Sin empezar"},
    {"Started", "Påbörjade", "Empezadas"},
    {"Nothing here with these choices", "Inget här med de här valen", "Nada con estas opciones"},
    {"Show everything", "Visa allt", "Mostrar todo"},
    {"Nothing in this category yet", "Inget i den här kategorin än", "Aún no hay nada en esta categoría"},
    {"more words narrow it down", "fler ord ger färre träffar", "más palabras acotan la búsqueda"},
    {"Try another word — titles, episode names and numbers, categories and folders are searched.",
     "Prova ett annat ord — titlar, avsnittsnamn och -nummer, kategorier och mappar genomsöks.",
     "Prueba otra palabra: se buscan títulos, nombres y números de episodios, categorías y carpetas."},

    /* looking up online (lookup.c) */
    {"Categories from the internet", "Kategorier från internet", "Categorías de internet"},
    {"Hemma can look up the genres of each series and film online, and put them under categories. Only titles and "
     "years are sent: series' to TVmaze, films' to OMDb. Nothing is looked up until you tick the box.",
     "Hemma kan slå upp genrerna för varje serie och film på nätet och lägga dem under kategorier. Bara titlar och år "
     "skickas: seriernas till TVmaze, filmernas till OMDb. Inget slås upp förrän du kryssar i rutan.",
     "Hemma puede buscar en internet los géneros de cada serie y película y ponerlos en categorías. Solo se envían "
     "títulos y años: los de las series a TVmaze, los de las películas a OMDb. No se busca nada hasta que marques la "
     "casilla."},
    {"Checking whether this Hemma can reach the internet…", "Kollar om den här Hemma når internet…",
     "Comprobando si este Hemma llega a internet…"},
    {"This Hemma can't reach the internet right now, so it can't look anything up.",
     "Den här Hemma når inte internet just nu, så den kan inte slå upp något.",
     "Este Hemma no llega a internet ahora mismo, así que no puede buscar nada."},
    {"This Hemma can reach the internet.", "Den här Hemma når internet.", "Este Hemma llega a internet."},
    {"Looked up: %d of %d series", "Uppslagna: %d av %d serier", "Buscadas: %d de %d series"},
    {"%d of %d films", "%d av %d filmer", "%d de %d películas"},
    {"Look up categories online", "Slå upp kategorier på nätet", "Buscar categorías en internet"},
    {"OMDb key, for films (optional)", "OMDb-nyckel, för filmer (valfritt)", "Clave de OMDb, para películas (opcional)"},
    {"For example 1a2b3c4d", "Till exempel 1a2b3c4d", "Por ejemplo 1a2b3c4d"},
    {"A free key comes by email from <a href=\"https://www.omdbapi.com/apikey.aspx\" rel=\"noreferrer\">omdbapi.com</a>. "
     "Without one, films get categories by hand and from .nfo files only.",
     "En gratis nyckel kommer med e-post från <a href=\"https://www.omdbapi.com/apikey.aspx\" rel=\"noreferrer\">"
     "omdbapi.com</a>. Utan den får filmer kategorier bara för hand och från .nfo-filer.",
     "Una clave gratuita llega por correo desde <a href=\"https://www.omdbapi.com/apikey.aspx\" rel=\"noreferrer\">"
     "omdbapi.com</a>. Sin ella, las películas solo tienen categorías puestas a mano y de archivos .nfo."},
    {"Series' genres come from <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a> (CC BY-SA), films' from "
     "<a href=\"https://www.omdbapi.com\" rel=\"noreferrer\">OMDb</a>. What was found stays when you switch it off.",
     "Seriernas genrer kommer från <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a> (CC BY-SA), filmernas "
     "från <a href=\"https://www.omdbapi.com\" rel=\"noreferrer\">OMDb</a>. Det som hittats finns kvar när du stänger av det.",
     "Los géneros de las series vienen de <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a> (CC BY-SA), los "
     "de las películas de <a href=\"https://www.omdbapi.com\" rel=\"noreferrer\">OMDb</a>. Lo encontrado se queda si lo "
     "desactivas."},
    {"OMDb refused the key: %s", "OMDb godtog inte nyckeln: %s", "OMDb rechazó la clave: %s"},
    {"An OMDb key is letters and digits, like 1a2b3c4d", "En OMDb-nyckel är bokstäver och siffror, som 1a2b3c4d",
     "Una clave de OMDb son letras y números, como 1a2b3c4d"},
    {"This Hemma can't reach the internet right now", "Den här Hemma når inte internet just nu",
     "Este Hemma no llega a internet ahora mismo"},
    {"Hemma looks up categories online now", "Hemma slår upp kategorier på nätet nu", "Hemma busca categorías en internet ahora"},
    {"Hemma doesn't look anything up online now", "Hemma slår inte upp något på nätet nu", "Hemma ya no busca nada en internet"},
    /* one title (Manage, the categories dialog) */
    {"From the internet", "Från internet", "De internet"},
    {"Found at %s as %s: %s", "Hittad på %s som %s: %s", "Encontrada en %s como %s: %s"},
    {"by the id you gave", "med id:t du gav", "por el id que diste"},
    {"Nothing found by its name at %s.", "Inget hittades med dess namn på %s.", "No se encontró nada con su nombre en %s."},
    {"Not looked up yet.", "Inte uppslagen än.", "Aún no se ha buscado."},
    {"Looking up online is switched off.", "Uppslagning på nätet är avstängd.", "La búsqueda en internet está desactivada."},
    {"Switch it on to look titles up by their name or by an id you give.",
     "Slå på den för att slå upp titlar med deras namn eller med ett id du ger.",
     "Actívala para buscar títulos por su nombre o por un id que des."},
    {"Films are looked up at OMDb, which needs a key.", "Filmer slås upp på OMDb, som kräver en nyckel.",
     "Las películas se buscan en OMDb, que necesita una clave."},
    {"Its IMDb link or id, when the name finds the wrong one", "Dess IMDb-länk eller id, när namnet hittar fel",
     "Su enlace o id de IMDb, si el nombre encuentra la equivocada"},
    {"Its TVmaze or IMDb link or id, when the name finds the wrong one",
     "Dess TVmaze- eller IMDb-länk eller id, när namnet hittar fel",
     "Su enlace o id de TVmaze o IMDb, si el nombre encuentra la equivocada"},
    {"Update categories", "Uppdatera kategorier", "Actualizar categorías"},
    {"Switch on looking up categories online first (Library settings)",
     "Slå på uppslagning av kategorier på nätet först (Biblioteksinställningar)",
     "Activa primero la búsqueda de categorías en internet (Ajustes de la biblioteca)"},
    {"Films are looked up at OMDb, which needs a key (Library settings)",
     "Filmer slås upp på OMDb, som kräver en nyckel (Biblioteksinställningar)",
     "Las películas se buscan en OMDb, que necesita una clave (Ajustes de la biblioteca)"},
    {"Paste its IMDb link, or its id like tt0094721", "Klistra in dess IMDb-länk, eller dess id som tt0094721",
     "Pega su enlace de IMDb, o su id como tt0094721"},
    {"Paste its TVmaze or IMDb link, or an id like 84 or tt0182576",
     "Klistra in dess TVmaze- eller IMDb-länk, eller ett id som 84 eller tt0182576",
     "Pega su enlace de TVmaze o IMDb, o un id como 84 o tt0182576"},
    {"Looking it up…", "Slår upp den…", "Buscando…"},
    /* toasts when an answer comes */
    {"Couldn't look it up: no answer from the internet", "Kunde inte slå upp den: inget svar från internet",
     "No se pudo buscar: sin respuesta de internet"},
    {"Couldn't look it up: %s", "Kunde inte slå upp den: %s", "No se pudo buscar: %s"},
    {"Nothing has that id", "Inget har det id:t", "Nada tiene ese id"},
    {"Found %s: %s", "Hittade %s: %s", "Encontrado %s: %s"},
    {"Nothing found by the name %s", "Inget hittades med namnet %s", "No se encontró nada con el nombre %s"},

    /* pictures looked up online (lookup.c, 2.5) */
    {"Pictures from the internet", "Bilder från internet", "Imágenes de internet"},
    {"Hemma can find a picture for each series and film that has none: a series' at TVmaze, a film's at Wikipedia by its "
     "title and year. Only titles and years are sent, and a picture you set yourself always stays. Nothing is looked up "
     "until you tick the box.",
     "Hemma kan hitta en bild till varje serie och film som saknar en: seriens på TVmaze, filmens på Wikipedia med dess "
     "titel och år. Bara titlar och år skickas, och en bild du själv har valt finns alltid kvar. Inget slås upp förrän du "
     "kryssar i rutan.",
     "Hemma puede encontrar una imagen para cada serie y película que no tenga: la de una serie en TVmaze, la de una "
     "película en Wikipedia por su título y año. Solo se envían títulos y años, y una imagen que hayas puesto tú siempre se "
     "queda. No se busca nada hasta que marques la casilla."},
    {"With a picture: %d of %d series", "Med bild: %d av %d serier", "Con imagen: %d de %d series"},
    {"Films' pictures come over https, which needs the program curl - and this computer doesn't have it. Series' "
     "pictures still come.",
     "Filmernas bilder kommer över https, som kräver programmet curl - och den här datorn har det inte. Seriernas bilder "
     "kommer ändå.",
     "Las imágenes de las películas llegan por https, que necesita el programa curl, y este ordenador no lo tiene. Las de "
     "las series llegan igual."},
    {"Find pictures online", "Hitta bilder på nätet", "Buscar imágenes en internet"},
    {"Series' pictures come from <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a>, films' from "
     "<a href=\"https://en.wikipedia.org\" rel=\"noreferrer\">Wikipedia</a> - the studios' own posters, for your "
     "household. Those found stay when you switch it off. A wrong one? Give the right one's link under the title's "
     "Categories, or set a picture of your own.",
     "Seriernas bilder kommer från <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a>, filmernas från "
     "<a href=\"https://en.wikipedia.org\" rel=\"noreferrer\">Wikipedia</a> - studiornas egna affischer, för ditt "
     "hushåll. De som hittats finns kvar när du stänger av det. En felaktig? Ge länken till den rätta under titelns "
     "Kategorier, eller välj en egen bild.",
     "Las imágenes de las series vienen de <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a>, las de las "
     "películas de <a href=\"https://en.wikipedia.org\" rel=\"noreferrer\">Wikipedia</a>: los carteles de los propios "
     "estudios, para tu hogar. Las encontradas se quedan si lo desactivas. ¿Una equivocada? Da el enlace de la correcta en "
     "las Categorías del título, o pon una imagen tuya."},
    {"Picture: its own - Hemma never changes it.", "Bild: en egen - Hemma byter aldrig ut den.",
     "Imagen: la suya propia; Hemma nunca la cambia."},
    {"Picture: from %s, %s", "Bild: från %s, %s", "Imagen: de %s, %s"},
    {"by the link you gave", "med länken du gav", "por el enlace que diste"},
    {"Picture: nothing found by its name at %s.", "Bild: inget hittades med dess namn på %s.",
     "Imagen: no se encontró nada con su nombre en %s."},
    {"Picture: it couldn't be fetched - Hemma tries again tomorrow.", "Bild: den kunde inte hämtas - Hemma försöker igen i morgon.",
     "Imagen: no se pudo descargar; Hemma lo intenta de nuevo mañana."},
    {"Picture: not looked for yet.", "Bild: inte letad efter än.", "Imagen: aún no se ha buscado."},
    {"Films' pictures need the program curl, which this computer doesn't have.",
     "Filmernas bilder kräver programmet curl, som den här datorn inte har.",
     "Las imágenes de las películas necesitan el programa curl, que este ordenador no tiene."},
    {"Its Wikipedia link, when the name finds the wrong one", "Dess Wikipedia-länk, när namnet hittar fel",
     "Su enlace de Wikipedia, si el nombre encuentra la equivocada"},
    {"Update picture", "Uppdatera bild", "Actualizar imagen"},
    {"It has a picture of its own now: that one stays", "Den har en egen bild nu: den får vara kvar",
     "Ya tiene una imagen propia: esa se queda"},
    {"Found a picture: %s", "Hittade en bild: %s", "Imagen encontrada: %s"},
    {"%s has no picture there", "%s har ingen bild där", "%s no tiene imagen allí"},
    {"Couldn't fetch the picture: %s", "Kunde inte hämta bilden: %s", "No se pudo descargar la imagen: %s"},
    {"Hemma looks for pictures online now", "Hemma letar efter bilder på nätet nu", "Hemma busca imágenes en internet ahora"},
    {"Hemma doesn't look for pictures online now", "Hemma letar inte efter bilder på nätet nu",
     "Hemma ya no busca imágenes en internet"},
    {"Switch on finding pictures online first (Library settings)",
     "Slå på att hitta bilder på nätet först (Biblioteksinställningar)",
     "Activa primero la búsqueda de imágenes en internet (Ajustes de la biblioteca)"},
    {"It has a picture of its own - Hemma never changes that one", "Den har en egen bild - den byter Hemma aldrig ut",
     "Tiene una imagen propia; Hemma nunca la cambia"},
    {"Films' pictures need the program curl, which this computer doesn't have",
     "Filmernas bilder kräver programmet curl, som den här datorn inte har",
     "Las imágenes de las películas necesitan el programa curl, que este ordenador no tiene"},
    {"Paste its Wikipedia link, like https://en.wikipedia.org/wiki/Hocus_Pocus_(1993_film)",
     "Klistra in dess Wikipedia-länk, som https://en.wikipedia.org/wiki/Hocus_Pocus_(1993_film)",
     "Pega su enlace de Wikipedia, como https://en.wikipedia.org/wiki/Hocus_Pocus_(1993_film)"},
};
TRANSLATIONS(media_cats, TR)

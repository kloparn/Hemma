/* modules/screens' words in Swedish and Spanish (issue #46; core/i18n.h says how). The rows' and cards'
   headings are their modules' Block titles, translated in those modules' tables. */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the module */
    {"Screens", "Skärmar", "Pantallas"},

    /* actions */
    {"Only an admin can change screens", "Bara en administratör kan ändra skärmar",
     "Solo un administrador puede cambiar las pantallas"},
    {"That screen is gone", "Den skärmen finns inte längre", "Esa pantalla ya no existe"},
    {"That row is gone", "Den raden finns inte längre", "Esa fila ya no existe"},
    {"Give the screen a name", "Ge skärmen ett namn", "Ponle un nombre a la pantalla"},
    {"That isn't on offer any more - pick another", "Det går inte att välja längre - välj något annat",
     "Eso ya no se puede elegir - elige otra cosa"},
    {"There's nothing to pick for that row yet", "Det finns inget att välja för den raden än",
     "Todavía no hay nada que elegir para esa fila"},
    {"Pick someone from the list", "Välj någon i listan", "Elige a alguien de la lista"},
    {"Only a TV picks who's watching: an admin ticks it under Screens, Edit, Shows on",
     "Bara en TV väljer vem som tittar: en administratör kryssar i den under Skärmar, Ändra, Visas på",
     "Solo una tele elige quién está viendo: un administrador la marca en Pantallas, Editar, Se muestra en"},
    {"Too many wrong PINs - try again in %d min", "För många fel PIN-koder - försök igen om %d min",
     "Demasiados PIN incorrectos - vuelve a intentarlo en %d min"},
    {"That PIN isn't right", "Den PIN-koden stämmer inte", "Ese PIN no es correcto"},
    {"You're on the TV's Who's watching? now", "Du finns nu med på TV:ns Vem tittar?",
     "Ahora apareces en el ¿Quién está viendo? de la tele"},
    {"You're off the TV's Who's watching?", "Du finns inte längre med på TV:ns Vem tittar?",
     "Ya no apareces en el ¿Quién está viendo? de la tele"},
    {"No PIN any more: the TV picks you straight away", "Ingen PIN-kod längre: TV:n väljer dig direkt",
     "Ya no hay PIN: la tele te elige directamente"},
    {"A PIN is 4 digits", "En PIN-kod är 4 siffror", "Un PIN tiene 4 cifras"},
    {"Hemma has no password secret yet, so it can't keep a PIN (System says how to add one)",
     "Hemma har ingen lösenordshemlighet än, så den kan inte spara en PIN-kod (System berättar hur du lägger till en)",
     "Hemma todavía no tiene un secreto para contraseñas, así que no puede guardar un PIN (Sistema explica cómo "
     "añadir uno)"},
    {"PIN saved: the TV asks for it when you're picked", "PIN-koden är sparad: TV:n frågar efter den när du väljs",
     "PIN guardado: la tele te lo pide cuando te eligen"},
    {"That's %d screens already - remove one first", "Det finns redan %d skärmar - ta bort en först",
     "Ya hay %d pantallas - quita una primero"},
    {"%s is ready for its rows", "%s är redo för sina rader", "%s está lista para sus filas"},
    {"%s is gone", "%s är borttagen", "Se eliminó %s"},
    {"That row isn't available here", "Den raden finns inte här", "Esa fila no está disponible aquí"},
    {"A screen holds %d rows at most", "En skärm rymmer högst %d rader", "Una pantalla tiene como máximo %d filas"},
    {"Added %s", "Lade till %s", "Añadido: %s"},
    {"Removed", "Borttaget", "Quitado"},

    /* the list and "On the TV" */
    {"What the TV and the wall tablets show", "Det som TV:n och surfplattorna på väggen visar",
     "Lo que muestran la tele y las tabletas de pared"},
    {"New screen", "Ny skärm", "Nueva pantalla"},
    {"No screens yet", "Inga skärmar än", "Todavía no hay pantallas"},
    {"Make one and pick its rows: Continue watching, On TV, Series, Movies...",
     "Skapa en och välj dess rader: Fortsätt titta, På TV, Serier, Filmer...",
     "Crea una y elige sus filas: Seguir viendo, En la tele, Series, Películas..."},
    {"An admin can put one together for the TV.", "En administratör kan sätta ihop en för TV:n.",
     "Un administrador puede montar una para la tele."},
    {"Wall · %d card", "Vägg · %d kort", "Pared · %d tarjeta"},
    {"Wall · %d cards", "Vägg · %d kort", "Pared · %d tarjetas"},
    {"TV · %d row", "TV · %d rad", "TV · %d fila"},
    {"TV · %d rows", "TV · %d rader", "TV · %d filas"},
    {"on %s", "på %s", "en %s"},
    {"Edit %s", "Ändra %s", "Editar %s"},
    {"On the TV", "På TV:n", "En la tele"},
    {"Under <a href=\"/people\">Household</a>, add someone for the TV (or the wall tablet) and choose the sign-in kind "
     "<b>%s</b>: it can only look.",
     "Lägg till någon för TV:n (eller surfplattan på väggen) under <a href=\"/people\">Hushåll</a> och välj "
     "inloggningssorten <b>%s</b>: den kan bara titta.",
     "En <a href=\"/people\">Hogar</a>, añade a alguien para la tele (o la tableta de pared) y elige el tipo de sesión "
     "<b>%s</b>: solo puede mirar."},
    {"Display", "Skärm", "Pantalla"}, /* (the sign-in kind: people's ROLE_NAMES) */
    {"On its web browser, open Hemma and sign in with it. Hemma only listens on this computer (%s) now, so a TV can't "
     "reach it: start it for the home network (see <a href=\"/system\">System</a>).",
     "Öppna Hemma i dess webbläsare och logga in med det kontot. Hemma lyssnar bara på den här datorn (%s) nu, så en "
     "TV kan inte nå den: starta den för hemnätverket (se <a href=\"/system\">System</a>).",
     "En su navegador, abre Hemma e inicia sesión con esa cuenta. Ahora Hemma solo escucha en este ordenador (%s), así "
     "que una tele no puede llegar a él: arráncalo para la red de casa (mira <a href=\"/system\">Sistema</a>)."},
    {"On its web browser, open Hemma - type the address this browser shows, with <code>http://</code> in front (not "
     "<i>localhost</i>) - and sign in with it.",
     "Öppna Hemma i dess webbläsare - skriv adressen som den här webbläsaren visar, med <code>http://</code> först "
     "(inte <i>localhost</i>) - och logga in med det kontot.",
     "En su navegador, abre Hemma - escribe la dirección que muestra este navegador, con <code>http://</code> delante "
     "(no <i>localhost</i>) - e inicia sesión con esa cuenta."},
    {"On its web browser, open Hemma at this address - all of it, with <code>http://</code> - and sign in with it:",
     "Öppna Hemma i dess webbläsare på den här adressen - hela, med <code>http://</code> - och logga in med det kontot:",
     "En su navegador, abre Hemma en esta dirección - entera, con <code>http://</code> - e inicia sesión con esa "
     "cuenta:"},
    {"Phones and computers can use <b>%s</b> too; many TVs can't.",
     "Telefoner och datorer kan också använda <b>%s</b>; många TV-apparater kan inte det.",
     "Los móviles y ordenadores también pueden usar <b>%s</b>; muchas teles no."},
    {"A tablet or phone can scan this instead", "En surfplatta eller telefon kan skanna den här i stället",
     "Una tableta o un móvil puede escanear esto en su lugar"},
    {"Edit a screen and tick it under <b>Shows on</b>: it opens that screen by itself. A TV asks <i>Who's watching?</i> "
     "- nobody types a password there.",
     "Ändra en skärm och kryssa i den under <b>Visas på</b>: den öppnar skärmen av sig själv. En TV frågar <i>Vem "
     "tittar?</i> - ingen skriver något lösenord där.",
     "Edita una pantalla y márcala en <b>Se muestra en</b>: abre esa pantalla por sí sola. Una tele pregunta "
     "<i>¿Quién está viendo?</i> - nadie escribe una contraseña allí."},

    /* "New screen" */
    {"Living room TV", "TV i vardagsrummet", "Tele del salón"},
    {"Kind", "Sort", "Tipo"},
    {"Wall", "Vägg", "Pared"},
    {"TV: big rows you scroll with the remote, like a streaming app. Wall: an always-on overview for a tablet - the "
     "clock, who's home, today, bookings, the shopping list...",
     "TV: stora rader du bläddrar i med fjärrkontrollen, som en streamingapp. Vägg: en översikt som alltid är på, för "
     "en surfplatta - klockan, vem som är hemma, idag, bokningar, inköpslistan...",
     "TV: filas grandes que recorres con el mando, como una app de streaming. Pared: un resumen siempre encendido para "
     "una tableta - el reloj, quién está en casa, hoy, las reservas, la lista de la compra..."},
    {"Make it", "Skapa den", "Crearla"},

    /* you on the TV */
    {"You on the TV", "Du på TV:n", "Tú en la tele"},
    {"Show me on the TV's <i>Who's watching?</i>", "Visa mig på TV:ns <i>Vem tittar?</i>",
     "Mostrarme en el <i>¿Quién está viendo?</i> de la tele"},
    {"Whoever has the TV's sign-in can see and clear the watch history of anyone it lists.",
     "Den som har TV:ns inloggning kan se och rensa tittarhistoriken för alla som står där.",
     "Quien tenga la sesión de la tele puede ver y borrar el historial de cualquiera que aparezca en ella."},
    {"Your PIN is set: the TV asks for it when you're picked.", "Din PIN-kod är inställd: TV:n frågar efter den när du väljs.",
     "Tu PIN está configurado: la tele te lo pide cuando te eligen."},
    {"Remove the PIN", "Ta bort PIN-koden", "Quitar el PIN"},
    {"A PIN for the TV", "En PIN-kod för TV:n", "Un PIN para la tele"},
    {"(optional)", "(valfritt)", "(opcional)"},
    {"4 digits", "4 siffror", "4 cifras"},
    {"The TV asks for it when someone picks you, so nobody watches - or clears your history - as you.",
     "TV:n frågar efter den när någon väljer dig, så att ingen tittar - eller rensar din historik - som du.",
     "La tele lo pide cuando alguien te elige, para que nadie vea - ni borre tu historial - como tú."},
    {"Save the PIN", "Spara PIN-koden", "Guardar el PIN"},

    /* a TV screen and "Who's watching?" */
    {"Switch", "Byt", "Cambiar"},
    {"Exit", "Avsluta", "Salir"},
    {"Who's watching?", "Vem tittar?", "¿Quién está viendo?"},
    {"%s's PIN", "PIN-kod för %s", "PIN de %s"},
    {"Too many wrong PINs - try again in %d min.", "För många fel PIN-koder - försök igen om %d min.",
     "Demasiados PIN incorrectos - vuelve a intentarlo en %d min."},
    {"Nobody's set up yet: add the household on a phone or computer.",
     "Ingen är inlagd än: lägg till hushållet på en telefon eller dator.",
     "Todavía no hay nadie: añade el hogar desde un móvil o un ordenador."},
    {"Nothing to show yet", "Inget att visa än", "Nada que mostrar todavía"},
    {"No rows yet", "Inga rader än", "Todavía no hay filas"},
    {"When there's something to watch, it shows up here.", "När det finns något att titta på dyker det upp här.",
     "Cuando haya algo que ver, aparecerá aquí."},
    {"This screen has nothing on it yet.", "Den här skärmen har inget på sig än.", "Esta pantalla todavía no tiene nada."},
    {"Add rows", "Lägg till rader", "Añadir filas"},

    /* a wall */
    {"%s %d %s · week %d", "%s %d %s · vecka %d", "%s %d de %s · semana %d"},
    {"Now", "Nu", "Ahora"},
    {"time\x04" "until %s", "till %s", "hasta las %s"},
    {"home", "hemma", "en casa"},
    {"away", "borta", "fuera"},
    {"online", "online", "conectado"},
    {"All quiet - nothing booked, planned or on the list.", "Lugnt - inget bokat, planerat eller på listan.",
     "Todo tranquilo - nada reservado, planeado ni en la lista."},
    {"No cards on this wall yet.", "Inga kort på den här väggen än.", "Todavía no hay tarjetas en esta pared."},

    /* the builder */
    {"Wall screen", "Väggskärm", "Pantalla de pared"},
    {"TV screen", "TV-skärm", "Pantalla de TV"},
    {"All screens", "Alla skärmar", "Todas las pantallas"},
    {"Only an admin changes screens", "Bara en administratör ändrar skärmar", "Solo un administrador cambia las pantallas"},
    {"Open %s", "Öppna %s", "Abrir %s"},
    {"Cards", "Kort", "Tarjetas"},
    {"Rows", "Rader", "Filas"},
    {"No cards yet: add some below.", "Inga kort än: lägg till några nedan.", "Todavía no hay tarjetas: añade algunas abajo."},
    {"No rows yet: add some below.", "Inga rader än: lägg till några nedan.", "Todavía no hay filas: añade algunas abajo."},
    {"In this order, flowing down the wall's columns. A card with nothing to show right now is left out.",
     "I den här ordningen, nedåt i väggens kolumner. Ett kort som inte har något att visa just nu utelämnas.",
     "En este orden, bajando por las columnas de la pared. Una tarjeta sin nada que mostrar ahora se deja fuera."},
    {"Top to bottom, as the screen shows them. A row with nothing to show right now is left out.",
     "Uppifrån och ner, som skärmen visar dem. En rad som inte har något att visa just nu utelämnas.",
     "De arriba abajo, como los muestra la pantalla. Una fila sin nada que mostrar ahora se deja fuera."},
    {"missing", "saknas", "falta"},
    {"%s isn't offered here any more - remove it, or leave it for when it's back",
     "%s erbjuds inte här längre - ta bort den, eller låt den vara tills den är tillbaka",
     "%s ya no se ofrece aquí - quítala, o déjala para cuando vuelva"},
    {"Move %s up", "Flytta upp %s", "Subir %s"},
    {"Move %s down", "Flytta ner %s", "Bajar %s"},
    {"Move up", "Flytta upp", "Subir"},
    {"Move down", "Flytta ner", "Bajar"},
    {"Remove %s", "Ta bort %s", "Quitar %s"},
    {"Heading", "Rubrik", "Título"},
    {"What it shows", "Vad den visar", "Qué muestra"},
    {"Not available any more", "Finns inte längre", "Ya no está disponible"},
    {"Add a card", "Lägg till ett kort", "Añadir una tarjeta"},
    {"Add a row", "Lägg till en rad", "Añadir una fila"},
    {"Nothing to pick yet", "Inget att välja än", "Nada que elegir todavía"},
    {"Nothing offers rows for this kind of screen yet.", "Inget erbjuder rader för den här sortens skärm än.",
     "Todavía nada ofrece filas para este tipo de pantalla."},
    {"Shows on", "Visas på", "Se muestra en"},
    {"A TV signs in with a <b>%s</b> account: it can only look. Add one under <a href=\"/people\">Household</a>, sign "
     "in with it on the TV, then tick it here - it opens this screen by itself.",
     "En TV loggar in med ett <b>%s</b>-konto: det kan bara titta. Lägg till ett under <a href=\"/people\">Hushåll</a>, "
     "logga in med det på TV:n och kryssa sedan i det här - det öppnar den här skärmen av sig själv.",
     "Una tele inicia sesión con una cuenta de tipo <b>%s</b>: solo puede mirar. Añade una en <a "
     "href=\"/people\">Hogar</a>, inicia sesión con ella en la tele y márcala aquí - abre esta pantalla por sí sola."},
    {"Ticked displays open this screen by themselves. Each one shows one screen.",
     "Ikryssade skärmkonton öppnar den här skärmen av sig själva. Vart och ett visar en skärm.",
     "Las cuentas de pantalla marcadas abren esta pantalla por sí solas. Cada una muestra una pantalla."},
    {"shows %s now", "visar %s nu", "ahora muestra %s"},
    {"Screen", "Skärm", "Pantalla"},
    {"Screen name", "Skärmens namn", "Nombre de la pantalla"},
    {"Rename", "Byt namn", "Renombrar"},
    {"Delete %s? Its rows go with it.", "Ta bort %s? Dess rader följer med.", "¿Eliminar %s? Sus filas se van con ella."},
    {"Delete screen", "Ta bort skärmen", "Eliminar la pantalla"},
    {"Not found", "Hittades inte", "No encontrado"},
    {"It may have been deleted.", "Den kan ha tagits bort.", "Puede que se haya eliminado."},
};
TRANSLATIONS(screens, TR)

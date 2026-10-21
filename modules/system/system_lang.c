/* modules/system's words in Swedish and Spanish (issue #46; core/i18n.h says how). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the module, its tabs and headings */
    {"System", "System", "Sistema"},
    {"Checks", "Kontroller", "Comprobaciones"},
    {"Log", "Logg", "Registro"},
    {"Backups", "Säkerhetskopior", "Copias de seguridad"},
    {"Updates", "Uppdateringar", "Actualizaciones"},
    {"Help", "Hjälp", "Ayuda"},
    {"Is everything working?", "Fungerar allt?", "¿Funciona todo?"},
    {"What Hemma is doing, live", "Vad Hemma gör, live", "Lo que hace Hemma, en directo"},
    {"When something isn't right", "När något inte stämmer", "Cuando algo no va bien"},
    {"Copies of everything Hemma knows", "Kopior av allt Hemma vet", "Copias de todo lo que sabe Hemma"},
    {"New versions of Hemma, checked before they go in", "Nya versioner av Hemma, kontrollerade innan de installeras",
     "Nuevas versiones de Hemma, comprobadas antes de instalarse"},
    {"Admins only", "Bara för administratörer", "Solo para administradores"},
    {"Not found", "Hittades inte", "No encontrado"},
    {"Only an admin can do that", "Bara en administratör kan göra det", "Solo un administrador puede hacer eso"},
    {"Noted", "Noterat", "Anotado"},

    /* times and sizes in the checks */
    {"less than a minute", "mindre än en minut", "menos de un minuto"},
    {"%lld day", "%lld dag", "%lld día"},
    {"%lld days", "%lld dagar", "%lld días"},
    {"UTC%s in January, UTC%s in July", "UTC%s i januari, UTC%s i juli", "UTC%s en enero, UTC%s en julio"},

    /* the checks: summary and rows */
    {"Something isn't working - %d thing below needs a look.", "Något fungerar inte - %d sak nedan behöver ses över.",
     "Algo no funciona - %d cosa de abajo necesita revisión."},
    {"Something isn't working - %d things below need a look.", "Något fungerar inte - %d saker nedan behöver ses över.",
     "Algo no funciona - %d cosas de abajo necesitan revisión."},
    {"%d thing below needs a look. Everything else works.", "%d sak nedan behöver ses över. Allt annat fungerar.",
     "%d cosa de abajo necesita revisión. Todo lo demás funciona."},
    {"%d things below need a look. Everything else works.", "%d saker nedan behöver ses över. Allt annat fungerar.",
     "%d cosas de abajo necesitan revisión. Todo lo demás funciona."},
    {"Everything works. Hemma %s has been running for %s.", "Allt fungerar. Hemma %s har körts i %s.",
     "Todo funciona. Hemma %s lleva en marcha %s."},
    {"Technical details", "Tekniska detaljer", "Detalles técnicos"},
    {"badge\x04" "Note", "Obs", "Nota"},
    {"Needs a look", "Behöver ses över", "Necesita revisión"},
    {"Problem", "Fel", "Problema"},
    {"Have a look", "Ta en titt", "Echar un vistazo"},

    /* the check: Hemma itself */
    {"Version %s, running for %s (since %s).", "Version %s, har körts i %s (sedan %s).",
     "Versión %s, en marcha desde hace %s (desde %s)."},
    {"The time before, it stopped without warning after %s - a power cut or a crash? The log has the lines from before.",
     "Förra gången stannade den utan förvarning efter %s - ett strömavbrott eller en krasch? Loggen har raderna från då.",
     "La vez anterior se detuvo sin avisar después de %s - ¿un corte de luz o un fallo? El registro tiene las líneas de "
     "entonces."},

    /* the check: the address */
    {"Address", "Adress", "Dirección"},
    {"Only this computer can open Hemma: phones and the TV can't reach it.",
     "Bara den här datorn kan öppna Hemma: telefoner och tv:n når den inte.",
     "Solo este ordenador puede abrir Hemma: los móviles y la tele no llegan a él."},
    {"Let the rest of the home in: start Hemma so it listens on the home network (docs/troubleshooting.md).",
     "Släpp in resten av hemmet: starta Hemma så att den lyssnar på hemnätverket (docs/troubleshooting.md).",
     "Deja entrar al resto de la casa: inicia Hemma para que escuche en la red de casa (docs/troubleshooting.md)."},
    {"It listens on %s. Start it without --host, or with --host 0.0.0.0 (HEMMA_HOST=0.0.0.0 in the settings file).",
     "Den lyssnar på %s. Starta den utan --host, eller med --host 0.0.0.0 (HEMMA_HOST=0.0.0.0 i inställningsfilen).",
     "Escucha en %s. Inícialo sin --host, o con --host 0.0.0.0 (HEMMA_HOST=0.0.0.0 en el archivo de ajustes)."},
    {"Hemma can't find its address on the home network.", "Hemma hittar inte sin adress på hemnätverket.",
     "Hemma no encuentra su dirección en la red de casa."},
    {"Check this computer's network cable or Wi-Fi - other devices need it to find Hemma.",
     "Kolla den här datorns nätverkskabel eller wifi - andra enheter behöver den för att hitta Hemma.",
     "Revisa el cable de red o el wifi de este ordenador - los demás dispositivos lo necesitan para encontrar Hemma."},
    {"Phones, computers and the TV open %s or %s. Tip: reserve that address in your router so it never changes "
     "(docs/home-network.md).",
     "Telefoner, datorer och tv:n öppnar %s eller %s. Tips: reservera adressen i din router så att den aldrig ändras "
     "(docs/home-network.md).",
     "Los móviles, los ordenadores y la tele abren %s o %s. Consejo: reserva esa dirección en tu router para que nunca "
     "cambie (docs/home-network.md)."},
    {"Phones, computers and the TV open %s. Tip: reserve that address in your router so it never changes "
     "(docs/home-network.md).",
     "Telefoner, datorer och tv:n öppnar %s. Tips: reservera adressen i din router så att den aldrig ändras "
     "(docs/home-network.md).",
     "Los móviles, los ordenadores y la tele abren %s. Consejo: reserva esa dirección en tu router para que nunca cambie "
     "(docs/home-network.md)."},

    /* the check: the time zone (issue #39) */
    {"Time zone", "Tidszon", "Zona horaria"},
    {"The time zone changed on %s, from %s to %s.", "Tidszonen ändrades %s, från %s till %s.",
     "La zona horaria cambió el %s, de %s a %s."},
    {"Things that last whole days (all-day events, chores' days, whole-day bookings) kept their days (%d).",
     "Saker som varar hela dagar (heldagshändelser, sysslornas dagar, heldagsbokningar) behöll sina dagar (%d).",
     "Lo que dura días enteros (eventos de todo el día, días de las tareas de la casa, reservas de días enteros) conservó "
     "sus días (%d)."},
    {"%d time typed before then was moved back to the time typed.",
     "%d tid som skrevs in före bytet flyttades tillbaka till den inskrivna tiden.",
     "%d hora escrita antes del cambio volvió a la hora escrita."},
    {"%d times typed before then were moved back to the times typed.",
     "%d tider som skrevs in före bytet flyttades tillbaka till de inskrivna tiderna.",
     "%d horas escritas antes del cambio volvieron a las horas escritas."},
    {"%d booking or event typed before then shows shifted by the difference - right if Hemma moved house, wrong if the "
     "old zone was.",
     "%d bokning eller händelse som skrevs in före bytet visas förskjuten med skillnaden - rätt om Hemma har flyttat, fel "
     "om den gamla zonen var fel.",
     "%d reserva o evento escrito antes del cambio se ve desplazado por la diferencia - bien si Hemma se mudó, mal si la "
     "zona antigua era errónea."},
    {"%d bookings and events typed before then show shifted by the difference - right if Hemma moved house, wrong if the "
     "old zone was.",
     "%d bokningar och händelser som skrevs in före bytet visas förskjutna med skillnaden - rätt om Hemma har flyttat, fel "
     "om den gamla zonen var fel.",
     "%d reservas y eventos escritos antes del cambio se ven desplazados por la diferencia - bien si Hemma se mudó, mal "
     "si la zona antigua era errónea."},
    {"Nothing typed before then shows at another time.", "Inget som skrevs in före bytet visas vid en annan tid.",
     "Nada de lo escrito antes del cambio se muestra a otra hora."},
    {"Was the old zone wrong (a Raspberry Pi left on its own UTC)? Then move them back to the times they were typed, "
     "above. If not, \"Got it\".",
     "Var den gamla zonen fel (en Raspberry Pi som stod kvar på sin egen UTC)? Flytta då tillbaka dem till tiderna de "
     "skrevs in med, ovan. Annars \"Uppfattat\".",
     "¿Era errónea la zona antigua (una Raspberry Pi que se quedó en su propio UTC)? Entonces devuélvelas a las horas en "
     "que se escribieron, arriba. Si no, \"Entendido\"."},
    {"The clock says %s, in UTC all year - a Raspberry Pi's own setting, not many homes' time.",
     "Klockan säger %s, i UTC året runt - en Raspberry Pis egen inställning, inte många hems tid.",
     "El reloj marca las %s, en UTC todo el año - el ajuste propio de una Raspberry Pi, no la hora de muchas casas."},
    {"If that isn't the time at home, set the zone now, before people add things: sudo raspi-config, Localisation "
     "Options, Timezone. Then restart Hemma.",
     "Om det inte är tiden hemma, ställ in zonen nu, innan någon lägger till saker: sudo raspi-config, Localisation "
     "Options, Timezone. Starta sedan om Hemma.",
     "Si no es la hora de tu casa, ajusta la zona ahora, antes de que se añadan cosas: sudo raspi-config, Localisation "
     "Options, Timezone. Después reinicia Hemma."},
    {"The clock says %s (%s).", "Klockan säger %s (%s).", "El reloj marca las %s (%s)."},
    {"The time zone changed on %s. Moved back to the times they were typed, these would show:",
     "Tidszonen ändrades %s. Flyttade tillbaka till tiderna de skrevs in med skulle de här visas så här:",
     "La zona horaria cambió el %s. Si vuelven a las horas en que se escribieron, se verían así:"},
    {"and %d more", "och %d till", "y %d más"},
    {"Move %d booking or event back to the time typed?",
     "Flytta tillbaka %d bokning eller händelse till tiden den skrevs in med?",
     "¿Devolver %d reserva o evento a la hora en que se escribió?"},
    {"Move %d bookings and events back to the times typed?",
     "Flytta tillbaka %d bokningar och händelser till tiderna de skrevs in med?",
     "¿Devolver %d reservas y eventos a las horas en que se escribieron?"},
    {"Move them back to the times typed", "Flytta tillbaka dem till de inskrivna tiderna",
     "Devolverlos a las horas escritas"},
    {"Got it - they're right as they are", "Uppfattat - de stämmer som de är", "Entendido - están bien así"},
    {"Got it - I've read about the time zone", "Uppfattat - jag har läst om tidszonen",
     "Entendido - he leído lo de la zona horaria"},
    {"Nothing to move: that's been done, or put away", "Inget att flytta: det är redan gjort, eller undanlagt",
     "Nada que mover: ya se hizo, o se descartó"},
    {"%s moved %d time typed before the time zone changed back to the times typed",
     "%s flyttade tillbaka %d tid som skrevs in före bytet av tidszon till tiden den skrevs in med",
     "%s devolvió %d hora escrita antes del cambio de zona horaria a la hora en que se escribió"},
    {"%s moved %d times typed before the time zone changed back to the times typed",
     "%s flyttade tillbaka %d tider som skrevs in före bytet av tidszon till tiderna de skrevs in med",
     "%s devolvió %d horas escritas antes del cambio de zona horaria a las horas en que se escribieron"},
    {"Moved %d back to the times typed", "Flyttade tillbaka %d till de inskrivna tiderna",
     "Se devolvieron %d a las horas escritas"},

    /* the checks: the database and saving */
    {"Database rescued", "Databasen räddad", "Base de datos rescatada"},
    {"Going back to a backup from before (Backups) brings the lost rows back - and loses what was added since. "
     "Otherwise carry on: everything that could be read is here.",
     "Att gå tillbaka till en säkerhetskopia från tidigare (Säkerhetskopior) tar tillbaka de förlorade raderna - och "
     "tappar det som lagts till sedan dess. Annars kör på: allt som gick att läsa finns här.",
     "Volver a una copia de seguridad anterior (Copias de seguridad) recupera las filas perdidas - y pierde lo añadido "
     "desde entonces. Si no, sigue adelante: aquí está todo lo que se pudo leer."},
    {"Got it - I've read about the rescued database", "Uppfattat - jag har läst om den räddade databasen",
     "Entendido - he leído lo de la base de datos rescatada"},
    {"Noted - the damaged file stays in the data folder until you delete it",
     "Noterat - den skadade filen ligger kvar i datamappen tills du tar bort den",
     "Anotado - el archivo dañado se queda en la carpeta de datos hasta que lo borres"},
    {"Saving", "Sparande", "Guardado de datos"},
    {"Saving failed: the database file is damaged. Recent changes are only in memory.",
     "Det gick inte att spara: databasfilen är skadad. De senaste ändringarna finns bara i minnet.",
     "No se pudo guardar: el archivo de la base de datos está dañado. Los últimos cambios solo están en memoria."},
    {"Restart Hemma: at the start it copies the damaged file aside and rescues every row it can still read into a fresh "
     "one.",
     "Starta om Hemma: vid starten kopierar den undan den skadade filen och räddar varje rad den fortfarande kan läsa "
     "till en ny.",
     "Reinicia Hemma: al arrancar aparta una copia del archivo dañado y rescata en uno nuevo cada fila que aún pueda "
     "leer."},
    {"Make room on the disk with %s, then restart Hemma. The log says what failed.",
     "Gör plats på disken med %s och starta sedan om Hemma. Loggen säger vad som gick fel.",
     "Libera espacio en el disco con %s y después reinicia Hemma. El registro dice qué falló."},
    {"Saving changes to the disk failed - is it full? Recent changes are only in memory and will be lost when Hemma "
     "stops.",
     "Det gick inte att spara ändringar på disken - är den full? De senaste ändringarna finns bara i minnet och "
     "försvinner när Hemma stannar.",
     "No se pudieron guardar los cambios en el disco - ¿está lleno? Los últimos cambios solo están en memoria y se "
     "perderán cuando Hemma se detenga."},
    {"Every change is saved in %s/hemma.db.", "Varje ändring sparas i %s/hemma.db.", "Cada cambio se guarda en %s/hemma.db."},

    /* the checks: signing in */
    {"Sign-in", "Inloggning", "Inicio de sesión"},
    {"Add one to the settings file (%s) and restart Hemma.", "Lägg till en i inställningsfilen (%s) och starta om Hemma.",
     "Añade uno al archivo de ajustes (%s) y reinicia Hemma."},
    {"There's no password secret, so nobody can sign in or be added.",
     "Det finns ingen lösenordshemlighet, så ingen kan logga in eller läggas till.",
     "No hay secreto de las contraseñas, así que nadie puede iniciar sesión ni ser añadido."},
    {"The secret is the line HEMMA_AUTH_SALT=... in the settings file; hemma --gen-salt makes one.",
     "Hemligheten är raden HEMMA_AUTH_SALT=... i inställningsfilen; hemma --gen-salt gör en.",
     "El secreto es la línea HEMMA_AUTH_SALT=... del archivo de ajustes; hemma --gen-salt crea uno."},
    {"Put the original line back into %s and restart Hemma (docs/accounts.md, \"Locked out\").",
     "Lägg tillbaka den ursprungliga raden i %s och starta om Hemma (docs/accounts.md, \"Locked out\").",
     "Vuelve a poner la línea original en %s y reinicia Hemma (docs/accounts.md, \"Locked out\")."},
    {"The password secret (a line in the settings file) isn't the one the passwords were made with, so nobody can sign "
     "in with a password.",
     "Lösenordshemligheten (en rad i inställningsfilen) är inte den som lösenorden gjordes med, så ingen kan logga in "
     "med lösenord.",
     "El secreto de las contraseñas (una línea del archivo de ajustes) no es con el que se hicieron las contraseñas, así "
     "que nadie puede iniciar sesión con contraseña."},
    {"The secret is the line HEMMA_AUTH_SALT=... in the settings file.",
     "Hemligheten är raden HEMMA_AUTH_SALT=... i inställningsfilen.",
     "El secreto es la línea HEMMA_AUTH_SALT=... del archivo de ajustes."},
    {"The welcome page is still open: whoever has the setup code can make the first admin.",
     "Välkomstsidan är fortfarande öppen: den som har inställningskoden kan skapa den första administratören.",
     "La página de bienvenida sigue abierta: quien tenga el código de configuración puede crear el primer administrador."},
    {"Open Hemma and create the admin account - the setup code is in the log.",
     "Öppna Hemma och skapa administratörskontot - inställningskoden står i loggen.",
     "Abre Hemma y crea la cuenta de administrador - el código de configuración está en el registro."},
    {"%d account can sign in, %d of them admin.", "%d konto kan logga in, %d av dem är administratör.",
     "%d cuenta puede iniciar sesión, %d de ellas es de administrador."},
    {"%d account can sign in, %d of them admins.", "%d konto kan logga in, %d av dem är administratörer.",
     "%d cuenta puede iniciar sesión, %d de ellas son de administrador."},
    {"%d accounts can sign in, %d of them admin.", "%d konton kan logga in, %d av dem är administratör.",
     "%d cuentas pueden iniciar sesión, %d de ellas es de administrador."},
    {"%d accounts can sign in, %d of them admins.", "%d konton kan logga in, %d av dem är administratörer.",
     "%d cuentas pueden iniciar sesión, %d de ellas son de administrador."},
    {"Tip: a second admin can help if you're ever locked out.",
     "Tips: en andra administratör kan hjälpa till om du någon gång blir utelåst.",
     "Consejo: un segundo administrador puede ayudarte si alguna vez te quedas fuera."},

    /* the checks: the log */
    {"Hemma can't keep its log file (%s): %s.", "Hemma kan inte föra sin loggfil (%s): %s.",
     "Hemma no puede mantener su archivo de registro (%s): %s."},
    {"writing to it failed (is the disk full?)", "det gick inte att skriva till den (är disken full?)",
     "no se pudo escribir en él (¿está lleno el disco?)"},
    {"it can't be opened for writing", "den går inte att öppna för skrivning", "no se puede abrir para escribir"},
    {"Check that the data folder can be written to and the disk isn't full. The log below still works.",
     "Kontrollera att det går att skriva i datamappen och att disken inte är full. Loggen nedan fungerar ändå.",
     "Comprueba que se puede escribir en la carpeta de datos y que el disco no está lleno. El registro de abajo sigue "
     "funcionando."},
    {"%d problem logged in the last day.", "%d problem loggat det senaste dygnet.",
     "%d problema registrado en el último día."},
    {"%d problems logged in the last day.", "%d problem loggade det senaste dygnet.",
     "%d problemas registrados en el último día."},
    {"Problems in the log", "Problem i loggen", "Problemas en el registro"},
    {"Have a look: each line says what went wrong.", "Ta en titt: varje rad säger vad som gick fel.",
     "Échale un vistazo: cada línea dice qué salió mal."},
    {"None logged since Hemma started.", "Inga loggade sedan Hemma startade.",
     "Ninguno registrado desde que Hemma arrancó."},

    /* the checks: disk space (one row per disk) */
    {"%s and %s", "%s och %s", "%s y %s"},
    {"Hemma's data", "Hemmas data", "los datos de Hemma"},
    {"%s free on the disk with %s (%d%% used).", "%s ledigt på disken med %s (%d%% använt).",
     "%s libres en el disco con %s (%d%% usado)."},
    {"%s wants %s free %s.", "%s vill ha %s ledigt %s.", "%s quiere %s libres %s."},
    {"Hemma wants %s free to save changes and keep its log - under 300 MB, saving fails soon.",
     "Hemma vill ha %s ledigt för att spara ändringar och föra sin logg - under 300 MB slutar sparandet snart att "
     "fungera.",
     "Hemma quiere %s libres para guardar cambios y mantener su registro - por debajo de 300 MB, pronto no podrá "
     "guardar."},
    {"Hemma wants %s free to save changes and keep its log.",
     "Hemma vill ha %s ledigt för att spara ändringar och föra sin logg.",
     "Hemma quiere %s libres para guardar cambios y mantener su registro."},
    {"Disk space", "Diskutrymme", "Espacio en disco"},
    {"Disk space (%.60s)", "Diskutrymme (%.60s)", "Espacio en disco (%.60s)"},
    {"Make room on that disk: when it's full, Hemma can't save changes (old videos? the Watch library settings show what "
     "takes room).",
     "Gör plats på den disken: när den är full kan Hemma inte spara ändringar (gamla videor? Biblioteksinställningarna "
     "under Titta visar vad som tar plats).",
     "Libera espacio en ese disco: cuando está lleno, Hemma no puede guardar cambios (¿vídeos viejos? los ajustes de la "
     "biblioteca en Ver muestran qué ocupa espacio)."},

    /* getting started */
    {"Getting started", "Kom igång", "Primeros pasos"},
    {"%d of %d done", "%d av %d klara", "%d de %d hechos"},
    {"Still to do", "Kvar att göra", "Por hacer"},
    {"Next: <a href=\"%s\"><b>%s</b></a>", "Nästa: <a href=\"%s\"><b>%s</b></a>",
     "Siguiente: <a href=\"%s\"><b>%s</b></a>"},
    {"Name your home", "Namnge ditt hem", "Ponle nombre a tu casa"},
    {"The name at the top of every page - \"The Svenssons\", \"Villa Kulla\"...",
     "Namnet överst på varje sida - \"Familjen Svensson\", \"Villa Villekulla\"...",
     "El nombre en lo alto de cada página - \"Los García\", \"Villa Kunterbunt\"..."},
    {"Add the household", "Lägg till hushållet", "Añade a los de casa"},
    {"Everyone who lives here, even the ones too young to sign in: each gets a colour, and a sign-in if they like.",
     "Alla som bor här, även de som är för små för att logga in: var och en får en färg, och en inloggning om de vill.",
     "Todos los que viven aquí, incluso los demasiado pequeños para iniciar sesión: cada uno tiene un color, y un inicio "
     "de sesión si quiere."},
    {"On a phone at home, open %s and sign in. Tip: add it to the home screen - it opens like an app.",
     "Öppna %s på en telefon hemma och logga in. Tips: lägg den på hemskärmen - den öppnas som en app.",
     "En un móvil de casa, abre %s e inicia sesión. Consejo: añádelo a la pantalla de inicio - se abre como una app."},
    {"On a phone at home, open Hemma's address (see Address in the Checks) and sign in. Tip: add it to the home screen - "
     "it opens like an app.",
     "Öppna Hemmas adress (se Adress under Kontroller) på en telefon hemma och logga in. Tips: lägg den på hemskärmen - "
     "den öppnas som en app.",
     "En un móvil de casa, abre la dirección de Hemma (mira Dirección en las Comprobaciones) e inicia sesión. Consejo: "
     "añádela a la pantalla de inicio - se abre como una app."},
    {"Open Hemma on a phone", "Öppna Hemma på en telefon", "Abre Hemma en un móvil"},
    {"Save your recovery codes", "Spara dina återställningskoder", "Guarda tus códigos de recuperación"},
    {"They get you back in if you forget your password. Keep them somewhere safe, away from this server.",
     "De tar dig in igen om du glömmer ditt lösenord. Förvara dem någonstans säkert, inte på den här servern.",
     "Te dejan entrar de nuevo si olvidas tu contraseña. Guárdalos en un sitio seguro, lejos de este servidor."},
    {"Every password depends on the password secret (a line in the settings file, %s, in the folder Hemma was started "
     "from). Keep a copy of that line somewhere safe, apart from Hemma's data - a password manager, or a note in a "
     "drawer. The nightly backups can't sign anyone in on a new install without it (a Download from Backups has it in).",
     "Varje lösenord hänger på lösenordshemligheten (en rad i inställningsfilen, %s, i mappen Hemma startades från). "
     "Spara en kopia av raden någonstans säkert, skild från Hemmas data - en lösenordshanterare, eller en lapp i en "
     "låda. Nattens säkerhetskopior kan inte logga in någon på en ny installation utan den (det du laddar ner under "
     "Säkerhetskopior har den med).",
     "Cada contraseña depende del secreto de las contraseñas (una línea del archivo de ajustes, %s, en la carpeta desde "
     "la que se inició Hemma). Guarda una copia de esa línea en un sitio seguro, aparte de los datos de Hemma - un "
     "gestor de contraseñas, o una nota en un cajón. Las copias de seguridad nocturnas no pueden iniciar la sesión de "
     "nadie en una instalación nueva sin ella (lo que descargas en Copias de seguridad la incluye)."},
    {"Every password depends on the password secret (a line in the settings file, %s). Keep a copy of that line "
     "somewhere safe, apart from Hemma's data - a password manager, or a note in a drawer. The nightly backups can't "
     "sign anyone in on a new install without it (a Download from Backups has it in).",
     "Varje lösenord hänger på lösenordshemligheten (en rad i inställningsfilen, %s). Spara en kopia av raden någonstans "
     "säkert, skild från Hemmas data - en lösenordshanterare, eller en lapp i en låda. Nattens säkerhetskopior kan inte "
     "logga in någon på en ny installation utan den (det du laddar ner under Säkerhetskopior har den med).",
     "Cada contraseña depende del secreto de las contraseñas (una línea del archivo de ajustes, %s). Guarda una copia de "
     "esa línea en un sitio seguro, aparte de los datos de Hemma - un gestor de contraseñas, o una nota en un cajón. "
     "Las copias de seguridad nocturnas no pueden iniciar la sesión de nadie en una instalación nueva sin ella (lo que "
     "descargas en Copias de seguridad la incluye)."},
    {"Keep a copy of the password secret", "Spara en kopia av lösenordshemligheten",
     "Guarda una copia del secreto de las contraseñas"},
    {"The line starts HEMMA_AUTH_SALT=. On the server: sudo grep HEMMA_AUTH_SALT /etc/hemma/hemma.env",
     "Raden börjar med HEMMA_AUTH_SALT=. På servern: sudo grep HEMMA_AUTH_SALT /etc/hemma/hemma.env",
     "La línea empieza por HEMMA_AUTH_SALT=. En el servidor: sudo grep HEMMA_AUTH_SALT /etc/hemma/hemma.env"},
    {"The line starts HEMMA_AUTH_SALT=.", "Raden börjar med HEMMA_AUTH_SALT=.", "La línea empieza por HEMMA_AUTH_SALT=."},
    {"Done - I have a copy", "Klart - jag har en kopia", "Hecho - tengo una copia"},
    {"Ticked off - keep that copy safe", "Avbockat - förvara kopian säkert", "Marcado - guarda bien esa copia"},
    {"All done - Hemma is set up.", "Allt klart - Hemma är inställt.", "Todo hecho - Hemma está configurado."},
    {"Hide this", "Dölj det här", "Ocultar esto"},
    {"Hidden - it can come back at the bottom of this page", "Dold - den kan komma tillbaka längst ner på den här sidan",
     "Oculta - puede volver desde el final de esta página"},
    {"Show the getting-started list again", "Visa kom igång-listan igen", "Volver a mostrar la lista de primeros pasos"},

    /* the home card */
    {"Something isn't working", "Något fungerar inte", "Algo no funciona"},
    {"Something needs a look", "Något behöver ses över", "Algo necesita revisión"},

    /* the log tab */
    {"All", "Alla", "Todos"},
    {"Warnings & problems", "Varningar och fel", "Avisos y problemas"},
    {"Search the log…", "Sök i loggen…", "Buscar en el registro…"},
    {"Search the log", "Sök i loggen", "Buscar en el registro"},
    {"Download the log", "Ladda ner loggen", "Descargar el registro"},
    {"Detailed logging until Hemma restarts", "Detaljerad loggning tills Hemma startar om",
     "Registro detallado hasta que Hemma se reinicie"},
    {"Detailed logging is on until Hemma restarts", "Detaljerad loggning är på tills Hemma startar om",
     "El registro detallado está activado hasta que Hemma se reinicie"},
    {"Detailed logging is off", "Detaljerad loggning är av", "El registro detallado está desactivado"},
    {"Nothing in the log matches that.", "Inget i loggen matchar det.", "Nada en el registro coincide con eso."},
    {"No warnings or problems in the log.", "Inga varningar eller fel i loggen.",
     "No hay avisos ni problemas en el registro."},
    {"Nothing logged yet.", "Inget loggat än.", "Nada registrado todavía."},
    {"Newest first, the latest 200 · %d lines match (all of them in the download)",
     "Nyast först, de senaste 200 · %d rader matchar (alla finns i nedladdningen)",
     "Lo más nuevo primero, las últimas 200 · %d líneas coinciden (todas están en la descarga)"},
    {"Newest first, the latest 200 · %d lines with warnings or problems (all of them in the download)",
     "Nyast först, de senaste 200 · %d rader med varningar eller fel (alla finns i nedladdningen)",
     "Lo más nuevo primero, las últimas 200 · %d líneas con avisos o problemas (todas están en la descarga)"},
    {"Newest first, the latest 200 · %d lines (all of them in the download)",
     "Nyast först, de senaste 200 · %d rader (alla finns i nedladdningen)",
     "Lo más nuevo primero, las últimas 200 · %d líneas (todas están en la descarga)"},
    {"Newest first · %d line matches", "Nyast först · %d rad matchar", "Lo más nuevo primero · %d línea coincide"},
    {"Newest first · %d lines match", "Nyast först · %d rader matchar", "Lo más nuevo primero · %d líneas coinciden"},
    {"Newest first · %d line with warnings or problems", "Nyast först · %d rad med varningar eller fel",
     "Lo más nuevo primero · %d línea con avisos o problemas"},
    {"Newest first · %d lines with warnings or problems", "Nyast först · %d rader med varningar eller fel",
     "Lo más nuevo primero · %d líneas con avisos o problemas"},
    {"Newest first · %d line", "Nyast först · %d rad", "Lo más nuevo primero · %d línea"},
    {"Newest first · %d lines", "Nyast först · %d rader", "Lo más nuevo primero · %d líneas"},
    {"problem", "fel", "problema"},
    {"warning", "varning", "aviso"},
    {"detail", "detalj", "detalle"},
    /* the log's dividers (log.c keeps them in English) */
    {"Hemma restarted", "Hemma startade om", "Hemma se reinició"},
    {"Hemma restarted - it had stopped without warning (a power cut or a crash?)",
     "Hemma startade om - den hade stannat utan förvarning (ett strömavbrott eller en krasch?)",
     "Hemma se reinició - se había detenido sin avisar (¿un corte de luz o un fallo?)"},
    {"Hemma %s started", "Hemma %s startade", "Hemma %s arrancó"},
    {"Hemma stopped", "Hemma stannade", "Hemma se detuvo"},

    /* the help tab */
    {"Where to look", "Var du ska titta", "Dónde mirar"},
    {"The <a href=\"/system/log\">Log</a> tab shows what Hemma is doing, live - turn on detailed logging there while you "
     "chase a problem. The <a href=\"/system\">Checks</a> say in plain words what works and what doesn't.",
     "Fliken <a href=\"/system/log\">Logg</a> visar vad Hemma gör, live - slå på detaljerad loggning där medan du jagar "
     "ett problem. <a href=\"/system\">Kontrollerna</a> säger med enkla ord vad som fungerar och vad som inte gör det.",
     "La pestaña <a href=\"/system/log\">Registro</a> muestra lo que hace Hemma, en directo - activa allí el registro "
     "detallado mientras persigues un problema. Las <a href=\"/system\">Comprobaciones</a> dicen con palabras sencillas "
     "qué funciona y qué no."},
    {"On the server, where Hemma runs as the service <code>hemma</code>:",
     "På servern, där Hemma körs som tjänsten <code>hemma</code>:",
     "En el servidor, donde Hemma se ejecuta como el servicio <code>hemma</code>:"},
    {"running or not, and its last lines", "om den körs eller inte, och dess sista rader",
     "si está en marcha o no, y sus últimas líneas"},
    {"follow the log live (Ctrl+C stops watching)", "följ loggen live (Ctrl+C slutar följa)",
     "sigue el registro en directo (Ctrl+C deja de mirar)"},
    {"the last 100 lines", "de sista 100 raderna", "las últimas 100 líneas"},
    {"restart it (after changing the settings file)", "starta om den (efter att du ändrat inställningsfilen)",
     "reinícialo (después de cambiar el archivo de ajustes)"},
    {"On the Mac, where Hemma starts when you log in:", "På Macen, där Hemma startar när du loggar in:",
     "En el Mac, donde Hemma arranca cuando inicias sesión:"},
    {"follow what it prints (Ctrl+C stops watching)", "följ vad den skriver ut (Ctrl+C slutar följa)",
     "sigue lo que escribe (Ctrl+C deja de mirar)"},
    {"restart it", "starta om den", "reinícialo"},
    {"The window Hemma runs in shows the same lines. Started in the background (docs/windows.md)? Then follow its file "
     "in PowerShell:",
     "Fönstret Hemma körs i visar samma rader. Startad i bakgrunden (docs/windows.md)? Följ då dess fil i PowerShell:",
     "La ventana en la que se ejecuta Hemma muestra las mismas líneas. ¿Lo iniciaste en segundo plano "
     "(docs/windows.md)? Entonces sigue su archivo en PowerShell:"},
    {"follow the log (Ctrl+C stops watching)", "följ loggen (Ctrl+C slutar följa)",
     "sigue el registro (Ctrl+C deja de mirar)"},
    {"The window Hemma was started from shows every line, as it happens. Ctrl+C there stops Hemma.",
     "Fönstret Hemma startades från visar varje rad, när den händer. Ctrl+C där stoppar Hemma.",
     "La ventana desde la que se inició Hemma muestra cada línea en el momento. Ctrl+C allí detiene Hemma."},
    {"Hemma keeps its own copy too: <code>%s</code>, and the one before it (<code>hemma.log.1</code>, next to it). <a "
     "href=\"/system/log.txt\" data-native download>The download</a> holds both.",
     "Hemma sparar en egen kopia också: <code>%s</code>, och den före den (<code>hemma.log.1</code>, bredvid). <a "
     "href=\"/system/log.txt\" data-native download>Nedladdningen</a> har båda.",
     "Hemma también guarda su propia copia: <code>%s</code>, y la anterior (<code>hemma.log.1</code>, al lado). <a "
     "href=\"/system/log.txt\" data-native download>La descarga</a> contiene las dos."},
    {"Hemma keeps its own copy too: hemma.log in the data folder, and the one before it (<code>hemma.log.1</code>, next "
     "to it). <a href=\"/system/log.txt\" data-native download>The download</a> holds both.",
     "Hemma sparar en egen kopia också: hemma.log i datamappen, och den före den (<code>hemma.log.1</code>, bredvid). <a "
     "href=\"/system/log.txt\" data-native download>Nedladdningen</a> har båda.",
     "Hemma también guarda su propia copia: hemma.log en la carpeta de datos, y la anterior (<code>hemma.log.1</code>, "
     "al lado). <a href=\"/system/log.txt\" data-native download>La descarga</a> contiene las dos."},
    {"Where things are", "Var saker finns", "Dónde están las cosas"},
    {"%s (in the folder Hemma was started from)", "%s (i mappen Hemma startades från)",
     "%s (en la carpeta desde la que se inició Hemma)"},
    {"the settings file, with the password secret (HEMMA_AUTH_SALT)",
     "inställningsfilen, med lösenordshemligheten (HEMMA_AUTH_SALT)",
     "el archivo de ajustes, con el secreto de las contraseñas (HEMMA_AUTH_SALT)"},
    {"Data", "Datamapp", "Datos"},
    {"the database (hemma.db) and the log", "databasen (hemma.db) och loggen", "la base de datos (hemma.db) y el registro"},
    {"Version", "Version", "Versión"},
    {"running since %s", "körs sedan %s", "en marcha desde %s"},
    {"When something's wrong", "När något är fel", "Cuando algo va mal"},
    {"<b>A phone or the TV can't open Hemma.</b> Use the address on the Checks tab and type the <code>http://</code> "
     "part (not https). The device must be on the home Wi-Fi; a Windows PC running Hemma must let it through its "
     "firewall. (docs/home-network.md)",
     "<b>En telefon eller tv:n kan inte öppna Hemma.</b> Använd adressen på fliken Kontroller och skriv "
     "<code>http://</code>-delen (inte https). Enheten måste vara på hemmets wifi; en Windows-dator som kör Hemma måste "
     "släppa igenom den i sin brandvägg. (docs/home-network.md)",
     "<b>Un móvil o la tele no puede abrir Hemma.</b> Usa la dirección de la pestaña Comprobaciones y escribe la parte "
     "<code>http://</code> (no https). El dispositivo tiene que estar en el wifi de casa; un PC con Windows que ejecute "
     "Hemma tiene que dejarlo pasar por su cortafuegos. (docs/home-network.md)"},
    {"<b>Nobody can sign in.</b> The Checks tab says if the password secret changed. One forgotten password: a recovery "
     "code, or <code>hemma --reset-password NAME</code> on the server. (docs/accounts.md)",
     "<b>Ingen kan logga in.</b> Fliken Kontroller säger om lösenordshemligheten har ändrats. Ett glömt lösenord: en "
     "återställningskod, eller <code>hemma --reset-password NAME</code> på servern. (docs/accounts.md)",
     "<b>Nadie puede iniciar sesión.</b> La pestaña Comprobaciones dice si cambió el secreto de las contraseñas. Una "
     "contraseña olvidada: un código de recuperación, o <code>hemma --reset-password NAME</code> en el servidor. "
     "(docs/accounts.md)"},
    {"<b>Uploads stop or can't be saved.</b> The Checks say whether Hemma may write into each library folder and how "
     "much room is left.",
     "<b>Uppladdningar stannar eller kan inte sparas.</b> Kontrollerna säger om Hemma får skriva i varje biblioteksmapp "
     "och hur mycket plats som finns kvar.",
     "<b>Las subidas se detienen o no se pueden guardar.</b> Las Comprobaciones dicen si Hemma puede escribir en cada "
     "carpeta de la biblioteca y cuánto espacio queda."},
    {"<b>A video won't play.</b> Watch → Library settings converts it into a copy that plays everywhere.",
     "<b>En video spelas inte.</b> Titta → Biblioteksinställningar konverterar den till en kopia som spelas överallt.",
     "<b>Un vídeo no se reproduce.</b> Ver → Ajustes de la biblioteca lo convierte en una copia que se reproduce en "
     "todas partes."},
    {"<b>Hemma doesn't start.</b> Its last lines say why - usually another program on the same port, or a typo in the "
     "settings file.",
     "<b>Hemma startar inte.</b> Dess sista rader säger varför - oftast ett annat program på samma port, eller ett "
     "stavfel i inställningsfilen.",
     "<b>Hemma no arranca.</b> Sus últimas líneas dicen por qué - normalmente otro programa en el mismo puerto, o una "
     "errata en el archivo de ajustes."},
    {"More in docs/troubleshooting.md, in Hemma's folder or its repository.",
     "Mer i docs/troubleshooting.md, i Hemmas mapp eller i dess repo.",
     "Más en docs/troubleshooting.md, en la carpeta de Hemma o en su repositorio."},
    {"Asking for help", "Be om hjälp", "Pedir ayuda"},
    {"Send the version (<b>Hemma %s</b>), what you did and what happened, and the log:",
     "Skicka versionen (<b>Hemma %s</b>), vad du gjorde och vad som hände, och loggen:",
     "Envía la versión (<b>Hemma %s</b>), qué hiciste y qué pasó, y el registro:"},
    {"The log names devices and people at home: share it only with someone you trust.",
     "Loggen nämner enheter och personer hemma: dela den bara med någon du litar på.",
     "El registro nombra dispositivos y personas de casa: compártelo solo con alguien de confianza."},
    {"For scripts: <code>%s/healthz</code> answers <code>ok</code> while Hemma runs.",
     "För skript: <code>%s/healthz</code> svarar <code>ok</code> medan Hemma körs.",
     "Para scripts: <code>%s/healthz</code> responde <code>ok</code> mientras Hemma funciona."},

    /* backups: the checks */
    {"Backup put back", "Säkerhetskopia återställd", "Copia de seguridad restaurada"},
    {"The backup %s waits to be put back: Hemma does it at its next start.",
     "Säkerhetskopian %s väntar på att återställas: Hemma gör det vid nästa start.",
     "La copia de seguridad %s espera a ser restaurada: Hemma lo hace en su próximo arranque."},
    {"Going back to a backup", "Tillbaka till en säkerhetskopia", "Volver a una copia de seguridad"},
    {"Restart Hemma to finish it - or say \"Don't put it back\" on the Backups tab.",
     "Starta om Hemma för att göra klart - eller välj \"Återställ den inte\" på fliken Säkerhetskopior.",
     "Reinicia Hemma para terminarlo - o elige \"No restaurarla\" en la pestaña Copias de seguridad."},
    {"The backup at %s failed: %s", "Säkerhetskopian %s misslyckades: %s", "La copia de seguridad de %s falló: %s"},
    {"See the Backups tab: is the folder there, and may Hemma write to it?",
     "Se fliken Säkerhetskopior: finns mappen där, och får Hemma skriva i den?",
     "Mira la pestaña Copias de seguridad: ¿existe la carpeta, y puede Hemma escribir en ella?"},
    {"No backup yet: the first one is made a few minutes after Hemma starts, then one every night.",
     "Ingen säkerhetskopia än: den första görs några minuter efter att Hemma startat, sedan en varje natt.",
     "Todavía no hay copia de seguridad: la primera se hace unos minutos después de que arranque Hemma, y luego una cada "
     "noche."},
    {"The last one: %s (checked, %lld row), the only one kept in %s.",
     "Den senaste: %s (kontrollerad, %lld rad), den enda som sparas i %s.",
     "La última: %s (comprobada, %lld fila), la única guardada en %s."},
    {"The last one: %s (checked, %lld rows), the only one kept in %s.",
     "Den senaste: %s (kontrollerad, %lld rader), den enda som sparas i %s.",
     "La última: %s (comprobada, %lld filas), la única guardada en %s."},
    {"The last one: %s (checked, %lld row), %d kept in %s.", "Den senaste: %s (kontrollerad, %lld rad), %d sparade i %s.",
     "La última: %s (comprobada, %lld fila), %d guardadas en %s."},
    {"The last one: %s (checked, %lld rows), %d kept in %s.",
     "Den senaste: %s (kontrollerad, %lld rader), %d sparade i %s.",
     "La última: %s (comprobada, %lld filas), %d guardadas en %s."},
    {"The last one: %s (checked, %lld row).", "Den senaste: %s (kontrollerad, %lld rad).",
     "La última: %s (comprobada, %lld fila)."},
    {"The last one: %s (checked, %lld rows).", "Den senaste: %s (kontrollerad, %lld rader).",
     "La última: %s (comprobada, %lld filas)."},
    {"The last one: %s (checked), the only one kept in %s.", "Den senaste: %s (kontrollerad), den enda som sparas i %s.",
     "La última: %s (comprobada), la única guardada en %s."},
    {"The last one: %s (checked), %d kept in %s.", "Den senaste: %s (kontrollerad), %d sparade i %s.",
     "La última: %s (comprobada), %d guardadas en %s."},
    {"The last one: %s (checked).", "Den senaste: %s (kontrollerad).", "La última: %s (comprobada)."},
    {"Back up now on the Backups tab - and see in the log why the nightly one didn't run.",
     "Säkerhetskopiera nu på fliken Säkerhetskopior - och se i loggen varför nattens inte kördes.",
     "Haz una copia ahora en la pestaña Copias de seguridad - y mira en el registro por qué no se hizo la de la noche."},
    {"They're on the same disk as the data: if that disk dies, they go with it. A folder on another disk (a USB disk) "
     "keeps them safe - choose one on the Backups tab.",
     "De ligger på samma disk som datan: om disken dör följer de med. En mapp på en annan disk (en USB-disk) håller dem "
     "säkra - välj en på fliken Säkerhetskopior.",
     "Están en el mismo disco que los datos: si ese disco muere, se van con él. Una carpeta en otro disco (un disco USB) "
     "las mantiene a salvo - elige una en la pestaña Copias de seguridad."},
    {"the folder %s can't be made - is the disk there, and may Hemma write to it?",
     "mappen %s går inte att skapa - finns disken där, och får Hemma skriva på den?",
     "no se puede crear la carpeta %s - ¿está el disco, y puede Hemma escribir en él?"},

    /* backups: the tab */
    {"Going back to %s: Hemma restarts now to put it in place. This page comes back in a few seconds.",
     "Tillbaka till %s: Hemma startar om nu för att lägga den på plats. Sidan kommer tillbaka om några sekunder.",
     "Volviendo a %s: Hemma se reinicia ahora para ponerla en su sitio. Esta página vuelve en unos segundos."},
    {"<b>%s waits to be put back.</b> Restart Hemma to finish it: it's put in place as Hemma starts, and what's there now "
     "is kept aside.",
     "<b>%s väntar på att återställas.</b> Starta om Hemma för att göra klart: den läggs på plats när Hemma startar, och "
     "det som finns nu sparas undan.",
     "<b>%s espera a ser restaurada.</b> Reinicia Hemma para terminarlo: se pone en su sitio al arrancar Hemma, y lo que "
     "hay ahora se guarda aparte."},
    {"Don't put it back", "Återställ den inte", "No restaurarla"},
    {"Backing up…", "Säkerhetskopierar…", "Haciendo la copia…"},
    {"Getting the backup ready…", "Gör i ordning säkerhetskopian…", "Preparando la copia de seguridad…"},
    {"Looking in the backup folder…", "Tittar i mappen för säkerhetskopior…", "Mirando en la carpeta de copias…"},
    {"Backed up and checked: %s.", "Säkerhetskopierad och kontrollerad: %s.", "Copia hecha y comprobada: %s."},
    {"The backup failed: %s", "Säkerhetskopieringen misslyckades: %s", "La copia de seguridad falló: %s"},
    {"Not put back: %s", "Inte återställd: %s", "No restaurada: %s"},
    {"Every night Hemma copies its database, checks the copy, and keeps the newest of the last %d days.",
     "Varje natt kopierar Hemma sin databas, kontrollerar kopian och sparar den nyaste från var och en av de senaste %d "
     "dagarna.",
     "Cada noche Hemma copia su base de datos, comprueba la copia y guarda la más reciente de cada uno de los últimos %d "
     "días."},
    {"In <code>%s</code>", "I <code>%s</code>", "En <code>%s</code>"},
    {"In <code>%s</code> - on the same disk as the data", "I <code>%s</code> - på samma disk som datan",
     "En <code>%s</code> - en el mismo disco que los datos"},
    {"In <code>%s</code> - on another disk than the data", "I <code>%s</code> - på en annan disk än datan",
     "En <code>%s</code> - en otro disco distinto al de los datos"},
    {"Back up now", "Säkerhetskopiera nu", "Hacer copia ahora"},
    {"No backups yet.", "Inga säkerhetskopior än.", "Todavía no hay copias de seguridad."},
    {"Includes the password secret", "Innehåller lösenordshemligheten", "Incluye el secreto de las contraseñas"},
    {"Download", "Ladda ner", "Descargar"},
    {"Go back to the backup of %s? Everything changed since then is lost - what's there now is kept aside as a copy. "
     "Hemma restarts to do it.",
     "Gå tillbaka till säkerhetskopian från %s? Allt som ändrats sedan dess försvinner - det som finns nu sparas undan "
     "som en kopia. Hemma startar om för att göra det.",
     "¿Volver a la copia de seguridad de %s? Se pierde todo lo cambiado desde entonces - lo que hay ahora se guarda "
     "aparte como copia. Hemma se reinicia para hacerlo."},
    {"Go back to this", "Gå tillbaka till den här", "Volver a esta"},
    {"A new memory card or computer", "Ett nytt minneskort eller en ny dator", "Una tarjeta de memoria u ordenador nuevos"},
    {"<b>Download</b> gives you a backup as one file: the database and the password secret (<code>HEMMA_AUTH_SALT</code>) "
     "every password is made with. Keep it like a password. On a new memory card or another computer, install Hemma, "
     "then <code>hemma --restore</code> that file - docs/raspberry-pi.md, \"Backups\", has the steps.",
     "<b>Ladda ner</b> ger dig en säkerhetskopia som en enda fil: databasen och lösenordshemligheten "
     "(<code>HEMMA_AUTH_SALT</code>) som varje lösenord görs med. Förvara den som ett lösenord. På ett nytt minneskort "
     "eller en annan dator: installera Hemma och kör sedan <code>hemma --restore</code> på filen - "
     "docs/raspberry-pi.md, \"Backups\", har stegen.",
     "<b>Descargar</b> te da una copia de seguridad en un solo archivo: la base de datos y el secreto de las contraseñas "
     "(<code>HEMMA_AUTH_SALT</code>) con el que se hace cada contraseña. Guárdalo como una contraseña. En una tarjeta de "
     "memoria u ordenador nuevos, instala Hemma y luego ejecuta <code>hemma --restore</code> con ese archivo - "
     "docs/raspberry-pi.md, \"Backups\", tiene los pasos."},
    {"The nightly backups leave the secret out: on their own they can't sign anyone in on a new install - keep a copy "
     "of the secret (Getting started) or a download.",
     "Nattens säkerhetskopior har inte med hemligheten: ensamma kan de inte logga in någon på en ny installation - spara "
     "en kopia av hemligheten (Kom igång) eller en nedladdning.",
     "Las copias nocturnas no incluyen el secreto: por sí solas no permiten iniciar sesión a nadie en una instalación "
     "nueva - guarda una copia del secreto (Primeros pasos) o una descarga."},
    {"Where they go", "Var de hamnar", "Dónde se guardan"},
    {"Backup folder", "Mapp för säkerhetskopior", "Carpeta de copias de seguridad"},
    {"Hemma chooses", "Hemma väljer", "Hemma elige"},
    {"Empty: Hemma chooses - a hidden .hemma-backups folder in the first library folder on another disk than the data, "
     "else %s/backups. Hemma must be allowed to write there (on a Pi: a folder you gave install-pi.sh).",
     "Tomt: Hemma väljer - en dold mapp .hemma-backups i den första biblioteksmappen på en annan disk än datan, annars "
     "%s/backups. Hemma måste få skriva där (på en Pi: en mapp du gav install-pi.sh).",
     "Vacío: Hemma elige - una carpeta oculta .hemma-backups en la primera carpeta de la biblioteca que esté en otro disco "
     "que los datos, si no %s/backups. Hemma tiene que poder escribir allí (en una Pi: una carpeta que le diste a "
     "install-pi.sh)."},

    /* backups: what the buttons answer */
    {"Hemma is restarting to go back to a backup", "Hemma startar om för att gå tillbaka till en säkerhetskopia",
     "Hemma se está reiniciando para volver a una copia de seguridad"},
    {"Busy with the backups right now - try again in a moment",
     "Upptagen med säkerhetskopiorna just nu - försök igen om en stund",
     "Ocupado con las copias de seguridad ahora mismo - vuelve a intentarlo en un momento"},
    {"That backup isn't there any more", "Den säkerhetskopian finns inte längre", "Esa copia de seguridad ya no está"},
    {"Hemma is already restarting to go back to a backup",
     "Hemma startar redan om för att gå tillbaka till en säkerhetskopia",
     "Hemma ya se está reiniciando para volver a una copia de seguridad"},
    {"Getting it ready - then Hemma restarts", "Gör i ordning den - sedan startar Hemma om",
     "Preparándola - luego Hemma se reinicia"},
    {"Getting it ready…", "Gör i ordning den…", "Preparándola…"},
    {"Too late: Hemma is restarting to put it back", "För sent: Hemma startar om för att återställa den",
     "Demasiado tarde: Hemma se está reiniciando para restaurarla"},
    {"It won't be put back", "Den återställs inte", "No se restaurará"},
    {"Give the whole path, e.g. /mnt/media/hemma-backups", "Ange hela sökvägen, t.ex. /mnt/media/hemma-backups",
     "Escribe la ruta completa, p. ej. /mnt/media/hemma-backups"},
    {"Saved - the next backup goes there", "Sparat - nästa säkerhetskopia hamnar där",
     "Guardado - la próxima copia irá allí"},
    {"Saved - Hemma chooses the folder", "Sparat - Hemma väljer mappen", "Guardado - Hemma elige la carpeta"},

    /* backups: the download (hemma-secret.txt, and when it can't be had) */
    {"SECRET - keep this file like a password.", "HEMLIGT - förvara den här filen som ett lösenord.",
     "SECRETO - guarda este archivo como una contraseña."},
    {"Every password and recovery code in this backup is made with the line above.\n"
     "To put it back on a new memory card or another computer, run hemma --restore with this .tar file\n"
     "(docs/raspberry-pi.md, \"Backups\"). It tells you if the settings file there needs this line.",
     "Varje lösenord och återställningskod i den här säkerhetskopian är gjord med raden ovan.\n"
     "För att lägga tillbaka den på ett nytt minneskort eller en annan dator, kör hemma --restore med den här .tar-filen\n"
     "(docs/raspberry-pi.md, \"Backups\"). Den säger till om inställningsfilen där behöver den här raden.",
     "Cada contraseña y código de recuperación de esta copia se hace con la línea de arriba.\n"
     "Para restaurarla en una tarjeta de memoria u ordenador nuevos, ejecuta hemma --restore con este archivo .tar\n"
     "(docs/raspberry-pi.md, \"Backups\"). Te dice si el archivo de ajustes de allí necesita esta línea."},
    {"the backup can't be read (has the disk gone?)", "säkerhetskopian går inte att läsa (är disken borta?)",
     "no se puede leer la copia de seguridad (¿se ha ido el disco?)"},
    {"Busy - try again in a moment", "Upptaget - försök igen om en stund", "Ocupado - vuelve a intentarlo en un momento"},

    /* updates: the checks */
    {"Update", "Uppdatering", "Actualización"},
    {"Updated to Hemma %s %s.", "Uppdaterad till Hemma %s %s.", "Actualizado a Hemma %s %s."},
    {"Hemma %s didn't start (%s), so the update helper went back to this one and the data as it was before. Nothing was "
     "lost.",
     "Hemma %s startade inte (%s), så hjälpprogrammet för uppdateringar gick tillbaka till den här och datan som den var "
     "innan. Inget gick förlorat.",
     "Hemma %s no arrancó (%s), así que el programa auxiliar de actualizaciones volvió a esta y a los datos como estaban "
     "antes. No se perdió nada."},
    {"See why on the Updates tab - and say so in an issue on GitHub.",
     "Se varför på fliken Uppdateringar - och berätta det i ett ärende på GitHub.",
     "Mira por qué en la pestaña Actualizaciones - y cuéntalo en un issue de GitHub."},
    {"An update wasn't installed (%s): %s.", "En uppdatering installerades inte (%s): %s.",
     "No se instaló una actualización (%s): %s."},
    {"Nothing was changed. The Updates tab says more.", "Inget ändrades. Fliken Uppdateringar säger mer.",
     "No se cambió nada. La pestaña Actualizaciones dice más."},
    {"An update of Hemma failed and going back didn't work at first (%s).",
     "En uppdatering av Hemma misslyckades och det gick inte att gå tillbaka på första försöket (%s).",
     "Una actualización de Hemma falló y volver atrás no funcionó a la primera (%s)."},
    {"Hemma runs again now. See the Updates tab and the log.", "Hemma körs igen nu. Se fliken Uppdateringar och loggen.",
     "Hemma vuelve a funcionar. Mira la pestaña Actualizaciones y el registro."},
    {"The update helper didn't answer.", "Hjälpprogrammet för uppdateringar svarade inte.",
     "El programa auxiliar de actualizaciones no respondió."},
    {"On the Pi: sudo systemctl enable --now hemma-update.path - or run sudo hemma-upgrade there.",
     "På Pi:n: sudo systemctl enable --now hemma-update.path - eller kör sudo hemma-upgrade där.",
     "En la Pi: sudo systemctl enable --now hemma-update.path - o ejecuta allí sudo hemma-upgrade."},
    {"Hemma %s is out (this is %s).", "Hemma %s har kommit (det här är %s).", "Ha salido Hemma %s (esta es la %s)."},
    {"Install it on the Updates tab.", "Installera den på fliken Uppdateringar.", "Instálala en la pestaña Actualizaciones."},
    {"This is the newest Hemma (checked %s).", "Det här är den nyaste Hemma (kontrollerat %s).",
     "Esta es la versión más nueva de Hemma (comprobado %s)."},
    {"Checked once a day.", "Kontrolleras en gång om dagen.", "Se comprueba una vez al día."},
    {"Checked when you ask, on the Updates tab.", "Kontrolleras när du ber om det, på fliken Uppdateringar.",
     "Se comprueba cuando lo pides, en la pestaña Actualizaciones."},
    {"Operating system updates", "Operativsystemets uppdateringar", "Actualizaciones del sistema operativo"},
    {"The operating system's security updates aren't installed by themselves.",
     "Operativsystemets säkerhetsuppdateringar installeras inte av sig själva.",
     "Las actualizaciones de seguridad del sistema operativo no se instalan solas."},
    {"To have them installed every day: sudo apt install unattended-upgrades",
     "För att få dem installerade varje dag: sudo apt install unattended-upgrades",
     "Para que se instalen cada día: sudo apt install unattended-upgrades"},

    /* updates: the tab */
    {"Updates from this page need Hemma installed with its installer (<code>install-pi.sh</code>, from version 0.21.1) "
     "on a Raspberry Pi or another Linux with systemd: then a root helper installs what you choose here.",
     "Uppdateringar från den här sidan kräver att Hemma är installerat med sitt installationsprogram "
     "(<code>install-pi.sh</code>, från version 0.21.1) på en Raspberry Pi eller en annan Linux med systemd: då "
     "installerar ett hjälpprogram med root-behörighet det du väljer här.",
     "Las actualizaciones desde esta página necesitan Hemma instalado con su instalador (<code>install-pi.sh</code>, "
     "desde la versión 0.21.1) en una Raspberry Pi u otro Linux con systemd: entonces un programa auxiliar con permisos "
     "de root instala lo que elijas aquí."},
    {"Here: download <b>%s.tar.gz</b> from <a href=\"%s/latest\" target=\"_blank\" rel=\"noopener noreferrer\">the newest "
     "release</a> and install it the way you installed this one. A Pi or Linux with systemd: unpack it and run "
     "<code>sudo sh install-pi.sh ./%s</code> - from then on, updates come from here or with <code>sudo "
     "hemma-upgrade</code>.",
     "Här: ladda ner <b>%s.tar.gz</b> från <a href=\"%s/latest\" target=\"_blank\" rel=\"noopener noreferrer\">den "
     "nyaste versionen</a> och installera den som du installerade den här. En Pi eller Linux med systemd: packa upp den "
     "och kör <code>sudo sh install-pi.sh ./%s</code> - sedan kommer uppdateringar härifrån eller med <code>sudo "
     "hemma-upgrade</code>.",
     "Aquí: descarga <b>%s.tar.gz</b> de <a href=\"%s/latest\" target=\"_blank\" rel=\"noopener noreferrer\">la versión "
     "más nueva</a> e instálala como instalaste esta. Una Pi o Linux con systemd: descomprímela y ejecuta <code>sudo sh "
     "install-pi.sh ./%s</code> - a partir de entonces, las actualizaciones llegan desde aquí o con <code>sudo "
     "hemma-upgrade</code>."},
    {"Here: build the newest release from its source (<a href=\"%s/latest\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">the newest release</a>, docs/windows.md).",
     "Här: bygg den nyaste versionen från källkoden (<a href=\"%s/latest\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">den nyaste versionen</a>, se docs/windows.md).",
     "Aquí: compila la versión más nueva desde su código fuente (<a href=\"%s/latest\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">la versión más nueva</a>, mira docs/windows.md)."},
    {"<b>Updated to Hemma %s</b> (from %s), %s. <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">What's new</a>",
     "<b>Uppdaterad till Hemma %s</b> (från %s), %s. <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">Vad är nytt</a>",
     "<b>Actualizado a Hemma %s</b> (desde %s), %s. <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">Novedades</a>"},
    {"<b>Updated to Hemma %s</b> (from the one before), %s. <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">What's new</a>",
     "<b>Uppdaterad till Hemma %s</b> (från den förra), %s. <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">Vad är nytt</a>",
     "<b>Actualizado a Hemma %s</b> (desde la anterior), %s. <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">Novedades</a>"},
    {"<b>Hemma %s didn't start</b>, so the update helper went back to this one and the data as it was before (%s) - "
     "nothing was lost.",
     "<b>Hemma %s startade inte</b>, så hjälpprogrammet för uppdateringar gick tillbaka till den här och datan som den "
     "var innan (%s) - inget gick förlorat.",
     "<b>Hemma %s no arrancó</b>, así que el programa auxiliar de actualizaciones volvió a esta y a los datos como "
     "estaban antes (%s) - no se perdió nada."},
    {"<b>Not installed</b> (%s): %s. Nothing was changed.", "<b>Inte installerad</b> (%s): %s. Inget ändrades.",
     "<b>No instalada</b> (%s): %s. No se cambió nada."},
    {"<b>An update failed</b> (%s), and going back didn't work at first: %s. Hemma runs again now - the Log tab may say "
     "why.",
     "<b>En uppdatering misslyckades</b> (%s), och det gick inte att gå tillbaka på första försöket: %s. Hemma körs igen "
     "nu - fliken Logg kan säga varför.",
     "<b>Una actualización falló</b> (%s), y volver atrás no funcionó a la primera: %s. Hemma vuelve a funcionar - la "
     "pestaña Registro puede decir por qué."},
    {"Installing Hemma %s…", "Installerar Hemma %s…", "Instalando Hemma %s…"},
    {"Checking the download you sent…", "Kontrollerar filen du skickade…", "Comprobando la descarga que enviaste…"},
    {"Downloading and checking Hemma %s…", "Laddar ner och kontrollerar Hemma %s…", "Descargando y comprobando Hemma %s…"},
    {"Hemma restarts by itself when it's in; this page comes back on its own.",
     "Hemma startar om av sig själv när den är installerad; sidan kommer tillbaka av sig själv.",
     "Hemma se reinicia solo cuando está instalado; esta página vuelve por sí sola."},
    {"This Hemma", "Den här Hemma", "Este Hemma"},
    {"built from %s", "byggd från %s", "compilado desde %s"},
    {"Releases on GitHub", "Versioner på GitHub", "Versiones en GitHub"},
    {"New versions", "Nya versioner", "Nuevas versiones"},
    {"The update helper didn't answer. On the Pi: <code>sudo systemctl enable --now hemma-update.path</code> - or update "
     "there with <code>sudo hemma-upgrade</code>.",
     "Hjälpprogrammet för uppdateringar svarade inte. På Pi:n: <code>sudo systemctl enable --now "
     "hemma-update.path</code> - eller uppdatera där med <code>sudo hemma-upgrade</code>.",
     "El programa auxiliar de actualizaciones no respondió. En la Pi: <code>sudo systemctl enable --now "
     "hemma-update.path</code> - o actualiza allí con <code>sudo hemma-upgrade</code>."},
    {"Asking github.com…", "Frågar github.com…", "Preguntando a github.com…"},
    {"<b>Hemma %s is out</b> (this is %s). <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener noreferrer\">What's "
     "new</a>",
     "<b>Hemma %s har kommit</b> (det här är %s). <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">Vad är nytt</a>",
     "<b>Ha salido Hemma %s</b> (esta es la %s). <a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener "
     "noreferrer\">Novedades</a>"},
    {"Install Hemma %s? Hemma restarts for a moment. If the new one doesn't start, the update helper goes back to this "
     "one by itself.",
     "Installera Hemma %s? Hemma startar om en kort stund. Om den nya inte startar går hjälpprogrammet för uppdateringar "
     "tillbaka till den här av sig själv.",
     "¿Instalar Hemma %s? Hemma se reinicia un momento. Si la nueva no arranca, el programa auxiliar de actualizaciones "
     "vuelve a esta por sí solo."},
    {"Install Hemma %s", "Installera Hemma %s", "Instalar Hemma %s"},
    {"The check %s didn't work: %s.", "Kontrollen %s fungerade inte: %s.", "La comprobación de %s no funcionó: %s."},
    {"Not checked yet.", "Inte kontrollerat än.", "Todavía no se ha comprobado."},
    {"Check now", "Kontrollera nu", "Comprobar ahora"},
    {"Check once a day", "Kontrollera en gång om dagen", "Comprobar una vez al día"},
    {"Checking asks github.com one thing: which version is newest. Nothing about your home leaves it, and nothing is "
     "installed until you say so. Only what the Hemma project signed is installed; the program and a copy of the data "
     "are kept, and if the new Hemma doesn't start, it goes back to them by itself.",
     "En kontroll frågar github.com en sak: vilken version som är nyast. Inget om ditt hem lämnar det, och inget "
     "installeras förrän du säger till. Bara det som Hemma-projektet har signerat installeras; programmet och en kopia av "
     "datan sparas, och om den nya Hemma inte startar går den tillbaka till dem av sig själv.",
     "Comprobar le pregunta una sola cosa a github.com: qué versión es la más nueva. Nada sobre tu casa sale de ella, y "
     "no se instala nada hasta que lo digas. Solo se instala lo que firmó el proyecto Hemma; se guardan el programa y una "
     "copia de los datos, y si el nuevo Hemma no arranca, vuelve a ellos por sí solo."},
    {"Install from a file", "Installera från en fil", "Instalar desde un archivo"},
    {"No internet here? Download <b>%s</b> from <a href=\"%s\" target=\"_blank\" rel=\"noopener noreferrer\">the releases "
     "page</a> on any computer, then choose it here.",
     "Inget internet här? Ladda ner <b>%s</b> från <a href=\"%s\" target=\"_blank\" rel=\"noopener noreferrer\">sidan "
     "med versioner</a> på vilken dator som helst och välj den sedan här.",
     "¿No hay internet aquí? Descarga <b>%s</b> de <a href=\"%s\" target=\"_blank\" rel=\"noopener noreferrer\">la "
     "página de versiones</a> en cualquier ordenador y luego elígelo aquí."},
    {"No internet here? Download <b>the download for this machine</b> from <a href=\"%s\" target=\"_blank\" "
     "rel=\"noopener noreferrer\">the releases page</a> on any computer, then choose it here.",
     "Inget internet här? Ladda ner <b>filen för den här maskinen</b> från <a href=\"%s\" target=\"_blank\" "
     "rel=\"noopener noreferrer\">sidan med versioner</a> på vilken dator som helst och välj den sedan här.",
     "¿No hay internet aquí? Descarga <b>la descarga para esta máquina</b> de <a href=\"%s\" target=\"_blank\" "
     "rel=\"noopener noreferrer\">la página de versiones</a> en cualquier ordenador y luego elígela aquí."},
    {"Receiving %s… %d%%", "Tar emot %s… %d%%", "Recibiendo %s… %d%%"},
    {"%s is in: the update helper checks it now - only a download the Hemma project signed is installed.",
     "%s har kommit fram: hjälpprogrammet för uppdateringar kontrollerar den nu - bara en fil som Hemma-projektet har "
     "signerat installeras.",
     "%s ha llegado: el programa auxiliar de actualizaciones lo comprueba ahora - solo se instala una descarga firmada "
     "por el proyecto Hemma."},
    {"The download is in: the update helper checks it now - only a download the Hemma project signed is installed.",
     "Filen har kommit fram: hjälpprogrammet för uppdateringar kontrollerar den nu - bara en fil som Hemma-projektet har "
     "signerat installeras.",
     "La descarga ha llegado: el programa auxiliar de actualizaciones la comprueba ahora - solo se instala una descarga "
     "firmada por el proyecto Hemma."},
    {"Choose the download first", "Välj filen först", "Elige primero la descarga"},
    {"The download", "Filen", "La descarga"},
    {"Install", "Installera", "Instalar"},
    {"(Choosing a file here needs JavaScript.)", "(Att välja en fil här kräver JavaScript.)",
     "(Elegir un archivo aquí necesita JavaScript.)"},
    {"The operating system", "Operativsystemet", "El sistema operativo"},
    {"Its security updates install by themselves (last looked for %s).",
     "Dess säkerhetsuppdateringar installeras av sig själva (senast letat efter %s).",
     "Sus actualizaciones de seguridad se instalan solas (última búsqueda: %s)."},
    {"Its security updates install by themselves.", "Dess säkerhetsuppdateringar installeras av sig själva.",
     "Sus actualizaciones de seguridad se instalan solas."},
    {"Its security updates aren't installed by themselves. To have that done every day, run this once on the machine:",
     "Dess säkerhetsuppdateringar installeras inte av sig själva. För att få det gjort varje dag, kör det här en gång på "
     "maskinen:",
     "Sus actualizaciones de seguridad no se instalan solas. Para que se haga cada día, ejecuta esto una vez en la "
     "máquina:"},

    /* updates: what the buttons and the upload answer */
    {"This install has no update helper - the Updates tab says how to update it",
     "Den här installationen har inget hjälpprogram för uppdateringar - fliken Uppdateringar säger hur du uppdaterar den",
     "Esta instalación no tiene programa auxiliar de actualizaciones - la pestaña Actualizaciones dice cómo "
     "actualizarla"},
    {"An update is under way", "En uppdatering pågår", "Hay una actualización en curso"},
    {"Couldn't ask the update helper (see the log)",
     "Det gick inte att fråga hjälpprogrammet för uppdateringar (se loggen)",
     "No se pudo preguntar al programa auxiliar de actualizaciones (mira el registro)"},
    {"Hemma checks for a new version once a day", "Hemma letar efter en ny version en gång om dagen",
     "Hemma busca una versión nueva una vez al día"},
    {"No more daily checks", "Inga fler dagliga kontroller", "Se acabaron las comprobaciones diarias"},
    {"That isn't a newer version - check again", "Det är inte en nyare version - kontrollera igen",
     "Esa no es una versión más nueva - vuelve a comprobar"},
    {"%s started the update to Hemma %s", "%s startade uppdateringen till Hemma %s",
     "%s inició la actualización a Hemma %s"},
    {"%s started an update of Hemma from a download", "%s startade en uppdatering av Hemma från en nedladdad fil",
     "%s inició una actualización de Hemma desde una descarga"},
    {"Downloading Hemma %s…", "Laddar ner Hemma %s…", "Descargando Hemma %s…"},
    {"Choose one file: the download for this machine", "Välj en fil: den för den här maskinen",
     "Elige un archivo: la descarga para esta máquina"},
    {"That isn't a Hemma download - they end in .tar.gz", "Det är ingen fil från Hemma - de slutar på .tar.gz",
     "Eso no es una descarga de Hemma - terminan en .tar.gz"},
    {"That file is far too big for a Hemma download", "Den filen är alldeles för stor för att vara från Hemma",
     "Ese archivo es demasiado grande para ser una descarga de Hemma"},
    {"Someone else is uploading a download right now", "Någon annan laddar upp en fil just nu",
     "Otra persona está subiendo una descarga ahora mismo"},
    {"Couldn't save the file on the server", "Det gick inte att spara filen på servern",
     "No se pudo guardar el archivo en el servidor"},
    {"The server's disk is full", "Serverns disk är full", "El disco del servidor está lleno"},
    {"The connection dropped", "Anslutningen bröts", "Se cortó la conexión"},
    {"Nothing arrived for a minute", "Inget kom fram på en minut", "No llegó nada en un minuto"},
    {"The server couldn't save the file", "Servern kunde inte spara filen", "El servidor no pudo guardar el archivo"},
    {"That isn't a Hemma download (not a .tar.gz)", "Det är ingen fil från Hemma (inte en .tar.gz)",
     "Eso no es una descarga de Hemma (no es un .tar.gz)"},
    {"That upload is over - choose the file again", "Den uppladdningen är slut - välj filen igen",
     "Esa subida ha terminado - elige el archivo otra vez"},
    {"The file changed size - choose it again", "Filen ändrade storlek - välj den igen",
     "El archivo cambió de tamaño - elígelo otra vez"},
    {"Only an admin can install updates", "Bara en administratör kan installera uppdateringar",
     "Solo un administrador puede instalar actualizaciones"},
};
TRANSLATIONS(system, TR)

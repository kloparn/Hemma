/* modules/people's words in Swedish and Spanish (issue #46; core/i18n.h says how). */
#include "../../core/i18n.h"

static const Tr TR[] = {
    /* the section */
    {"Household", "Hushåll", "Hogar"},
    {"Add someone", "Lägg till någon", "Añadir a alguien"},

    /* roles and what they can do (the person dialog's picker, the people list) */
    {"Member", "Medlem", "Miembro"},
    {"Admin", "Administratör", "Administrador"},
    {"Display", "Skärm", "Pantalla"},
    {"Child", "Barn", "Niño"},
    {"Guest", "Gäst", "Invitado"},
    {"use everything", "använder allt", "usa todo"},
    {"also manage people", "hanterar också personer", "también gestiona personas"},
    {"only look (wall tablet, TV)", "tittar bara (väggplatta, TV)", "solo mira (tableta de pared, TV)"},
    {"their own to-dos and the lists", "sina egna uppgifter och listorna", "sus propias tareas y las listas"},
    {"signs in until a day", "loggar in fram till en viss dag", "inicia sesión hasta un día"},

    /* the person dialog */
    {"Your profile", "Din profil", "Tu perfil"},
    {"Edit person", "Ändra person", "Editar persona"},
    {"Avatar", "Ikon", "Icono"},
    {"Initials", "Initialer", "Iniciales"},
    {"Colour", "Färg", "Color"},
    {"Signing in", "Inloggning", "Inicio de sesión"},
    {"Leave the username empty for people who don't need their own sign-in (things can still be booked for them).",
     "Lämna användarnamnet tomt för den som inte behöver en egen inloggning (saker kan ändå bokas åt hen).",
     "Deja el nombre de usuario vacío para quien no necesite su propio acceso (igualmente se pueden reservar cosas "
     "para esa persona)."},
    {"Username", "Användarnamn", "Nombre de usuario"},
    {"e.g. eva", "t.ex. eva", "p. ej. eva"},
    {"Can", "Kan", "Puede"},
    {"A guest's last day", "Gästens sista dag", "Último día del invitado"},
    {"Their sign-in ends after it, by itself. Change it to give them more days.",
     "Inloggningen upphör av sig själv efter den dagen. Ändra den för att ge fler dagar.",
     "Su acceso termina solo después de ese día. Cámbialo para darle más días."},
    {"Change your own password under <b>Your account</b>.", "Byt ditt eget lösenord under <b>Ditt konto</b>.",
     "Cambia tu propia contraseña en <b>Tu cuenta</b>."},
    {"New password", "Nytt lösenord", "Nueva contraseña"},
    {"Password", "Lösenord", "Contraseña"},
    {"Leave empty to keep the current one", "Lämna tomt för att behålla det nuvarande",
     "Déjalo vacío para mantener la actual"},
    {"At least 8 characters", "Minst 8 tecken", "Al menos 8 caracteres"},
    {"Sign %s out everywhere", "Logga ut %s överallt", "Cerrar la sesión de %s en todas partes"},
    {"Remove %s", "Ta bort %s", "Quitar %s"},

    /* removing someone (issue #45) */
    {"Erase %s", "Radera %s", "Borrar a %s"},
    {"Nothing else of theirs is waiting", "Inget annat av hens väntar", "No queda nada más suyo pendiente"},
    {"%s stops working where it's signed in, and its settings go.",
     "%s slutar fungera där den är inloggad, och dess inställningar försvinner.",
     "%s deja de funcionar donde tiene la sesión iniciada, y sus ajustes se borran."},
    {"Everything about %s goes: what they booked, their to-dos and notes, what they watched, their lines in Latest. "
     "Things they added for everyone stay, without their name.",
     "Allt om %s försvinner: det hen bokat, hens uppgifter och lappar, det hen tittat på, hens rader i Senaste. Det "
     "hen lagt till för alla blir kvar, utan hens namn.",
     "Todo lo de %s se borra: lo que reservó, sus tareas y notas, lo que vio, sus líneas en Novedades. Lo que añadió "
     "para todos se queda, sin su nombre."},
    {"How %s leaves", "Hur %s lämnar hushållet", "Cómo se va %s"},
    {"%s moved out - keep their history", "%s har flyttat - behåll hens historik",
     "%s se ha mudado - conservar su historial"},
    {"What they did stays under their name, “%s (moved out)”: bookings that happened, notes, Latest, what they "
     "watched. What's still ahead of them:",
     "Det hen gjort blir kvar under hens namn, “%s (flyttat)”: bokningar som blivit av, lappar, Senaste, det hen "
     "tittat på. Det som fortfarande ligger framför hen:",
     "Lo que hizo se queda con su nombre, “%s (se ha mudado)”: las reservas que ya pasaron, notas, Novedades, lo que "
     "vio. Lo que aún tiene por delante:"},
    {"Erase everything about %s", "Radera allt om %s", "Borrar todo lo de %s"},
    {"Their bookings, to-dos, notes, watch history and lines in Latest go too. Things they added for everyone stay, "
     "without their name:",
     "Hens bokningar, uppgifter, lappar, tittarhistorik och rader i Senaste försvinner också. Det hen lagt till för "
     "alla blir kvar, utan hens namn:",
     "También se borran sus reservas, tareas, notas, historial de lo visto y líneas en Novedades. Lo que añadió para "
     "todos se queda, sin su nombre:"},

    /* the people list */
    {"People", "Personer", "Personas"},
    {"(you)", "(du)", "(tú)"},
    {"Guest until %s", "Gäst t.o.m. %s", "Invitado, último día: %s"},
    {"Guest, ended %s", "Gäst, slutade %s", "Invitado, terminó %s"},
    {"no sign-in", "ingen inloggning", "sin acceso"},
    {"home", "hemma", "en casa"},
    {"online on %d screen", "online på %d skärm", "conectado en %d pantalla"},
    {"online on %d screens", "online på %d skärmar", "conectado en %d pantallas"},
    {"signed in %s", "loggade in %s", "inició sesión %s"},
    {"Edit %s", "Ändra %s", "Editar %s"},
    {"Moved out", "Har flyttat", "Se han mudado"},
    {"moved out %s", "flyttade %s", "se mudó %s"},
    {"%s is back in the household", "%s är tillbaka i hushållet", "%s ha vuelto al hogar"},
    {"Back in the household", "Tillbaka i hushållet", "De vuelta en el hogar"},
    {"Erase", "Radera", "Borrar"},

    /* the home */
    {"Your home", "Ditt hem", "Tu casa"},
    {"Home name", "Hemmets namn", "Nombre de la casa"},
    {"Rename", "Byt namn", "Renombrar"},
    {"Wall display", "Väggskärm", "Pantalla de pared"},
    {"An always-on overview for a tablet in the hallway or kitchen. Tip: give the tablet its own <i>Display</i> "
     "sign-in so it can only look, and pick its cards under <a href=\"/screens\">Screens</a>.",
     "En översikt som alltid är på, för en surfplatta i hallen eller köket. Tips: ge surfplattan en egen "
     "<i>Skärm</i>-inloggning så att den bara kan titta, och välj dess kort under <a href=\"/screens\">Skärmar</a>.",
     "Un resumen siempre encendido para una tableta en el recibidor o la cocina. Consejo: dale a la tableta su propio "
     "acceso de <i>Pantalla</i> para que solo pueda mirar, y elige sus tarjetas en <a href=\"/screens\">Pantallas</a>."},
    {"Open wall display", "Öppna väggskärmen", "Abrir la pantalla de pared"},

    /* your account */
    {"Your account", "Ditt konto", "Tu cuenta"},
    {"Signed in as @%s", "Inloggad som @%s", "Sesión iniciada como @%s"},
    {"a guest until %s", "gäst t.o.m. %s", "invitado, último día: %s"},
    {"You signed in with a temporary password. Choose your own now.",
     "Du loggade in med ett tillfälligt lösenord. Välj ett eget nu.",
     "Iniciaste sesión con una contraseña temporal. Elige la tuya ahora."},
    {"Change password", "Byt lösenord", "Cambiar contraseña"},
    {"Temporary password", "Tillfälligt lösenord", "Contraseña temporal"},
    {"Current password", "Nuvarande lösenord", "Contraseña actual"},
    {"Again", "Igen", "Otra vez"},
    {"Signed in on", "Inloggad på", "Sesión iniciada en"},
    {"A browser", "En webbläsare", "Un navegador"},
    {"this device", "den här enheten", "este dispositivo"},
    {"active %s", "aktiv %s", "activo %s"},
    {"Sign out other devices", "Logga ut andra enheter", "Cerrar sesión en otros dispositivos"},
    {"Sign out", "Logga ut", "Cerrar sesión"},
    {"Your new password is set and you're signed in. You have %d recovery codes left.",
     "Ditt nya lösenord är satt och du är inloggad. Du har %d återställningskoder kvar.",
     "Tu contraseña nueva está puesta y has iniciado sesión. Te quedan %d códigos de recuperación."},
    {"Your new password is set and you're signed in. You have %d recovery code left - make new ones under Recovery "
     "codes.",
     "Ditt nya lösenord är satt och du är inloggad. Du har %d återställningskod kvar - skapa nya under "
     "Återställningskoder.",
     "Tu contraseña nueva está puesta y has iniciado sesión. Te queda %d código de recuperación - crea nuevos en "
     "Códigos de recuperación."},
    {"Your new password is set and you're signed in. You have %d recovery codes left - make new ones under Recovery "
     "codes.",
     "Ditt nya lösenord är satt och du är inloggad. Du har %d återställningskoder kvar - skapa nya under "
     "Återställningskoder.",
     "Tu contraseña nueva está puesta y has iniciado sesión. Te quedan %d códigos de recuperación - crea nuevos en "
     "Códigos de recuperación."},

    /* recovery codes */
    {"Recovery codes", "Återställningskoder", "Códigos de recuperación"},
    {"Your new codes are waiting to be saved. <a href=\"/people/codes\">Show them</a>",
     "Dina nya koder väntar på att sparas. <a href=\"/people/codes\">Visa dem</a>",
     "Tus códigos nuevos esperan a que los guardes. <a href=\"/people/codes\">Muéstralos</a>"},
    {"%d of %d left. Each one gets you back in once if you forget your password.",
     "%d av %d kvar. Var och en tar dig in igen en gång om du glömmer ditt lösenord.",
     "Quedan %d de %d. Cada uno te deja volver a entrar una vez si olvidas tu contraseña."},
    {"None yet. Make some: they get you back in if you ever forget your password.",
     "Inga än. Skapa några: de tar dig in igen om du någon gång glömmer ditt lösenord.",
     "Ninguno todavía. Crea algunos: te dejan volver a entrar si alguna vez olvidas tu contraseña."},
    {"Make new codes", "Skapa nya koder", "Crear códigos nuevos"},
    {"Make recovery codes", "Skapa återställningskoder", "Crear códigos de recuperación"},
    {"Your password", "Ditt lösenord", "Tu contraseña"},
    {"Your old codes stop working.", "Dina gamla koder slutar fungera.", "Tus códigos antiguos dejarán de funcionar."},
    {"Make %d new codes", "Skapa %d nya koder", "Crear %d códigos nuevos"},
    {"Codes are only shown right after they're made. You have %d left; you can make new ones under Household.",
     "Koderna visas bara direkt efter att de skapats. Du har %d kvar; du kan skapa nya under Hushåll.",
     "Los códigos solo se muestran justo después de crearlos. Códigos que te quedan: %d; puedes crear nuevos en "
     "Hogar."},
    {"Back to Household", "Tillbaka till Hushåll", "Volver a Hogar"},
    {"Your recovery codes", "Dina återställningskoder", "Tus códigos de recuperación"},
    {"<b>Save these now</b> - this is the only time Hemma shows them.",
     "<b>Spara dem nu</b> - det här är enda gången Hemma visar dem.",
     "<b>Guárdalos ahora</b> - es la única vez que Hemma los muestra."},
    {"Forgot your password? Choose <b>Forgot your password?</b> on the sign-in page and enter your username "
     "(<b>%s</b>) and one of these codes. Each code works once.",
     "Glömt ditt lösenord? Välj <b>Glömt ditt lösenord?</b> på inloggningssidan och skriv ditt användarnamn (<b>%s</b>) "
     "och en av de här koderna. Varje kod fungerar en gång.",
     "¿Olvidaste tu contraseña? Elige <b>¿Olvidaste tu contraseña?</b> en la página de inicio de sesión y escribe tu "
     "nombre de usuario (<b>%s</b>) y uno de estos códigos. Cada código sirve una vez."},
    {"Keep them somewhere safe: in a password manager, or printed and put away. Anyone with one of them and your "
     "username can choose a new password for you.",
     "Förvara dem säkert: i en lösenordshanterare, eller utskrivna och undanlagda. Den som har en av dem och ditt "
     "användarnamn kan välja ett nytt lösenord åt dig.",
     "Guárdalos en un lugar seguro: en un gestor de contraseñas, o impresos y bien guardados. Cualquiera con uno de "
     "ellos y tu nombre de usuario puede elegir una contraseña nueva por ti."},
    {"Download as a text file", "Ladda ner som textfil", "Descargar como archivo de texto"},
    {"I've saved them", "Jag har sparat dem", "Ya los he guardado"},
    /* (the file /people/codes.txt) */
    {"Recovery codes are only available right after they're made.\n",
     "Återställningskoder finns bara direkt efter att de skapats.\n",
     "Los códigos de recuperación solo están disponibles justo después de crearlos.\n"},
    {"%s - recovery codes for %s (username: %s), made %s", "%s - återställningskoder för %s (användarnamn: %s), skapade %s",
     "%s - códigos de recuperación de %s (nombre de usuario: %s), creados el %s"},
    {"Forgot your password? On the sign-in page choose \"Forgot your password?\" and enter your\nusername and one of "
     "these codes. Each code works once. Keep this file somewhere safe.\n",
     "Glömt ditt lösenord? Välj \"Glömt ditt lösenord?\" på inloggningssidan och skriv ditt\nanvändarnamn och en av de här "
     "koderna. Varje kod fungerar en gång. Förvara filen säkert.\n",
     "¿Olvidaste tu contraseña? En la página de inicio de sesión elige \"¿Olvidaste tu contraseña?\" y escribe tu\n"
     "nombre de usuario y uno de estos códigos. Cada código sirve una vez. Guarda este archivo en un lugar seguro.\n"},

    /* your calendar on your phone (issue #41) */
    {"Your calendar on your phone", "Din kalender i mobilen", "Tu calendario en el móvil"},
    {"Your bookings, your to-dos and the household's events in your phone's own calendar. It keeps a copy for when "
     "you're out, and its reminders work as usual. Hemma isn't on the internet, so the phone brings it up to date "
     "while it's at home.",
     "Dina bokningar, dina uppgifter och hushållets händelser i mobilens egen kalender. Den sparar en kopia för när du "
     "är ute, och påminnelserna fungerar som vanligt. Hemma finns inte på internet, så mobilen uppdaterar den medan "
     "den är hemma.",
     "Tus reservas, tus tareas y los eventos del hogar en el propio calendario de tu móvil. Guarda una copia para "
     "cuando estés fuera, y sus recordatorios funcionan como siempre. Hemma no está en internet, así que el móvil lo "
     "pone al día mientras está en casa."},
    {"Make my calendar link", "Skapa min kalenderlänk", "Crear mi enlace de calendario"},
    {"Open this page at Hemma's address to see your link.", "Öppna den här sidan på Hemmas adress för att se din länk.",
     "Abre esta página en la dirección de Hemma para ver tu enlace."},
    {"You opened Hemma at <b>%s</b>, which only this computer can reach: this link works for a calendar on this "
     "computer. For your phone, open this page on the phone, or at Hemma's address at home.",
     "Du öppnade Hemma på <b>%s</b>, som bara den här datorn når: länken fungerar för en kalender på den här datorn. "
     "För mobilen, öppna den här sidan i mobilen, eller på Hemmas adress hemma.",
     "Abriste Hemma en <b>%s</b>, a la que solo llega este ordenador: este enlace sirve para un calendario en este "
     "ordenador. Para tu móvil, abre esta página en el móvil, o en la dirección de Hemma en casa."},
    {"Add to this device's calendar", "Lägg till i den här enhetens kalender", "Añadir al calendario de este dispositivo"},
    {"A code with your calendar link, for a phone's camera", "En kod med din kalenderlänk, för en mobilkamera",
     "Un código con tu enlace de calendario, para la cámara de un móvil"},
    {"<b>iPhone:</b> tap the button on the phone, and choose Subscribe. <b>Android:</b> install ICSx⁵ (free, from "
     "Google Play or F-Droid) and add this address in it. A computer's calendar subscribes to it too:",
     "<b>iPhone:</b> tryck på knappen i mobilen och välj Prenumerera. <b>Android:</b> installera ICSx⁵ (gratis, från "
     "Google Play eller F-Droid) och lägg till den här adressen i den. En dators kalender kan också prenumerera på den:",
     "<b>iPhone:</b> toca el botón en el móvil y elige Suscribirse. <b>Android:</b> instala ICSx⁵ (gratis, desde "
     "Google Play o F-Droid) y añade esta dirección en ella. El calendario de un ordenador también puede suscribirse:"},
    {"<b>iPhone:</b> tap the button on the phone, or scan the code with its camera, and choose Subscribe. "
     "<b>Android:</b> install ICSx⁵ (free, from Google Play or F-Droid) and add this address in it. A computer's "
     "calendar subscribes to it too:",
     "<b>iPhone:</b> tryck på knappen i mobilen, eller skanna koden med kameran, och välj Prenumerera. "
     "<b>Android:</b> installera ICSx⁵ (gratis, från Google Play eller F-Droid) och lägg till den här adressen i den. "
     "En dators kalender kan också prenumerera på den:",
     "<b>iPhone:</b> toca el botón en el móvil, o escanea el código con su cámara, y elige Suscribirse. "
     "<b>Android:</b> instala ICSx⁵ (gratis, desde Google Play o F-Droid) y añade esta dirección en ella. El "
     "calendario de un ordenador también puede suscribirse:"},
    {"Every booking in the household, not only yours", "Alla bokningar i hushållet, inte bara dina",
     "Todas las reservas del hogar, no solo las tuyas"},
    {"Anyone with the link can read this calendar: keep it to your own phone and computer. A new link stops the old "
     "one.",
     "Alla som har länken kan läsa kalendern: håll den till din egen mobil och dator. En ny länk stoppar den gamla.",
     "Cualquiera con el enlace puede leer este calendario: tenlo solo en tu móvil y tu ordenador. Un enlace nuevo "
     "anula el antiguo."},
    {"Make a new link? Phones with the old one stop getting updates.",
     "Skapa en ny länk? Mobiler med den gamla slutar få uppdateringar.",
     "¿Crear un enlace nuevo? Los móviles con el antiguo dejarán de recibir actualizaciones."},
    {"Make a new link", "Skapa en ny länk", "Crear un enlace nuevo"},
    {"Turn it off", "Stäng av den", "Desactivarlo"},

    /* language (issue #46: "Language", "Automatic" and the languages' own names are the core's) */
    {"An admin picks this screen's language, under Household.",
     "En administratör väljer den här skärmens språk, under Hushåll.",
     "Un administrador elige el idioma de esta pantalla, en Hogar."},
    {"(this browser: %s)", "(den här webbläsaren: %s)", "(este navegador: %s)"},
    {"(this browser asks for none of these, so English is used)",
     "(den här webbläsaren ber inte om något av dessa, så engelska används)",
     "(este navegador no pide ninguno de estos, así que se usa el inglés)"},
    {"Pick a language from the list", "Välj ett språk i listan", "Elige un idioma de la lista"},

    /* privacy (issue #42) */
    {"Privacy", "Integritet", "Privacidad"},
    {"What the rest of the household sees of you.", "Det resten av hushållet ser av dig.",
     "Lo que el resto del hogar ve de ti."},
    {"On the wall tablet", "På väggplattan", "En la tableta de pared"},
    {"With home or away", "Med hemma eller borta", "Con si estoy en casa o fuera"},
    {"Just my name", "Bara mitt namn", "Solo mi nombre"},
    {"Not at all", "Inte alls", "Nada"},
    {"Others see when I have Hemma open, and which part", "Andra ser när jag har Hemma öppet, och vilken del",
     "Los demás ven cuándo tengo Hemma abierto, y qué parte"},
    {"On the TV's <i>Who's watching?</i>", "På TV:ns <i>Vem tittar?</i>", "En el <i>¿Quién está viendo?</i> de la tele"},
    {"My phone and computer may tell Hemma when I'm home", "Min mobil och dator får berätta för Hemma när jag är hemma",
     "Mi móvil y mi ordenador pueden decirle a Hemma cuándo estoy en casa"},
    {"Whoever picks you on the TV sees and can clear what you're watching: a PIN for it is under "
     "<a href=\"/screens\">Screens</a>.",
     "Den som väljer dig på TV:n ser och kan rensa det du tittar på: en PIN-kod för det finns under "
     "<a href=\"/screens\">Skärmar</a>.",
     "Quien te elija en la tele ve y puede borrar lo que estás viendo: hay un PIN para eso en "
     "<a href=\"/screens\">Pantallas</a>."},
    {"Who's home comes from the devices an admin marked as yours, on the <a href=\"/net\">Network</a> page.",
     "Vem som är hemma kommer från enheterna en administratör har markerat som dina, på sidan "
     "<a href=\"/net\">Nätverk</a>.",
     "Quién está en casa sale de los dispositivos que un administrador marcó como tuyos, en la página "
     "<a href=\"/net\">Red</a>."},

    /* saving people */
    {"Keep at least one admin who can sign in", "Behåll minst en administratör som kan logga in",
     "Mantén al menos un administrador que pueda iniciar sesión"},
    {"Unknown person", "Okänd person", "Persona desconocida"},
    {"Only an admin can change other people", "Bara en administratör kan ändra andra personer",
     "Solo un administrador puede cambiar a otras personas"},
    {"Only an admin can add people", "Bara en administratör kan lägga till personer",
     "Solo un administrador puede añadir personas"},
    {"A name, please", "Ett namn, tack", "Un nombre, por favor"},
    {"There's already a %s here", "Det finns redan någon som heter %s här", "Ya hay alguien que se llama %s aquí"},
    {"Change your own password under Your account - it asks for the current one first",
     "Byt ditt eget lösenord under Ditt konto - där frågar det efter det nuvarande först",
     "Cambia tu propia contraseña en Tu cuenta - allí te pide primero la actual"},
    {"Usernames are 2–32 of a–z, 0–9 and . _ -", "Användarnamn är 2–32 tecken av a–z, 0–9 och . _ -",
     "Los nombres de usuario tienen 2–32 caracteres de a–z, 0–9 y . _ -"},
    {"@%s is taken", "@%s är upptaget", "@%s ya está en uso"},
    {"Give %s a password to sign in with", "Ge %s ett lösenord att logga in med",
     "Dale a %s una contraseña para iniciar sesión"},
    {"A password needs a username too", "Ett lösenord behöver ett användarnamn också",
     "Una contraseña también necesita un nombre de usuario"},
    /* (core/auth.c's auth_password_problem, said here) */
    {"Use at least 8 characters", "Använd minst 8 tecken", "Usa al menos 8 caracteres"},
    {"That's a bit long — 200 characters at most", "Det är lite för långt — högst 200 tecken",
     "Es un poco largo — 200 caracteres como máximo"},
    {"Passwords can't be set: Hemma has no password secret yet (System says how to add one)",
     "Lösenord kan inte sättas: Hemma har ingen lösenordshemlighet än (System visar hur du lägger till en)",
     "No se pueden poner contraseñas: Hemma aún no tiene un secreto de contraseñas (Sistema explica cómo añadirlo)"},
    {"Pick the guest's last day", "Välj gästens sista dag", "Elige el último día del invitado"},
    {"Pick a day from today on", "Välj en dag från och med idag", "Elige un día a partir de hoy"},
    {"Added %s", "Lade till %s", "Añadido: %s"},
    {"%s joined the household", "%s gick med i hushållet", "%s se unió al hogar"},
    {"Only an admin can remove people", "Bara en administratör kan ta bort personer",
     "Solo un administrador puede quitar personas"},
    {"Can't remove that person", "Den personen går inte att ta bort", "No se puede quitar a esa persona"},
    {"%s moved out", "%s har flyttat", "%s se ha mudado"},
    {"%s moved out - their history stays", "%s har flyttat - hens historik blir kvar",
     "%s se ha mudado - su historial se queda"},
    {"Removed %s and everything about them", "Tog bort %s och allt om hen", "Se ha borrado a %s y todo lo suyo"},
    {"Only an admin can do that", "Bara en administratör kan göra det", "Solo un administrador puede hacer eso"},
    {"They're not among those who moved out", "Hen är inte bland dem som har flyttat",
     "No está entre quienes se han mudado"},
    {"%s is back - give them a sign-in under Edit if they need one",
     "%s är tillbaka - ge hen en inloggning under Ändra om hen behöver en",
     "%s ha vuelto - dale un acceso en Editar si lo necesita"},
    {"Only an admin can rename the home", "Bara en administratör kan byta namn på hemmet",
     "Solo un administrador puede cambiar el nombre de la casa"},

    /* passwords, codes, devices */
    {"Too many tries. Wait %d minute and try again.", "För många försök. Vänta %d minut och försök igen.",
     "Demasiados intentos. Espera %d minuto y vuelve a intentarlo."},
    {"Too many tries. Wait %d minutes and try again.", "För många försök. Vänta %d minuter och försök igen.",
     "Demasiados intentos. Espera %d minutos y vuelve a intentarlo."},
    {"Your current password isn't right", "Ditt nuvarande lösenord stämmer inte", "Tu contraseña actual no es correcta"},
    {"The new passwords don't match", "De nya lösenorden stämmer inte överens", "Las contraseñas nuevas no coinciden"},
    {"Password changed — signed out %d other device", "Lösenordet är bytt — loggade ut %d annan enhet",
     "Contraseña cambiada — se cerró la sesión en %d dispositivo más"},
    {"Password changed — signed out %d other devices", "Lösenordet är bytt — loggade ut %d andra enheter",
     "Contraseña cambiada — se cerró la sesión en %d dispositivos más"},
    {"Password changed", "Lösenordet är bytt", "Contraseña cambiada"},
    {"Your password isn't right", "Ditt lösenord stämmer inte", "Tu contraseña no es correcta"},
    {"This account can't have recovery codes", "Det här kontot kan inte ha återställningskoder",
     "Esta cuenta no puede tener códigos de recuperación"},
    {"Recovery codes saved - keep them safe", "Återställningskoderna är sparade - förvara dem säkert",
     "Códigos de recuperación guardados - guárdalos en un lugar seguro"},
    {"Signed out %d other device", "Loggade ut %d annan enhet", "Se cerró la sesión en %d dispositivo más"},
    {"Signed out %d other devices", "Loggade ut %d andra enheter", "Se cerró la sesión en %d dispositivos más"},
    {"Signed %s out of %d device", "Loggade ut %s från %d enhet", "Se cerró la sesión de %s en %d dispositivo"},
    {"Signed %s out of %d devices", "Loggade ut %s från %d enheter", "Se cerró la sesión de %s en %d dispositivos"},

    /* the calendar link's toasts */
    {"A calendar link is for people who sign in", "En kalenderlänk är för den som loggar in",
     "Un enlace de calendario es para quien inicia sesión"},
    {"Your calendar link is ready", "Din kalenderlänk är klar", "Tu enlace de calendario está listo"},
    {"A new calendar link - the old one stopped working", "En ny kalenderlänk - den gamla slutade fungera",
     "Un enlace de calendario nuevo - el antiguo dejó de funcionar"},
    {"Turned off - phones that used it stop getting updates",
     "Avstängd - mobiler som använde den får inga fler uppdateringar",
     "Desactivado - los móviles que lo usaban dejan de recibir actualizaciones"},
    {"Every booking goes in your calendar", "Alla bokningar hamnar i din kalender", "Todas las reservas van a tu calendario"},
    {"Only your own bookings go in your calendar", "Bara dina egna bokningar hamnar i din kalender",
     "Solo tus propias reservas van a tu calendario"},

    /* the Android app (2.0) */
    {"Hemma on your phone", "Hemma i mobilen", "Hemma en tu móvil"},
    /* what the app does (2.3) */
    {"Hemma has an app for Android phones. Besides Hemma itself, it:",
     "Hemma finns som app för Android-mobiler. Förutom Hemma självt:", "Hemma tiene una app para móviles Android. Además de Hemma, la app:"},
    {"Finds Hemma at home by itself - no address to type.", "Hittar Hemma hemma av sig själv - ingen adress att skriva.",
     "Encuentra Hemma en casa por sí sola, sin escribir ninguna dirección."},
    {"Keeps you signed in, and opens straight to Hemma.", "Håller dig inloggad och öppnar direkt till Hemma.",
     "Mantiene tu sesión iniciada y abre Hemma directamente."},
    {"Plays audiobooks with the phone locked, with the book and the chapter on the lock screen.",
     "Spelar ljudböcker med mobilen låst, med boken och kapitlet på låsskärmen.",
     "Reproduce audiolibros con el móvil bloqueado, con el libro y el capítulo en la pantalla de bloqueo."},
    {"Saves videos and audiobooks for offline: on a trip, on the train, wherever there's no Hemma.",
     "Sparar videor och ljudböcker offline: på resan, på tåget, där det inte finns något Hemma.",
     "Guarda vídeos y audiolibros sin conexión: de viaje, en el tren, donde no haya Hemma."},
    {"Talks only to Hemma on your home network - nothing else, nowhere else.",
     "Pratar bara med Hemma i ditt hemnätverk - inget annat, ingen annanstans.",
     "Solo habla con Hemma en la red de tu casa: con nada más, en ningún otro sitio."},
    {"The Hemma app", "Hemma-appen", "La app de Hemma"},
    {"You're using Hemma's app for Android. Besides Hemma itself, it:", "Du använder Hemmas app för Android. Förutom Hemma självt:",
     "Estás usando la app de Hemma para Android. Además de Hemma, la app:"},
    {"Saved for offline", "Sparat offline", "Guardado sin conexión"},
    {"Save something with Save for offline on its page in Watch. A new Hemma brings a new app: Get the app for Android "
     "in a phone's browser installs it over this one.",
     "Spara något med Spara offline på dess sida i Titta. Ett nytt Hemma har med sig en ny app: Hämta appen för Android i "
     "mobilens webbläsare installerar den över den här.",
     "Guarda algo con Guardar sin conexión en su página de Ver. Un Hemma nuevo trae una app nueva: Descargar la app para "
     "Android en el navegador del móvil la instala sobre esta."},
    {"Get the app for Android", "Hämta appen för Android", "Descargar la app para Android"},
    /* Home's card for a browser on Android (2.1) */
    {"Hemma has an app for your phone", "Hemma finns som app för din mobil", "Hemma tiene una app para tu móvil"},
    {"Open the app instead!", "Öppna appen i stället!", "¡Abre la app en su lugar!"},
    {"You're using Hemma in the browser, and you've signed in from the Hemma app before. The app keeps you signed in "
     "and plays audiobooks with the phone locked.",
     "Du använder Hemma i webbläsaren, och du har loggat in från Hemma-appen förut. Appen håller dig inloggad och "
     "spelar ljudböcker med mobilen låst.",
     "Estás usando Hemma en el navegador, y ya has iniciado sesión desde la app de Hemma. La app mantiene tu sesión "
     "y reproduce audiolibros con el móvil bloqueado."},
    {"Open the app", "Öppna appen", "Abrir la app"},
    {"Not on this phone, or an app from before Hemma 2.2? The button gets the app instead: open the file to install it.",
     "Inte på den här mobilen, eller en app från före Hemma 2.2? Knappen hämtar appen i stället: öppna filen för att "
     "installera den.",
     "¿No está en este móvil, o es una app anterior a Hemma 2.2? El botón descarga la app: abre el archivo para "
     "instalarla."},
    {"You're using Hemma in the browser. The app finds Hemma at home by itself, keeps you signed in and plays "
     "audiobooks with the phone locked.",
     "Du använder Hemma i webbläsaren. Appen hittar Hemma hemma av sig själv, håller dig inloggad och spelar "
     "ljudböcker med mobilen låst.",
     "Estás usando Hemma en el navegador. La app encuentra Hemma en casa por sí sola, mantiene tu sesión iniciada y "
     "reproduce audiolibros con el móvil bloqueado."},
    {"Get the app", "Hämta appen", "Descargar la app"},
    {"Not now", "Inte nu", "Ahora no"},
    {"The app stays under Household → Your account.", "Appen finns kvar under Hushåll → Ditt konto.",
     "La app sigue en Hogar → Tu cuenta."},
    {"Open the file once it's downloaded. Android asks whether your browser may install apps: allow it, this once. "
     "The app only ever talks to Hemma on your home network.",
     "Öppna filen när den har laddats ner. Android frågar om webbläsaren får installera appar: tillåt det, den här "
     "gången. Appen pratar bara med Hemma i ditt hemnätverk.",
     "Abre el archivo cuando se haya descargado. Android pregunta si tu navegador puede instalar apps: permítelo, esta "
     "vez. La app solo habla con Hemma en tu red de casa."},
    {"On an iPhone: open Hemma in Safari, tap Share, then Add to Home Screen.",
     "På en iPhone: öppna Hemma i Safari, tryck på Dela och sedan Lägg till på hemskärmen.",
     "En un iPhone: abre Hemma en Safari, toca Compartir y luego Añadir a pantalla de inicio."},
};
TRANSLATIONS(people, TR)

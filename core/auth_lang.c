/* core/auth.c's words in Swedish and Spanish (issue #46; core/i18n.h says how): the signed-out pages -
   welcome (the first admin), sign-in, forgot password, "Nobody can sign in", the damaged-data page.
   The docs are English: their headings (Locked out, A damaged database) and file names stay as they are. */
#include "i18n.h"

static const Tr TR[] = {
    /* passwords (people's forms show these too) */
    {"Use at least 8 characters", "Använd minst 8 tecken", "Usa al menos 8 caracteres"},
    {"That's a bit long — 200 characters at most", "Det är lite för långt — högst 200 tecken",
     "Es un poco largo — 200 caracteres como máximo"},
    {"Again", "Igen", "Otra vez"},
    {"Keep me signed in on this device", "Håll mig inloggad på den här enheten",
     "Mantener la sesión iniciada en este dispositivo"},

    /* waiting after too many tries */
    {"Lots of sign-in tries right now. Wait %L second and try again.",
     "Många inloggningsförsök just nu. Vänta %L sekund och försök igen.",
     "Hay muchos intentos de iniciar sesión ahora mismo. Espera %L segundo y vuelve a intentarlo."},
    {"Lots of sign-in tries right now. Wait %L seconds and try again.",
     "Många inloggningsförsök just nu. Vänta %L sekunder och försök igen.",
     "Hay muchos intentos de iniciar sesión ahora mismo. Espera %L segundos y vuelve a intentarlo."},
    {"Too many tries. Wait %L minute and try again.", "För många försök. Vänta %L minut och försök igen.",
     "Demasiados intentos. Espera %L minuto y vuelve a intentarlo."},
    {"Too many tries. Wait %L minutes and try again.", "För många försök. Vänta %L minuter och försök igen.",
     "Demasiados intentos. Espera %L minutos y vuelve a intentarlo."},

    /* no secret key yet */
    {"Almost there", "Nästan klart", "Ya casi está"},
    {"%s needs a secret key before anyone can sign in.", "%s behöver en hemlig nyckel innan någon kan logga in.",
     "%s necesita una clave secreta antes de que nadie pueda iniciar sesión."},
    {"Make one: run <code>hemma --gen-salt</code>.", "Skapa en: kör <code>hemma --gen-salt</code>.",
     "Crea una: ejecuta <code>hemma --gen-salt</code>."},
    {"Put it in Hemma's settings file as <code>HEMMA_AUTH_SALT=...</code>: <code>.env</code> next to Hemma, or "
     "<code>/etc/hemma/hemma.env</code> on a Pi set up with install-pi.sh (see <code>.env.example</code>).",
     "Lägg den i Hemmas inställningsfil som <code>HEMMA_AUTH_SALT=...</code>: <code>.env</code> bredvid Hemma, eller "
     "<code>/etc/hemma/hemma.env</code> på en Pi som installerats med install-pi.sh (se <code>.env.example</code>).",
     "Ponla en el archivo de ajustes de Hemma como <code>HEMMA_AUTH_SALT=...</code>: <code>.env</code> junto a Hemma, "
     "o <code>/etc/hemma/hemma.env</code> en una Pi instalada con install-pi.sh (mira <code>.env.example</code>)."},
    {"Restart Hemma and open this page again to create the admin account.",
     "Starta om Hemma och öppna den här sidan igen för att skapa administratörskontot.",
     "Reinicia Hemma y vuelve a abrir esta página para crear la cuenta de administrador."},

    /* nobody can sign in */
    {"Sign in", "Logga in", "Iniciar sesión"},
    {"Nobody can sign in", "Ingen kan logga in", "Nadie puede iniciar sesión"},
    {"%s has no account that can sign in right now.", "%s har inget konto som kan logga in just nu.",
     "%s no tiene ninguna cuenta que pueda iniciar sesión ahora mismo."},
    {"Whoever runs the Hemma server can bring the admin back: see <b>Locked out</b> in Hemma's account docs "
     "(<code>docs/accounts.md</code>).",
     "Den som sköter Hemma-servern kan få tillbaka administratören: se avsnittet <b>Locked out</b> i Hemmas "
     "dokumentation om konton (<code>docs/accounts.md</code>).",
     "Quien gestione el servidor de Hemma puede recuperar al administrador: mira el apartado <b>Locked out</b> en la "
     "documentación de cuentas de Hemma (<code>docs/accounts.md</code>)."},

    /* the data was damaged (issue #87) */
    {"Hemma's data was damaged", "Hemmas data har skadats", "Los datos de Hemma se dañaron"},
    {"Nothing has been deleted.", "Inget har raderats.", "No se ha borrado nada."},
    {"No admin account could be read back, so nobody can look after Hemma right now. Whoever runs the Hemma server "
     "can get more back from the damaged copy - see <b>A damaged database</b> in Hemma's troubleshooting guide "
     "(<code>docs/troubleshooting.md</code>) - and bring an admin back: <b>Locked out</b> in the account docs "
     "(<code>docs/accounts.md</code>).",
     "Inget administratörskonto gick att läsa tillbaka, så ingen kan sköta Hemma just nu. Den som sköter "
     "Hemma-servern kan få tillbaka mer från den skadade kopian - se avsnittet <b>A damaged database</b> i Hemmas "
     "felsökningsguide (<code>docs/troubleshooting.md</code>) - och få tillbaka en administratör: avsnittet "
     "<b>Locked out</b> i dokumentationen om konton (<code>docs/accounts.md</code>).",
     "No se pudo recuperar ninguna cuenta de administrador, así que ahora mismo nadie puede ocuparse de Hemma. Quien "
     "gestione el servidor de Hemma puede recuperar más de la copia dañada - mira el apartado <b>A damaged "
     "database</b> en la guía de solución de problemas de Hemma (<code>docs/troubleshooting.md</code>) - y recuperar "
     "un administrador: el apartado <b>Locked out</b> en la documentación de cuentas (<code>docs/accounts.md</code>)."},
    {"Hemma's data was damaged, and it rescued what it could still read: some things may be missing.",
     "Hemmas data skadades, och Hemma räddade det som fortfarande gick att läsa: en del kan saknas.",
     "Los datos de Hemma se dañaron y Hemma rescató lo que aún podía leer: puede que falten algunas cosas."},

    /* the welcome page (the first admin) and its errors */
    {"Welcome", "Välkommen", "Bienvenida"},
    {"Welcome to Hemma", "Välkommen till Hemma", "Te damos la bienvenida a Hemma"},
    {"Let's create the admin account: the person who adds everyone else and looks after the settings.",
     "Nu skapar vi administratörskontot: personen som lägger till alla andra och sköter inställningarna.",
     "Vamos a crear la cuenta de administrador: la persona que añade a todos los demás y se ocupa de los ajustes."},
    {"Setup code", "Installationskod", "Código de configuración"},
    {"From where Hemma was installed - see below", "Där Hemma installerades - se nedan",
     "De donde se instaló Hemma - mira abajo"},
    {"Your name", "Ditt namn", "Tu nombre"},
    {"e.g. Alex", "t.ex. Alex", "p. ej. Alex"},
    {"Username", "Användarnamn", "Nombre de usuario"},
    {"e.g. alex", "t.ex. alex", "p. ej. alex"},
    {"You sign in with this: a–z, 0–9 and . _ -", "Du loggar in med det här: a–z, 0–9 och . _ -",
     "Con esto inicias sesión: a–z, 0–9 y . _ -"},
    {"Password", "Lösenord", "Contraseña"},
    {"Name of your home <span class=\"muted\">(optional)</span>", "Namn på ditt hem <span class=\"muted\">(valfritt)</span>",
     "Nombre de tu hogar <span class=\"muted\">(opcional)</span>"},
    {"Create the admin account", "Skapa administratörskontot", "Crear la cuenta de administrador"},
    {"Next you get recovery codes: your way back in if you ever forget the password.",
     "Sedan får du återställningskoder: din väg tillbaka in om du någon gång glömmer lösenordet.",
     "Después recibirás códigos de recuperación: tu forma de volver a entrar si alguna vez olvidas la contraseña."},
    {"<b>Where's the setup code?</b> It shows that you're the one setting Hemma up - not just anyone on your Wi-Fi "
     "who opens this page first. You'll find it where Hemma was installed:",
     "<b>Var finns installationskoden?</b> Den visar att det är du som ställer in Hemma - inte vem som helst på ditt "
     "wifi som råkar öppna den här sidan först. Du hittar den där Hemma installerades:",
     "<b>¿Dónde está el código de configuración?</b> Demuestra que eres tú quien configura Hemma - no cualquiera en tu "
     "wifi que abra esta página primero. Lo encontrarás donde se instaló Hemma:"},
    {"<b>On this Raspberry Pi's memory card</b>, in the file <code>%s</code>: put the card in a computer and open it "
     "(then back in the Pi)",
     "<b>På den här Raspberry Pi:ns minneskort</b>, i filen <code>%s</code>: sätt kortet i en dator och öppna filen "
     "(sätt sedan tillbaka kortet i Pi:n)",
     "<b>En la tarjeta de memoria de esta Raspberry Pi</b>, en el archivo <code>%s</code>: pon la tarjeta en un "
     "ordenador y ábrelo (luego devuelve la tarjeta a la Pi)"},
    {"<b>Raspberry Pi or Linux</b>: in the installer's last lines. Missed them? It's also in "
     "<code>/etc/hemma/hemma.env</code>, or run <code>sudo journalctl -u hemma | grep \"setup code\"</code>",
     "<b>Raspberry Pi eller Linux</b>: på installationsprogrammets sista rader. Missade du dem? Den finns också i "
     "<code>/etc/hemma/hemma.env</code>, eller kör <code>sudo journalctl -u hemma | grep \"setup code\"</code>",
     "<b>Raspberry Pi o Linux</b>: en las últimas líneas del instalador. ¿Te las perdiste? También está en "
     "<code>/etc/hemma/hemma.env</code>, o ejecuta <code>sudo journalctl -u hemma | grep \"setup code\"</code>"},
    {"<b>Windows or Mac</b>: in Hemma's own window (or its log), right after it starts",
     "<b>Windows eller Mac</b>: i Hemmas eget fönster (eller dess logg), direkt efter att det startat",
     "<b>Windows o Mac</b>: en la propia ventana de Hemma (o en su registro), justo después de que arranque"},
    {"It works once: after this the welcome page and the code are gone for good.",
     "Den fungerar en gång: efter det är välkomstsidan och koden borta för gott.",
     "Funciona una sola vez: después, la página de bienvenida y el código desaparecen para siempre."},
    {"I already have an account", "Jag har redan ett konto", "Ya tengo una cuenta"},
    {"That isn't the setup code. It was shown where Hemma was installed - see below where to look.",
     "Det är inte installationskoden. Den visades där Hemma installerades - se nedan var du hittar den.",
     "Ese no es el código de configuración. Se mostró donde se instaló Hemma - abajo verás dónde buscarlo."},
    {"Please tell us your name.", "Berätta vad du heter.", "Dinos cómo te llamas."},
    {"Usernames are 2–32 characters: a–z, 0–9 and . _ -", "Användarnamn har 2–32 tecken: a–z, 0–9 och . _ -",
     "Los nombres de usuario tienen 2–32 caracteres: a–z, 0–9 y . _ -"},
    {"That username is taken - pick another one.", "Det användarnamnet är upptaget - välj ett annat.",
     "Ese nombre de usuario ya está en uso - elige otro."},
    {"Use at least 8 characters for the password.", "Använd minst 8 tecken i lösenordet.",
     "Usa al menos 8 caracteres en la contraseña."},
    {"That password is a bit long - 200 characters at most.", "Lösenordet är lite för långt - högst 200 tecken.",
     "Esa contraseña es un poco larga - 200 caracteres como máximo."},
    {"The two passwords don't match.", "De två lösenorden är inte likadana.", "Las dos contraseñas no coinciden."},
    {"That username and recovery code don't match.", "Användarnamnet och återställningskoden hör inte ihop.",
     "Ese nombre de usuario y ese código de recuperación no coinciden."},

    /* forgot your password? */
    {"Forgot password", "Glömt lösenord", "Contraseña olvidada"},
    {"Forgot your password?", "Glömt ditt lösenord?", "¿Olvidaste tu contraseña?"},
    {"Use one of your recovery codes to choose a new one.", "Använd en av dina återställningskoder för att välja ett nytt.",
     "Usa uno de tus códigos de recuperación para elegir una nueva."},
    {"Username or name", "Användarnamn eller namn", "Nombre de usuario o nombre"},
    {"Recovery code", "Återställningskod", "Código de recuperación"},
    {"New password", "Nytt lösenord", "Nueva contraseña"},
    {"Set the new password", "Spara det nya lösenordet", "Guardar la nueva contraseña"},
    {"No codes left? Ask an admin of your household to set a new password for you - or whoever runs the Hemma server "
     "can reset it there (see Hemma's account docs).",
     "Inga koder kvar? Be en administratör i ditt hushåll att ge dig ett nytt lösenord - eller så kan den som sköter "
     "Hemma-servern återställa det där (se Hemmas dokumentation om konton).",
     "¿No te quedan códigos? Pide a un administrador de tu hogar que te ponga una contraseña nueva - o quien gestione "
     "el servidor de Hemma puede restablecerla allí (mira la documentación de cuentas de Hemma)."},
    {"Back to sign in", "Tillbaka till inloggningen", "Volver al inicio de sesión"},

    /* signing in */
    {"Welcome back, %s", "Välkommen tillbaka, %s", "Hola de nuevo, %s"},
    {"to %s", "på %s", "en %s"},
    {"A demo: sign in as <b>anna</b> (an admin) or <b>jonas</b>, password <b>%s</b>.",
     "En demo: logga in som <b>anna</b> (administratör) eller <b>jonas</b>, lösenord <b>%s</b>.",
     "Una demo: inicia sesión como <b>anna</b> (administradora) o <b>jonas</b>, contraseña <b>%s</b>."},
    {"Not %s? <a href=\"/login?other=1&amp;next=%u\">Sign in as someone else</a>",
     "Inte %s? <a href=\"/login?other=1&amp;next=%u\">Logga in som någon annan</a>",
     "¿No eres %s? <a href=\"/login?other=1&amp;next=%u\">Inicia sesión como otra persona</a>"},
    {"That username and password don't match.", "Användarnamnet och lösenordet hör inte ihop.",
     "Ese nombre de usuario y esa contraseña no coinciden."},
    {"You're signed out.", "Du är utloggad.", "Has cerrado sesión."},
    {"This Hemma already has an admin. Sign in, or ask them to add you.",
     "Den här Hemma har redan en administratör. Logga in, eller be administratören lägga till dig.",
     "Este Hemma ya tiene un administrador. Inicia sesión o pídele que te añada."},
    {"Your sign-in has ended. To stay longer, ask %r for more days.",
     "Din inloggning har gått ut. Vill du stanna längre, be %r om fler dagar.",
     "Tu acceso ha terminado. Para quedarte más tiempo, pide más días a %r."},
};
TRANSLATIONS(auth, TR)

# Accounts: the admin, everyone else, and forgotten passwords

Everything in Hemma sits behind a sign-in. This page covers the whole story: creating the
first account, adding the rest of the household, recovery codes, what to do when someone
forgets their password, and the last resort when nobody can get in.

## Who's who

Everyone in the household is a **person** in Hemma. A person can have a **sign-in**
(a username and a password), or not: a toddler can be a person that things get booked
for, without signing in. On the sign-in page you can type your **username or your name**
("admin" or "Alex"); a name only works when nobody else with a sign-in has the same one.

A sign-in is one of three kinds:

| Kind | Can |
|---|---|
| **Member** | Use everything |
| **Admin** | Use everything, and also add and remove people, rename the home, manage media folders |
| **Display** | Only look. For the living-room TV or a tablet on the wall: it shows its screen but can't change anything ([a TV](#a-tv-whos-watching)) |

There is always at least one admin who can sign in. Hemma refuses any change that would
leave none: removing the last one, making them a member, or taking their sign-in away.

## The first admin: the welcome page

The very first time anyone opens Hemma, it shows a **Welcome to Hemma** page instead of
the sign-in page. Fill in:

- **Setup code**, the one-time code from where you installed Hemma, like `7KQ4-M2XP`. It
  proves you're the person who installed it, not just anyone on your Wi-Fi who opened the
  page first. Where to find it:
  - **Raspberry Pi or Linux (installer)**: the installer's last lines; also
    `sudo grep SETUP /etc/hemma/hemma.env`, or `sudo journalctl -u hemma | grep "setup code"`.
  - **Windows or Mac**: Hemma's own window (or its log) right after it starts:
    `The welcome page asks for this setup code: 7KQ4-M2XP`.

  Case and the dash don't matter. It stays the same across restarts until the admin
  exists, then it's gone. To pick your own, set `HEMMA_SETUP_CODE` in the settings file
  (`hemma --gen-setup-code` makes one).
- **Your name**, as the household knows you ("Alex").
- **Username**, what you'll sign in with: lower-case letters, digits and `.` `_` `-`
  ("alex").
- **Password**, twice. At least 8 characters. A short sentence is easy to remember and
  hard to guess.
- **Name of your home** (optional). It shows at the top of the pages. You can change it
  later under **Household**.

Press **Create the admin account**. You're signed in straight away, and Hemma shows your
**recovery codes** (see below). Save them, press **I've saved them**, and you land on
**Household**, where you add everyone else.

Good to know:

- **Only you can do it.** Without the setup code nobody becomes the admin, even if they
  reach the welcome page first. Wrong codes are refused, and a device that keeps guessing
  has to wait. When Hemma starts without an admin, its log says so:
  `No admin yet - open http://192.168.1.50:8080/ in a browser to create one`, followed by
  `The welcome page asks for this setup code: ...`.
- The welcome page appears **only once** in Hemma's life. As soon as the admin exists it
  is gone for good, also if someone tries to post the form by hand. If two people press
  the button at the same moment, only one account is created.
- Hemma makes its secret salt (`HEMMA_AUTH_SALT`) by itself on its first start and writes
  it into the settings file, readable by its owner only. If the page says **Almost there**
  instead, it couldn't: the log says why (usually a settings file it isn't allowed to
  write). Add a line `HEMMA_AUTH_SALT=` followed by the output of `hemma --gen-salt`, then
  restart Hemma.
- Don't want a welcome page at all, for example on a server you set up remotely? Create
  the admin from the settings file instead: see
  [Setting up the admin from the settings file](#setting-up-the-admin-from-the-settings-file).

## Adding everyone else

As the admin, go to **Household** and press **Add someone**. Give them a name, and if
they should sign in, a username and a password. Choose what they can do: *Member*,
*Admin* or *Display*. Tell them their username and password; they can change the
password themselves under **Household → Your account → Change password**.

That's also where admins change their own password: it asks for the current one first.
(The pencil next to your own name edits your profile, not your password.)

Each person can then make their own recovery codes (see [Recovery codes](#recovery-codes)).

## A TV: "Who's watching?"

A TV everyone shares gets a **Display** sign-in of its own, set up once:

1. As the admin, under **Household**, press **Add someone**: a name ("Living room TV"), a
   username, a password, and the kind *Display*.
2. On the TV's web browser, open Hemma - type the address with `http://`, like
   `http://192.168.1.50` - and sign in with it, **Keep me signed in** ticked.
3. Under **Screens**, edit a screen (Hemma starts with one called *TV*) and tick the TV
   under **Shows on**. From then on the TV's Hemma address opens that screen by itself.

The screen is big rows you move through with the remote's arrows, like a streaming app:
Continue watching, On now, Recently added, Series, Movies. OK opens what's highlighted.
Admins choose the rows and their order under **Screens → Edit** (Up, Down, Remove, Add a
row) and can give a row a heading of its own. A row with nothing to show right now - no
channels yet, nothing half-watched - is simply left out.

The TV starts with everyone's picture: **Who's watching?** Pick yourself with the remote -
no password. Continue watching then shows your shows, and what you watch on the TV counts
as yours. That's all anyone can do on the TV: watch. **Switch** (top right) hands it to
someone else, and after **4 hours without use** (nothing opened, nothing playing) the TV
asks again by itself, so the next person doesn't watch as you. On your own phone or
computer you're always yourself: **Screens** shows the same rows there, no question asked.

## Recovery codes

Recovery codes are your spare keys. If you ever forget your password, one code lets you
choose a new one, without needing anybody's help.

- You get **8 codes**. They look like `7KQ4-M2XP-9RTA`.
- **Each code works once.** Cross it off after using it.
- The admin gets them when the account is created. Everyone else makes theirs under
  **Household → Your account → Recovery codes → Make recovery codes** (Hemma asks for
  your password first).
- Hemma shows the codes **only once**, right after making them. Keep them somewhere safe:
  - in a password manager, next to your Hemma password, or
  - printed on paper (use the browser's *Print*), kept in a drawer, or
  - press **Download as a text file** and store the file somewhere safe.
- Anyone who has one of your codes *and* knows your username can choose a new password
  for you, so treat them like a password.
- **Household → Your account** shows how many codes you have left. Running low, or lost
  the paper? Press **Make new codes**: you get 8 fresh ones and **the old ones stop
  working**.
- Hemma never stores the codes themselves, only a scrambled fingerprint of each, so even
  a copy of the database doesn't reveal them.
- Display accounts (wall tablets) don't have recovery codes. An admin resets their
  password if needed.

## Forgot your password?

1. On the sign-in page, press **Forgot your password?**
2. Enter your **username**, one of your **recovery codes** and a **new password** (twice).
   Capitals and dashes in the code don't matter: `7kq4 m2xp 9rta` works too.
3. Press **Set the new password**.

You're signed in with the new password. For safety, Hemma signs your account out on
every other phone, tablet and computer; sign in there again with the new password.
Hemma also tells you how many codes you have left. If it's only one or two, make new ones
under **Household → Your account**.

If the code doesn't work, Hemma just says the username and code don't match (it never
tells whether a username exists). After several wrong tries it asks you to wait a few
minutes before the next try. That's on purpose: it stops anyone from guessing.

**No codes left?** Two ways out:

- **Ask an admin.** An admin can set a new password for you: **Household**, the pencil
  next to your name, **New password**. That signs you out on every device; sign in with
  the new password, then change it to one only you know.
- **If you are the admin** (or no admin can help), whoever runs the Hemma server can reset
  it there. That's the next section.

## Locked out completely: reset the password on the server

This is the last resort, for when the recovery codes are gone too. It needs access to the
computer Hemma runs on (for a Raspberry Pi, that's usually over SSH; see
[ssh.md](ssh.md)). Nothing is deleted: only the password changes.

Run Hemma once more with `--reset-password` and the username, using **the same settings
file and data folder** as the running Hemma, and **as the same user** that Hemma runs as.

**On a Raspberry Pi or Linux server installed with `install-pi.sh`:**

```sh
sudo -u hemma /opt/hemma/hemma --env /etc/hemma/hemma.env --data /var/lib/hemma --reset-password alex
```

**On Windows** (if Hemma lives in `C:\Hemma`):

```powershell
C:\Hemma\hemma.exe --env C:\Hemma\hemma.env --data C:\Hemma\data --reset-password alex
```

**On a Mac or Linux without the installer** (if Hemma lives in `~/hemma`, as in the Linux
guide; the macOS guide uses `~/Hemma`):

```sh
~/hemma/hemma --env ~/hemma/hemma.env --data ~/hemma/data --reset-password alex
```

It prints a temporary password:

```
Temporary password for Alex (@alex):

    k7qm-x2pt-9rta-hw4e

Hemma is running and has applied it: the old password no longer works and every device
was signed out.
Sign in with it - Hemma then asks for a new password of your own.
```

- Hemma doesn't need to be stopped. A running Hemma picks the new password up within a
  few seconds. If Hemma isn't running, the password takes effect when it next starts.
- Sign in with the temporary password (dashes included). Hemma takes you to **Household**
  and asks you to choose your own password right away.
- Forgot the username too? Run the command with any name: Hemma lists the usernames it
  knows.
- If your recovery codes might have fallen into someone's hands, make new ones afterwards
  (**Household → Your account → Make new codes**).

Why `sudo -u hemma`? On a Pi, Hemma's files belong to the user `hemma`. Running the
command as that user keeps it that way. (Running it as root could leave database files
the service can't open.)

### No admin account left at all

If the admin account itself is gone (for example after experimenting directly with the
database), `--reset-password` has nothing to reset. Then bring an admin back through the
settings file:

1. Add these three lines to the settings file, with a **new** password:

   ```
   HEMMA_ADMIN_USER=alex
   HEMMA_ADMIN_PASSWORD=a-new-long-password
   HEMMA_ADMIN_RESET=1
   ```

2. Restart Hemma (on a Pi: `sudo systemctl restart hemma`). The account `alex` is an
   admin again with that password, and is signed out everywhere.
3. Sign in, then **remove those three lines** from the settings file again. (Forgetting
   them does no harm: each reset is applied only once.)

## Setting up the admin from the settings file

Instead of the welcome page, the first admin can come from the settings file. This is
handy when you set up Hemma remotely, or when nobody else on the network should be able
to reach the welcome page first.

Before Hemma's **very first start**, add to the settings file (`.env`, or
`/etc/hemma/hemma.env` on a Pi):

```
HEMMA_ADMIN_USER=alex
HEMMA_ADMIN_PASSWORD=a-long-password
HEMMA_ADMIN_NAME="Alex"
```

(`HEMMA_ADMIN_NAME` is optional.) When Hemma starts, it creates that admin and the
welcome page never appears. Sign in, then:

- remove the `HEMMA_ADMIN_PASSWORD` line again (it's only used that one time),
- make your recovery codes under **Household → Your account**.

Hemma uses these lines only on the very first start. Later changes to them do nothing:
people are managed in Hemma itself, and an admin you rename or remove there stays renamed
or removed. (To force a change, use `HEMMA_ADMIN_RESET` as described above.)

## Good to know

- **The secret salt matters.** Every password and every recovery code depends on
  `HEMMA_AUTH_SALT` in the settings file (made on the first start). If it changes or gets
  lost, nobody's password works any more (fix: put the old salt back, or reset each
  password on the server). Hemma never makes a new one once there are passwords - it
  warns instead. Keep a copy of that line somewhere safe, separate from your database
  backups: a stolen copy of the database alone is then useless for guessing passwords.
- **Signing in is protected against guessing.** After 5 wrong passwords (or recovery
  codes) from one device, Hemma makes that device wait a minute, then longer. Wrong tries
  from many devices against one account are limited too, and so are 10 wrong tries from
  one device whatever the usernames (checking a password takes real work, on a Raspberry
  Pi about a second: a flood of made-up names mustn't slow Hemma down for everyone).
  Wrong current passwords under **Your account** count towards that too. Waiting devices
  and accounts show up in Hemma's log. When lots of sign-in tries arrive at once, a device
  that hasn't signed in since Hemma last started is asked to wait a few seconds (asking
  again and again counts like wrong passwords); devices that have stay quick, and so does
  Hemma for everyone already signed in.
- **"Keep me signed in"** keeps you signed in on that device for a year, as long as you
  use it now and then (unticked: until a week without use). **Admins sign in again every
  30 days**, however much they use it: Hemma runs on plain http, so a stolen admin cookie
  stops working within a month.
- **"Welcome back, Eva" when you aren't Eva?** The sign-in page guesses who you are from
  the device's address on the network. Tap **Not Eva? Sign in as someone else**.
- **Nobody can sign in, and the log says "HEMMA_AUTH_SALT isn't the secret this
  household's passwords were made with"?** Hemma was started with a different settings
  file (another computer's, or one that lost its line). Put the original `HEMMA_AUTH_SALT`
  line back and restart.
- **See where you're signed in.** **Household → Your account → Signed in on** lists your
  devices. **Sign out other devices** ends all of them except the one you're holding.
  Changing your password does that too.
- **An admin can sign someone out everywhere**: **Household**, the pencil next to their
  name, **Sign *name* out everywhere**.

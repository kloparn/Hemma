# Accounts: the admin, everyone else, and forgotten passwords

Everything in Hemma sits behind a sign-in. This page covers the whole story: creating the
first account, adding the rest of the household, recovery codes, what to do when someone
forgets their password, and the last resort when nobody can get in.

## Who's who

Everyone in the household is a **person** in Hemma. A person can have a **sign-in**
(a username and a password), or not: a toddler can be a person that things get booked
for, without signing in. On the sign-in page you can type your **username or your name**
("admin" or "Alex"); a name only works when nobody else with a sign-in has the same one.

A sign-in is one of five kinds:

| Kind | Can |
|---|---|
| **Member** | Use everything |
| **Child** | Their own to-dos, the shopping list, the board, booking what's open to children, and watching - without the folders marked *Not for children* ([children](#children)) |
| **Guest** | Until a last day: book what's open to guests, the shopping list, the board, watching ([guests](#guests)) |
| **Admin** | Use everything, and also add and remove people, rename the home, add, fix and remove videos and manage media folders |
| **Display** | Only look. For the living-room TV or a tablet on the wall: it shows its screen but can't change anything ([a TV](#a-tv-whos-watching)) |

There is always at least one admin who can sign in. Hemma refuses any change that would
leave none: removing the last one, making them a member, or taking their sign-in away.

## The first admin: the welcome page

The very first time anyone opens Hemma, it shows a **Welcome to Hemma** page instead of
the sign-in page. Fill in:

- **Setup code**, the one-time code from where you installed Hemma, like `7KQ4-M2XP`. It
  proves you're the person who installed it, not just anyone on your Wi-Fi who opened the
  page first. Where to find it:
  - **Raspberry Pi**: the file `HEMMA-SETUP.txt` on its memory card (shut the Pi down, put the
    card in any computer, open the file, put the card back), or the Pi's screen if one is
    plugged in.
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
*Child*, *Guest* (with their last day), *Admin* or *Display*. Tell them their username and
password; they can change the password themselves under **Household → Your account →
Change password**.

That's also where admins change their own password: it asks for the current one first.
(The pencil next to your own name edits your profile, not your password.)

Each person can then make their own recovery codes (see [Recovery codes](#recovery-codes)).

## Children

Give a child the kind **Child**, with a sign-in of their own or without one (then they're
still someone things are booked for, and they can be picked on the TV). Signed in, a child:

- sees **their own to-dos** under Plan, adds to-dos for themselves, ticks theirs off and
  takes off only what they put on themselves - a chore a grown-up gave them stays;
- uses the **shopping list** and the **board** (changing only their own notes), and looks
  at the household's calendar;
- **books only what's open to children**, for themselves, and cancels or moves only their
  own bookings. Which things those are is up to an admin: under **Book**, edit a thing and
  tick **Children may book it** (a new TV room and sauna start ticked, a car doesn't;
  things you had before this version start unticked);
- **watches** - everything except folders an admin marked **Not for children**: open the
  folder under **Watch → Folders** and tick it (Library settings lists them all, and opens
  one again). Those videos aren't in a child's lists, search or Live TV, and their address
  doesn't play for them either. A series with an episode in such a folder goes whole, and
  so does a channel that would air something from one. The same goes for a TV while a
  child is picked under *Who's watching?*;
- watches, but doesn't **download** videos (that's for grown-ups: members and admins);
- doesn't get the **Network** page, the household's settings or anyone else's account.

Anything else a child tries says **Ask a grown-up to do that**.

## Guests

A guest - a lodger, family staying a fortnight - gets the kind **Guest** and a **last day**.
Their sign-in ends after that day by itself: their devices are signed out, and signing in
says *Your sign-in has ended* and who to ask for more days. To give them more, edit them and
pick a later day (or make them a member).

Until then a guest books what an admin opened to guests (tick **Guests may book it** on a
thing: the guest room, laundry and sauna start ticked), uses the shopping list and the
board, and watches (without downloading). They don't see who's home, the household's news, calendar, to-dos,
screens or Network page, and under Household only their own account. Guests get no
recovery codes: if they forget their password, an admin sets a new one.

## When someone moves out

Under **Household**, edit them and press **Remove**. Before anything happens, Hemma shows
what removing them would do, two ways:

- **They moved out - keep their history.** What they did stays, under their name: bookings
  that happened, notes on the board, the Latest feed, what they watched. What's still ahead
  of them is handed on: bookings to come are cancelled, a repeating booking or event stops,
  their open to-dos go to *anyone* (or, for a chore taking turns, to the next one in line),
  their devices are nobody's. Their sign-in goes. They're listed under **Moved out**, where
  **Back in the household** brings them back (give them a sign-in again under Edit), and
  **Erase** removes the rest.
- **Erase everything about them.** All of that, and their past bookings, done to-dos,
  notes, watch history and lines in Latest go too. Things they added for everyone (an event,
  a shopping item, a to-do for someone else) stay, without their name.

A TV's or tablet's sign-in has no history: removing it just removes it.

## Privacy: what others see of you

Under **Household → Your account → Privacy** everyone chooses for themselves:

- **On the wall tablet**: with home or away, just your name, or not at all.
- **Others see when I have Hemma open, and which part** - untick it, and nobody sees you
  online or what you're looking at.
- **On the TV's *Who's watching?*** - see [a TV](#a-tv-whos-watching).
- **My phone and computer may tell Hemma when I'm home** - untick it, and your devices never
  say you're home (or away): you're simply not shown either way.

Which devices count as yours is the admin's to set on the **Network** page; there, under
**Your devices**, you can rename yours or say one isn't yours.

## Language: English, Swedish or Spanish

Hemma speaks the language your browser asks for (the one your phone or computer is set to), and
English when that's none of the three. Everyone can choose their own under **Household → Your
account → Language** - it follows you to every device you sign in on. **Automatic** goes back to
the browser's.

The sign-in page has **English · Svenska · Español** at the bottom, for a browser that asks for
something else. A TV or a wall tablet signed in with a display account can't change anything
itself: an admin picks its language in its dialog under **Household**.

Quick text works in all three: *"car fri 14-17"*, *"bastu ikväll 19"*, *"coche mañana de 14 a 17"*.
The calendar on your phone comes in your language too. The server's log, the command line and
these guides stay in English.

## A TV: "Who's watching?"

A TV everyone shares gets a **Display** sign-in of its own, set up once:

1. As the admin, under **Household**, press **Add someone**: a name ("Living room TV"), a
   username, a password, and the kind *Display*.
2. On the TV's web browser, open Hemma - type the address with `http://`, like
   `http://192.168.1.50` - and sign in with it, **Keep me signed in** ticked. The
   **Screens** page shows the address to type, in big letters, and as a QR code a tablet or
   phone can scan instead (TVs can't). Type the numbers: many TVs can't look up
   `hemma.local` names.
3. Under **Screens**, edit a screen (Hemma starts with one called *TV*) and tick the TV
   under **Shows on**. From then on the TV's Hemma address opens that screen by itself.

The screen is big rows you move through with the remote's arrows, like a streaming app:
Continue watching, On TV, Recently added, Series, Movies. OK opens what's highlighted.
The remote's **Back** key goes back from a series, a video or a channel (in full screen it
leaves full screen first); the arrows reach the **Back** button at the top too - press Up
on a playing video.
Admins choose the rows and their order under **Screens → Edit** (Up, Down, Remove, Add a
row) and can give a row a heading of its own. A row with nothing to show right now - no
channels yet, nothing half-watched - is simply left out.

The TV starts with the household's pictures: **Who's watching?** Pick yourself with the
remote. No password is needed. Continue watching then shows your shows, and what you watch
on the TV counts as yours. That's all anyone can do on the TV: watch. **Switch** (top
right) hands it to someone else. After **4 hours without use** (nothing opened, nothing
playing) the TV asks again by itself, so the next person doesn't watch as you. On your own
phone or computer you're always yourself: **Screens** shows the same rows there, no
question asked.

Whoever holds the TV's sign-in can watch as anyone it lists, see their history and clear
it. So:

- **You choose whether it lists you**: **Show me on the TV** under **Screens** (the "You on
  the TV" card) or **Household → Privacy**. Members and children are listed until they
  untick it; admins and guests aren't until they tick it.
- **Anyone can set a PIN** there: four digits the TV asks for when you're picked. After
  five wrong tries it waits a minute, then longer, before it lets anyone try again.
- **A TV's or tablet's sign-in ends 90 days after signing in**, however much it's used, so
  a stolen copy of it stops working. Sign the TV in again then.

## A tablet on the wall

A wall tablet works the same way: its own **Display** sign-in, then under **Screens**, edit
the screen called *Wall* (Hemma makes one for you) and tick the tablet under **Shows on**.
On a computer, `http://<your Hemma address>/wall` opens it too.

The wall shows a big clock and the date, what's on **now and next** (a booking that's
running, the next thing in the calendar - today or tomorrow) and **who's home**. Below come
cards: Today, Bookings (each thing, free or whose and until when), Coming days, the
shopping list, notes, what's on Live TV, what's new to watch, and the latest things people
did. A card with nothing to show stays away, so a quiet day makes a calm wall. Admins pick
the cards and their order under **Screens → Edit**, and can make more walls (**New screen →
Wall**). The wall only shows: ticking things off happens on your phone.

## Your calendar on your phone

Your bookings, your to-dos and the household's events can show in your phone's own calendar
app, next to everything else in your life. The phone keeps a copy, so you see them when
you're out too, and its reminders work as usual.

1. Open Hemma (on the phone, or on a computer at home) and go to **Household → Your calendar
   on your phone**.
2. Press **Make my calendar link**.
3. **iPhone:** press **Add to this device's calendar** and choose **Subscribe**. (Or open
   the page on a computer and scan its code with the iPhone's camera.)
   **Android:** install **ICSx⁵** (free, from Google Play or F-Droid), add a subscription
   and paste the `http://` address shown under the button.
   **A computer:** Outlook, Apple Calendar and Thunderbird can subscribe to the same
   address ("add a calendar from the internet" or similar).

Good to know:

- Hemma isn't on the internet, so the phone brings its copy up to date **while it's on the
  home Wi-Fi** (or over your VPN, see [home-network.md](home-network.md#reaching-hemma-from-outside-the-safe-way)).
  Google Calendar's own website can't reach a home network, which is why Android goes through
  ICSx⁵.
- What's in it: your bookings, your to-dos with a day and the ones for anyone, and the
  household's events and yours, from a month back to a year ahead. A to-do still open after
  its day shows on today, saying when it was due. Tick **Every booking in the household, not
  only yours** to see everyone's bookings too.
- **Anyone with the link can read that calendar**, so keep it to your own devices. Shared it
  by mistake, or lost a phone? **Make a new link**: the old one stops working at once. **Turn
  it off** stops it altogether.
- Open the page by Hemma's address at home (like `http://hemma.local`), not on the server
  itself at `127.0.0.1`: the link uses the address you opened the page by.
- Display accounts (a TV, a wall tablet) have no calendar link.

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
  stops working within a month. **Display sign-ins (a TV, a wall tablet) end after 90
  days** the same way.
- **What you typed stays out of the address.** After a wrong password, Hemma puts the name
  you typed back in the box for a minute, but never in the page's address. That matters
  when a password lands in the name box by mistake: an address ends up in the browser's
  history and suggestions.
- **Video addresses carry a key** (`?k=...` at the end). Some TVs' video players fetch the
  video without the browser's sign-in, so the address proves it instead. A key works for
  that one video (or subtitle), only from the network address of the device it was shown
  on, for about two days, and stops working sooner when that device signs out or its
  sign-in runs out. It never opens a page. Someone else on that same address (a device
  that takes it over) could use a copied key until then.
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

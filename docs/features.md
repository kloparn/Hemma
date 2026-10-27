# What Hemma does

Everything in Hemma, section by section. The [README](../README.md) has the short version.

## Home

A greeting, who's home, a card from every section and the latest news.

## Book

Shared things like the car, the laundry room, the guest room or the sauna.

- A week view, and a warning when two bookings clash.
- Quick text, the way you'd say it: *"car fri 14-17 ikea"*, *"bastu ikväll 19"* or *"coche mañana de
  14 a 17"* - in English, Swedish or Spanish, and with any other names you give a thing. Hemma shows the
  booking, and whether the thing is free, before you press Enter.
- Repeating bookings: *"laundry every tuesday 18-21"*.
- Whoever's booking someone else cancels or moves is told.

## Plan

- Chores with repeats and people (*"vacuum @sam fri every week"*), and chores that take turns
  (*"vacuum every week @sam,@alex"*): it shows whose turn is next, and a turn can be swapped.
- A shared shopping list that remembers what you often buy.
- A calendar that also shows bookings, with repeating events (*"football practice every wednesday
  17:30-19"*).
- Your bookings, to-dos and the household's events in your phone's own calendar, with its reminders
  ([how](accounts.md#your-calendar-on-your-phone)).

## Watch

Your movie and series drive as a little cinema.

- **Sorted for you.** Series in seasons and specials; films and series in categories (Comedy,
  Animated, Kids, or your own) - ticked by hand, read from .nfo files, or looked up online if an admin
  switches that on. List them by category, by what's newest, or by what you watched lately.
- **Search** by any words, in any order.
- **Subtitles**, the ones inside your video files too - even DVD and Blu-ray subtitles, which are
  pictures, not text.
- **Remembers where you stopped**, for each person.
- **Plays everywhere.** Hemma converts videos a browser can't play (DVD rips, Dolby sound that Chrome
  plays silently, DTS sound no browser plays) into a copy that does, keeping the file's subtitles as
  .srt files, and puts HEVC films into a file Safari on iPhone, iPad and Mac can play
  ([which videos](video-formats.md)).
- **Live TV** ("90s TV"): channels that play your shows non-stop on a shared clock, with a TV guide.
- **Downloads** for grown-ups: a video, or a whole season as one .zip, to watch away from home.
  Downloads slow down while someone is watching.

For admins:

- **Adding videos.** Drop videos or whole folders on the page. Before anything is sent, Hemma shows
  where each one will go - episodes into their series and season, films into Movies. The uploading
  computer makes the copies itself, so only the small copy travels and the Pi does no heavy lifting.
  An upload picks up where it stopped if the connection drops, never idles while something is ready
  to go, and skips episodes and films you already have - sending just the subtitles they carry when
  the library's copy has none. Its card shows what's being processed, uploaded and saved, and how fast.
- **Looking after the library.** Remove an episode, a season, a series or a film for good, rename a
  series, fix a title or an episode's number (Hemma renames the files), see what takes the most room,
  and mark folders *Not for children*.

### Posters

Hemma never fetches pictures from the internet, so a new film or series shows as a coloured card
with its title, like *Spring* below. To give it a poster, put `poster.jpg` in a series folder or
`Film Name (Year).jpg` next to a film, or (as an admin) press **Picture** on its page and pick one
from your computer.

<p align="center">
  <img src="img/movies.webp" alt="Watch's Movies page: six films with posters and one, Spring, shown as a coloured card with its title" width="820">
</p>

## Board

The fridge door: notes with pins, colours and a photo (the school letter, the parking permit) that
disappear when they're out of date.

## Network

The devices on your network, who owns which, a good guess at who's home (from phones on the Wi-Fi)
and Wake-on-LAN.

## Screens: the TV

A streaming-app view with big rows you move through with the remote (Continue watching, On TV,
Recently added, Series, Movies, a category), arranged by an admin. The TV asks *Who's watching?*:
pick yourself (no password) and your progress follows you. On the TV you can only watch
([setting one up](accounts.md#a-tv-whos-watching)).

## Screens: the wall

An always-on overview for a tablet on the wall: a big clock, what's on now and next, who's home,
then cards for today, bookings, the coming days, the shopping list, notes, Live TV and what's new -
an admin picks them. Cards with nothing to show stay away. It's signed in with a read-only *display*
account ([setting one up](accounts.md#a-tablet-on-the-wall)).

## Household

- People and their sign-ins: members, children (their own to-dos, what's open to children, films for
  them), guests until a last day, admins, and read-only displays for the TV and the wall.
- Someone moving out keeps their history, or it's all erased - Hemma says what will happen first.
- Everyone's own privacy: on the wall, on the TV, who's home, what they're doing.
- Your home's name, and the link that puts your calendar on your phone.

More in [Accounts](accounts.md).

## System

For admins: is everything working (in plain words), a getting-started list that ticks itself off,
the server's log live, nightly backups you can go back to, updates, and what to do when something's
wrong ([troubleshooting](troubleshooting.md)).

## Languages

Hemma speaks English, Swedish or Spanish: the language each phone or computer asks for, and anyone
can pick their own under **Your account** ([how](accounts.md#language-english-swedish-or-spanish)).
It's all built in.

## The internet

Hemma doesn't need it. The only two things that use it - checking for updates, and looking up the
categories of films and series - are off until an admin switches them on
([why](decisions/0009-categories-looked-up-when-asked.md)). Inside your home, every page except the
sign-in page needs an account in your household; outside it, nobody can reach Hemma as long as you
don't open a port for it on your router ([the safe way in from outside](home-network.md#reaching-hemma-from-outside-the-safe-way)).

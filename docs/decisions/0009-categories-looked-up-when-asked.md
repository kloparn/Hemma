# 0009 - Categories may be looked up online, when an admin asks

**Status:** accepted · **Since:** 2026-10-27 (Hemma 1.4) · amends [0005](0005-nothing-leaves-home.md) · amended by [0012](0012-pictures-and-https-through-curl.md) (pictures, https)

## Context

Hemma 1.4 puts series and films into categories (Comedy, Animated, Kids...) to find them by. A
household can tick them by hand, and Hemma reads the genres a media manager wrote into `.nfo`
files - but a big library already on the disk would take an evening of ticking. The owner asked for
Hemma to look the genres up online too, "if internet is available ... if the user has checked the
box", greyed when the server can't reach the internet, and for a way to correct a wrong match by
pasting the title's id.

## Decision

- A second way out, besides the update check: **looking up categories online**. It is off until an
  admin ticks it (Library settings), and greyed - with the reason - while this Hemma can't reach
  the services; opening that page checks.
- Only a title and its year leave the home: series go to TVmaze (no account or key), films to OMDb
  with a free key the admin pastes in. Nothing about the household - who watches what, names,
  progress - is ever sent. One lookup every few seconds, each title once; what was found stays.
- An admin can give a title's TVmaze or IMDb id instead; that answer is kept and never replaced by
  a lookup by name.
- Hemma itself fetches over plain http only (it carries no TLS library), so only services that
  answer http:// can be used, and nothing secret travels that way. The OMDb key is a free one.
- Pictures and descriptions are still never fetched.

## Consequences

- The README's promise becomes: Hemma doesn't need the internet, and the two things that use it are
  off until an admin switches them on.
- core/fetch.c is Hemma's own small HTTP client (threads only). Anything new that wants the
  internet gets its own box, off by default, greyed while it's unreachable.
- TVmaze answers every name with something, so a title only matches a show holding all its words;
  a wrong match is fixed by pasting the right id.

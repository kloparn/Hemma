# 0012 - Pictures may be fetched too, and https goes through the computer's curl

**Status:** accepted · **Since:** 2026-10-30 (Hemma 2.5) · amends [0009](0009-categories-looked-up-when-asked.md)

## Context

A film or series without a picture shows as a coloured card. Giving each one a poster by hand -
a file in its folder, or Set picture - is an evening's work for a big library, and the owner asked
for Hemma to "automatically fetch if an image is not given", without setting up another key.

Series' posters are on TVmaze, which 1.4 already asks for genres and which serves its pictures over
plain http. Films are harder: OMDb's posters (the categories' films source) come from a server that
answers only https, and every other film source that needs no key - Wikipedia above all - answers
only https too. Hemma has no TLS of its own (no library but SQLite), and 0009 said pictures are
never fetched.

## Decision

- A third way out: **pictures from the internet**, a box of its own on Library settings, off until
  an admin ticks it and greyed while this Hemma can't reach the internet - as for categories.
- Only a series or film **without** a picture gets one: a series the poster of its TVmaze show (the
  show an admin's id named, when there is one), a film the picture of its Wikipedia article, found
  by its title and year (a film of that year, by the article's description). Only titles and years
  are sent. The picture is saved where Set picture saves one - so other players find it too - and
  never over a picture that's there; one of the household's own is never replaced. An admin can give
  the right show's or page's link; that replaces only the picture Hemma saved.
- **https goes through the computer's own curl**, run as a program (core/fetch.c `fetch_https`,
  core/plat.c `plat_run`: no shell, a time limit), which checks the certificate against the
  computer's own list. Curl is on Raspberry Pi OS and Debian, Windows 10 and later, and macOS.
  Without it, films get no pictures; everything else works as before. Hemma still links no TLS
  code, and still sends nothing secret over plain http.

## Consequences

- The README's promise holds with three things instead of two: Hemma doesn't need the internet, and
  what uses it is off until an admin switches it on.
- Hemma runs another program for the first time (the update helper is run by systemd, not by
  Hemma). The systemd unit allows it (`SystemCallFilter=@system-service` holds what posix_spawn
  needs; the program inherits the unit's other limits).
- Wikipedia's film posters are the studios', kept small (fair use, ~250 px wide); fine on a phone
  and the lists, soft on a big TV. TVmaze's are bigger.
- Anything else that needs https later can use the same `fetch_https`, behind its own box.

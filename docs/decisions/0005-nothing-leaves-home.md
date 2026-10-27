# 0005 - Nothing leaves the home unasked

**Status:** accepted, amended by [0009](0009-categories-looked-up-when-asked.md) (2026-10-27: categories looked up online, when an admin asks) · **Since:** the start (2026-10)

## Context

Hemma knows who lives in a home, when they're there, what they book and what they watch. Media
servers usually look films up online for posters and descriptions, and apps report usage.

## Decision

- Hemma never contacts the internet by itself. No posters or descriptions fetched, no accounts
  elsewhere, no analytics.
- A film without a picture shows a drawn card with its title; a household adds pictures by
  putting `poster.jpg` in a folder or choosing one in Hemma.
- The one way out is the update check, and it is off until an admin turns it on (System ->
  Updates); updates are signed releases checked before anything is installed.

## Consequences

- The library looks plainer until pictures are added.
- Hemma works with no internet connection at all; the README promises that nothing leaves the home.

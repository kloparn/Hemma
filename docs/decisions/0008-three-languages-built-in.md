# 0008 - English, Swedish and Spanish, built in

**Status:** accepted · **Since:** 2026-10-20 (issue #46)

## Context

Hemma began in English with Swedish-aware input (the household that started it speaks Swedish).
A household page is used by everyone in the home, children included, in the language they read
best. Translation services and downloadable language packs would break "nothing leaves the home"
([0005](0005-nothing-leaves-home.md)) and add moving parts.

## Decision

- Three languages, compiled into the program: English, Swedish and Spanish.
- The English text is the key: `T("English")` where a text is shown, `N_()` for text shown later
  or to someone else (toasts to others, the Latest feed - translated for each reader). Each
  module keeps its translations beside its code (`*_lang.c`).
- A request runs in the reader's language: their own choice under Your account, else their
  browser's, else English. Quick-add text is understood in all three.
- Logs, the command line and the guides stay English.

## Consequences

- About 300 KB more program, and every new text needs its Swedish and Spanish in the same pull
  request: tests fail on a missing translation, and each module's pages are rendered in all three
  languages to catch text that missed its `T()`.
- A fourth language is a column more in every table, not a new mechanism.

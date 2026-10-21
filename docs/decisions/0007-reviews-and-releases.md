# 0007 - A written review for the core; merging releases

**Status:** accepted · **Since:** 2026-10-21 (issue #48)

## Context

Issue #48 found no recorded review on any merged pull request: large changes merged minutes after
they were opened, with CI as the only gate, and the security and correctness reviews came
afterwards, as outside issues. Hemma holds a household's sign-ins and runs unattended. At the
same time it has one maintainer, and since 0.14.0 merging a version raise publishes a signed
release by itself, which keeps releasing cheap.

## Decision

- **A written review for `core/`.** A pull request that changes `core/` (the server, sign-in,
  the store, the app core, the page shell) gets a review written on the pull request before it is
  merged. An AI review counts, as long as its findings are posted there and answered. Enforced by
  the checklist and habit, not by repository settings.
- **Reviewable pull requests.** Aim for under about 1,500 changed lines; keep features and
  refactors apart. Translations and generated test data don't count towards that.
- **The checklist** in `.github/pull_request_template.md` for every pull request: tests, zero
  warnings, the phone width, the version and its changelog section.
- **Merging releases.** A merge that raises the version publishes a signed release, as before.
  Before the merge, the branch's build has run on the real Raspberry Pi - built for it, copied
  over and started there.

## Consequences

- Reviews slow a `core/` change by a little; the rest stays quick.
- A release can still carry a bug the Pi didn't show in its first hours; the update helper goes
  back to the previous program when a new one doesn't start, and every release can be put back.

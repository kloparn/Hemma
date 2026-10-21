<!-- What it does for the household, in a sentence or two. Link the issue: "Closes #12". -->

## Checklist

- [ ] Tests for what changed (`*_test.c`; `tests/browser` for the player or uploads), and the suite is green
- [ ] Zero warnings, every target (`./build.ps1 all` or `make`)
- [ ] VERSION raised (fixes: the last number, something new or changed: the middle one) and a CHANGELOG.md section
- [ ] New text in all three languages (`T()` + the module's `*_lang.c`) - or say here that you need help
- [ ] New or changed pages checked at phone width (390 px), and without JavaScript
- [ ] Under ~1,500 changed lines, or a reason why not (translations and test data don't count)
- [ ] Changes `core/`? A review is written on this pull request before it's merged ([0007](../docs/decisions/0007-reviews-and-releases.md))
- [ ] A new video format? Its fuzz seed and tests are in ([0006](../docs/decisions/0006-formats-grow-with-fuzzing.md))
- [ ] Ran on a Raspberry Pi (for anything the Pi runs differently: disks, the system, the installer)

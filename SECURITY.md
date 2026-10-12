# Security

Hemma runs inside a home network and puts a sign-in in front of every page. If you find
a way around that, or anything else that could harm a household running it, please
report it privately instead of in a public issue: on GitHub, open the repository's
**Security** tab and choose **Report a vulnerability**.

Please include what you found, how to reproduce it, and the version (`hemma --version`).
Fixes go into a new release, with a note in its release notes once it's out.

## Releases

Every release raises the version (`HEMMA_VERSION` in `core/app.h`) and gets a git tag
(`v0.3.0`) and a GitHub release whose notes come from [CHANGELOG.md](CHANGELOG.md).
`hemma --version` also prints the commit it was built from, so a binary copied onto a
Pi can be matched to the code even between releases. Before a release:

- **SQLite** is vendored (`third_party/sqlite`, pinned and hash-checked), so nothing
  warns about its security fixes automatically: check
  [sqlite.org's release history](https://sqlite.org/changes.html) and update the
  amalgamation when a fix matters to Hemma.
- The CI run of the release commit must be green (tests on Linux and macOS, the suite
  under AddressSanitizer + UBSan, the converter fuzzer).

## What Hemma is built for

- **A trusted home network.** Hemma speaks plain `http://` and is meant to be reached only
  from inside your own network. Don't forward a port to it from the internet; for access
  from outside, use a VPN (see [docs/home-network.md](docs/home-network.md)).
- **Every page needs a sign-in**, except the sign-in page itself, and the welcome page
  until the first admin exists - which asks for a one-time setup code that is only shown
  where Hemma was installed, so nobody else on the network can claim the admin account.
  Passwords are stored only as salted PBKDF2 hashes mixed with a server secret that is
  kept outside the database; recovery codes are stored as keyed hashes; sessions only as
  SHA-256 fingerprints.
- **Admins** manage people, library folders and uploads. **Members** use the household
  features. **Display** accounts (a wall tablet) can only look.

How accounts, recovery codes and resets work: [docs/accounts.md](docs/accounts.md).

# 0011 - An Android app: a thin window onto Hemma, served by Hemma itself

**Status:** accepted · **Since:** 2026-10-28 (Hemma 2.0.0)

## Context

Hemma is a web page on the home network (0001, 0004): any phone's browser opens it. Three things a
browser can't do well there:

- **Find Hemma.** A web page can't look around the home network, so everyone types an address the
  first time - and Android's Chrome installs a web app with its own window only from an `https://`
  site, which Hemma at home isn't (0004).
- **Feel like an app.** On plain http, "Add to Home screen" opens an ordinary browser tab.
- **Keep an audiobook going with the phone locked**, reliably: Chrome does it through the Media
  Session API (1.6), but a browser tab in the background is the first thing Android pauses.

The owner (2026-10-28): "a web-app pretty much, only allow local connection, or rather it tries to
find the local-network of the hemma application", sideloaded, not through a store, and a soft launch
worth a major version: 2.0 (nothing breaks - 1.0 was a milestone the same way).

## Decision

- **The app is a window onto Hemma's own pages** (a WebView), not a second user interface. The
  household's rules stay in C on the server (0001); the app adds only what a WebView can't do:
  finding Hemma, the lock screen's media controls for audiobooks, the system's file picker for
  uploads, downloads with the sign-in's cookie, a video's full screen, the Back key.
- **It finds Hemma by itself**: mDNS / DNS-SD (`_hemma._tcp`, announced by Avahi where the installer
  finds it) and, at the same time, by asking every address of the Wi-Fi (its /24, at most a /22) for
  `GET /hemma.json` - a public answer saying this is Hemma, its version, its id and the household's
  name (which the sign-in page shows anyway). The id lets the app find the same Hemma again when the
  router hands out a new address. An address can be typed too.
- **Home network only.** The app connects to private addresses alone (10/8, 172.16/12, 192.168/16,
  link-local, loopback; IPv6 link-local and unique-local) and to names a home network gives, once
  they lead to one; any other link opens in the phone's browser. Plain http, as Hemma is (0004).
- **Plain Java and Android's own tools - no Gradle, no libraries.** `apps/android/build.sh` (and
  `build.ps1`'s Build-App) runs aapt2, javac, d8, zipalign and apksigner from the Android SDK: the app
  is about 40 KB and builds in seconds, offline, with what Android Studio installs. Its address rules
  (Net.java) are plain Java, tested with a JDK alone (test/NetTest.java).
- **Hemma serves the app itself**: the build puts the signed APK into the program (like av.wasm) as
  `/static/hemma.apk`, and Your account offers "Get the app for Android" - the app always matches its
  Hemma, and nothing comes from a store or the internet. No Android tools at build time: no app (the
  card isn't shown).
- **Signed with the project's own key.** Android installs only signed apps, and an update only when it
  is signed with the same key: releases sign with a key kept as a GitHub secret
  (HEMMA_APP_KEYSTORE); a developer's own builds with a key made once on their computer (and
  debuggable). CI builds without the release key leave the app out rather than sign it with a key
  nobody keeps.

## Consequences

- The project has Java in it now, in `apps/android/` only - a deliberate exception to "everything in
  C", kept small: what the app does, wire.js could not.
- wire.js gains one hook: in the app (window.HemmaApp) the listener tells the app what plays and takes
  its lock-screen buttons back through window.hemmaAction (tests/browser/audiobook.spec.mjs).
- People install it from a file ("allow your browser to install apps", once). Google has announced
  developer verification for apps installed outside its store on certified Android devices; if it
  comes to the household's phones, the project's key gets registered then - the app stays the same.
- iPhones: an app needs a Mac with Xcode, and Apple's paid programme to give it to others. Not now:
  Safari plays audiobooks on the lock screen through the Media Session API, and "Add to Home Screen"
  gives the icon.

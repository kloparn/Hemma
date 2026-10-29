# The Hemma app for Android

Hemma is a web page, and any phone's browser opens it. The app is the same Hemma in a window of its
own, with three things a browser can't do as well:

- **It finds Hemma by itself** on the Wi-Fi you're on - no address to type.
- **It keeps you signed in** and opens straight to Hemma, like any app.
- **Audiobooks keep playing with the phone locked**, with the book's cover, the chapter, play and
  pause, 15 seconds back and 30 forward on the lock screen and in the notifications.

It only ever connects to Hemma on your home network. Away from home it waits until you're back.

## Getting it

The app comes from your own Hemma, not from a store:

1. On the Android phone, open Hemma in the browser and sign in.
2. **Household → Your account → Hemma on your phone → Get the app for Android.**
3. Open the downloaded file. Android asks whether the browser may install apps: allow it, this once.
   It may also say the app is from an unknown developer (it isn't from the Play Store): choose
   **Install anyway**.
4. Open **Hemma**. Android 17 and newer asks whether Hemma may *find and connect to nearby devices*:
   allow it - that's how the app reaches Hemma on your home network.

The app looks around the Wi-Fi and opens Hemma when it finds it. If it doesn't, type the address
(the **Screens** page in Hemma shows it, like `192.168.1.50`).

A new Hemma version brings a new app: **Get the app for Android** again installs it over the old one.

No "Get the app" button? Your Hemma was built without Android's tools: the releases from 2.0 on have
it. On an **iPhone**, there's no app: open Hemma in Safari, tap **Share**, then **Add to Home Screen** -
audiobooks show on the lock screen there too.

## How it finds Hemma

Two ways at once:

- Hemma announces itself on the network as a *Hemma* service (mDNS, the same thing that makes
  `hemma.local` names work). On a Raspberry Pi the installer sets that up, when Avahi is there.
- The app asks every address of your Wi-Fi whether it's Hemma (`GET /hemma.json` on ports 80 and
  8080) - a few seconds, and it works whatever runs Hemma.

It remembers which Hemma it found (by an id of its own, not the address), so when the router hands
Hemma a new address the app finds it there again.

## For developers

The app is `apps/android/`: plain Java on Android's own APIs, no libraries, built without Gradle by
`apps/android/build.sh` (or `./build.ps1 app` on Windows) with the Android SDK's aapt2, d8, zipalign
and apksigner and a JDK - what Android Studio installs. Why it's built this way:
[decision 0011](decisions/0011-an-android-app-as-a-thin-window.md).

```sh
./build.ps1 app                     # Windows -> build/hemma.apk (ANDROID_HOME / JAVA_HOME, else Android Studio's)
make app                            # Linux / macOS (ANDROID_HOME and a JDK on PATH)
adb install -r build/hemma.apk      # onto a phone with USB debugging on
```

- A normal build of Hemma builds the app too when it finds those tools, and puts it into the program
  as `/static/hemma.apk` (the "Get the app" card). Without them there's no app and no card.
- **The key.** Android installs an update only when it's signed with the same key as the app it
  replaces. Your own builds sign with a key made once in `~/.hemma/hemma-app-dev.p12`, and are
  debuggable: `chrome://inspect` on your computer shows the app's pages. Releases sign with the
  project's key (`HEMMA_APP_KEYSTORE` + `HEMMA_APP_KEY_PASSWORD`, a GitHub secret); a CI build without
  it leaves the app out.
- What the app adds lives in four files: `Net.java` (which addresses are the home network; tested by
  `test/NetTest.java` with a JDK alone), `Finder.java` (looking around), `MainActivity.java` (the
  window, uploads, downloads, full screen, Back), `PlayerService.java` (the lock screen). wire.js tells
  the app what plays through `window.HemmaApp.media()` and takes its buttons back through
  `window.hemmaAction()` (tests/browser/audiobook.spec.mjs).

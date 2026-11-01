# The Hemma app for Android

Hemma is a web page, and any phone's browser opens it. The app is the same Hemma in a window of its
own, with four things a browser can't do as well:

- **It finds Hemma by itself** on the Wi-Fi you're on - no address to type.
- **It keeps you signed in** and opens straight to Hemma, like any app.
- **Audiobooks keep playing with the phone locked**, with the book's cover, the chapter, play and
  pause, 15 seconds back and 30 forward on the lock screen and in the notifications.
- **Videos and audiobooks for offline** (from 2.3): save them at home, watch and listen anywhere.

It only ever connects to Hemma on your home network. Away from home it waits until you're back - and
shows what you saved for offline meanwhile.

## Offline

1. At home, open the video, the series or the audiobook in the app and press **Save for offline**
   (a series has it per season, next to *Download*). A notification shows how far it has got; it carries
   on if the Wi-Fi drops, and when you open the app again.
2. Away from home, open the app: its start screen has **Offline** with what you saved. At home it's
   under **Watch → Offline**, and **Household → Your account → Saved for offline**.
3. **Play** plays it from where you got to. Videos come in the language you listen to (see *Sound
   language* in Watch's Library settings) and with their subtitles; audiobooks with their chapters, the
   lock screen's controls, 15 seconds back and 30 forward.
4. Back home, the app tells Hemma where you got to, so *Continue watching* and your books carry on there - and
   the other way round (from 2.7.1): watch further on the TV or in a browser, and the saved copy starts there
   too. Whichever place is newer wins. The app catches up whenever it's at home: when it finds Hemma, when you
   put it away, and when you open the Offline screen - so open the app once before you leave.

Saved things stay on the phone until you **Delete** them on the Offline screen (or uninstall the app).
A film takes about as much room as its file in Hemma - a gigabyte or more for a long one in HD - and the
Offline screen shows how much room the phone has left. Grown-ups and children can save (children only
what's open to them); guests and the TV's or the wall's display accounts can't.

## Getting it

The app comes from your own Hemma, not from a store:

1. On the Android phone, open Hemma in the browser and sign in.
2. **Get the app** on the card at the top of Home (*Hemma has an app for your phone*). If you picked *Not now*,
   it's always under **Household → Your account → Hemma on your phone → Get the app for Android.**
3. Open the downloaded file. Android asks whether the browser may install apps: allow it, this once.
   It may also say the app is from an unknown developer (it isn't from the Play Store): choose
   **Install anyway**.
4. Open **Hemma**. Android 17 and newer asks whether Hemma may *find and connect to nearby devices*:
   allow it - that's how the app reaches Hemma on your home network.

The app looks around the Wi-Fi and opens Hemma when it finds it. If it doesn't, type the address
(the **Screens** page in Hemma shows it, like `192.168.1.50`).

A new Hemma version brings a new app: **Get the app for Android** again installs it over the old one.

Once you've signed in from the app, Hemma in the phone's browser says **Open the app instead!** on Home: the
button opens the app (from 2.2; on a phone without it, or with an older app, it downloads the app instead).

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

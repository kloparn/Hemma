# Converting on your graphics card

Hemma can make an H.264 copy of an HEVC (x265) video on the computer you're using: *Library settings → Converting →
Play everywhere from HEVC → On this device* (also on a series' Manage page and a video's own page). From 2.8 the
browser hands the heavy part, the picture, to that computer's **graphics card** when it can - Hemma's own
converter still reads the file, makes the sound and the subtitles and writes the copy.

On a PC with a GeForce RTX 3060, four 23-minute 1080p episodes (HEVC 10-bit) took **5 minutes 22 seconds** on the
card - about 17 times faster than watching them - with the processor a quarter busy. All twenty threads of its
processor took 17 minutes 47 seconds for the same four. The card's copies came out a third smaller than the
processor's, and as good to the eye (46.3 dB PSNR against the originals; the processor's 47.1).

## The one step it needs

Browsers only let a page use the graphics card's video coders on a **secure address**: `https://...`, or the
computer's own address, `127.0.0.1` or `localhost`. Hemma's address on your network (`http://192.168.1.50`) isn't
one. So on the computer that converts, open Hemma through a forwarded port on the computer itself:

**Windows** - in PowerShell as administrator (put your Hemma's address in place of `192.168.1.50`):

```powershell
netsh interface portproxy add v4tov4 listenaddress=127.0.0.1 listenport=8099 connectaddress=192.168.1.50 connectport=80
```

**Linux or macOS** - while you convert:

```sh
socat TCP-LISTEN:8099,bind=127.0.0.1,reuseaddr,fork TCP:192.168.1.50:80
```

Then open **http://127.0.0.1:8099** in Chrome or Edge on that computer and sign in (it's a new address to the
browser, so once more). It's the same Hemma - only this computer's browser sees it as a secure page.

*Library settings → Converting → How hard to work → This device* says which it is: "Its graphics card makes HEVC
videos H.264 here" when the card is at work, or how to let it when the page isn't secure. Other devices - phones,
the TV - carry on with Hemma's usual address.

To take the forward away again on Windows:

```powershell
netsh interface portproxy delete v4tov4 listenaddress=127.0.0.1 listenport=8099
```

## What it needs

- **A recent Chrome or Edge** - it takes WebCodecs and WebAssembly's JSPI. Firefox and Safari convert on the
  processor as before.
- **A graphics card that decodes HEVC and encodes H.264** - most recent ones do. The browser asks the card first;
  one that can't is simply not used. (Tried: an NVIDIA GeForce RTX 3060 on Windows 11 with Chrome 156.)

## How it goes about it

- Three videos at a time on the card. The processor reads the files, makes the sound (Dolby and DTS become AAC) and
  the subtitles and writes the copies - a few percent of it.
- The copies are H.264 High at the same quality setting as the processor's (QP 23), with a keyframe every two
  seconds, so they seek as well.
- A video the card can't take (a size or kind it doesn't do) is made by the processor, as without a card. One where
  the card stops part-way is made again by the processor, and Hemma's log says why
  (`upload: ... made by adam's device without its graphics card, which stopped: ...`). Two such stops and that
  batch's other videos go to the processor too.
- Hemma's log says each copy the card made (`upload: ... made on the graphics card of adam's device`).
- On the card, nothing is split into parts: *Full throttle* splits a long video between cores, but the card is
  quicker with whole videos.

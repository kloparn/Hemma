# Hemma on Windows

A Windows 10 or 11 PC that stays on can be the household's Hemma server. Windows is also
where Hemma is developed, so this is the best-trodden path for building it, including
building the files for a Raspberry Pi, Linux or a Mac.

All commands are for **PowerShell** (Start menu → type *PowerShell*). Some steps need
**PowerShell as administrator** (right-click → *Run as administrator*). Those are marked.

There's no ready-made Hemma for Windows to download yet ([why](#if-windows-security-removes-hemmaexe)):
steps 1 to 3 build it from its source code, which takes a few minutes. To try it before setting
it up, start the program you built with `.\build\hemma.exe --demo`: a made-up household, in a
throwaway folder; the login shows in its window.

## 1. Install zig

Hemma is C code, and [zig](https://ziglang.org) is the compiler that turns it into a
program. One command installs it:

```powershell
winget install zig.zig
```

Close and reopen PowerShell afterwards. (`build.ps1` also finds zig on its own if the
terminal doesn't know it yet.)

## 2. Get the source code

On [the project page](https://github.com/kloparn/Hemma), choose **Code -> Download ZIP** and unzip it, for example to
`C:\Users\you\hemma`. Or, with [Git](https://git-scm.com) (`winget install Git.Git`):

```powershell
git clone https://github.com/kloparn/Hemma.git hemma
```

## 3. Build

In PowerShell, go to the folder and build the optimised program:

```powershell
cd C:\Users\you\hemma
.\build.ps1 release          # -> build\hemma.exe
```

The first build takes a minute or two, because the database engine (SQLite) is compiled
once. If PowerShell says *running scripts is disabled on this system*, run it like this
instead:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 release
```

The same script builds for other computers too:

| Command | Result | For |
|---|---|---|
| `.\build.ps1 release` | `build\hemma.exe` | this PC |
| `.\build.ps1 pi` | `build\hemma-pi64` | Raspberry Pi (64-bit OS), any 64-bit ARM Linux |
| `.\build.ps1 pi32` | `build\hemma-pi32` | Raspberry Pi (32-bit OS), any 32-bit ARM Linux |
| `.\build.ps1 linux` | `build\hemma-linux` | Linux on Intel/AMD (x86-64) |
| `.\build.ps1 mac` | `build\hemma-mac-arm64`, `build\hemma-mac-x64` | Macs with Apple silicon / Intel |

Each one is a single file with no dependencies.

### If Windows Security removes hemma.exe

Windows Security (Defender) sometimes calls a freshly built `hemma.exe` a threat and removes it
("Threats found", often naming *CobaltStrike*). That's a false alarm: its machine learning
guesses from how a program looks, and a new, unsigned program with its own web server and
network scanning looks like the tools it watches for. It's also why there's no Windows download
yet. For a `hemma.exe` you built yourself from this source: *Windows Security → Virus & threat
protection → Protection history*, open the entry, then **Actions → Allow on device**. Don't do
that for a `hemma.exe` from anywhere else.

## 4. Set it up in its own folder

Keep the program, its settings and its database together, for example in `C:\Hemma`:

```powershell
mkdir C:\Hemma
copy build\hemma.exe C:\Hemma\
cd C:\Hemma
```

On its first start Hemma creates the settings file `hemma.env` there, with a fresh secret
that every password depends on. Keep a copy of it somewhere safe, apart from the data
folder. (On a PC that other people also sign in to, make `C:\Hemma` private: right-click
→ Properties → Security, and remove "Users".) Other settings you can add are listed in [.env.example](../.env.example).

Now start it:

```powershell
C:\Hemma\hemma.exe --data C:\Hemma\data --env C:\Hemma\hemma.env
```

It prints where it's listening, for example `Hemma is up on http://192.168.1.50:8080`.
That address is the one other devices use. Open it, or `http://localhost:8080` on the PC
itself, and **create the admin account**. The welcome page asks for the **setup code**
Hemma printed when it started (`The welcome page asks for this setup code: 7KQ4-M2XP`)
in its window. More in [accounts.md](accounts.md). Stop Hemma with Ctrl+C.

### The firewall question

The first time Hemma starts, Windows asks whether *hemma.exe* may communicate on
networks. Tick **Private networks** and press **Allow access**. Without that, only the PC
itself can open Hemma.

- Your home network must be set to **Private**: *Settings → Network & internet →
  Wi-Fi* (or *Ethernet*) → your network → *Network profile type: Private network*.
- Pressed *Cancel* by mistake? Windows then blocks it. Open *Control Panel → Windows
  Defender Firewall → Allow an app through Windows Firewall*, find *hemma.exe* and tick
  *Private*.
- Or add the rule yourself (**as administrator**):

  ```powershell
  New-NetFirewallRule -DisplayName "Hemma" -Direction Inbound -Program "C:\Hemma\hemma.exe" -Action Allow -Profile Private
  ```

Firewall rules belong to the program's location, so `build\hemma.exe` and
`C:\Hemma\hemma.exe` each need their own.

Hemma has to listen on the network, not just on the PC itself. Don't start it with
`--host 127.0.0.1` (that's what `.\build.ps1 run` does, because it's meant for
development).

## 5. Start Hemma with Windows

Task Scheduler can start Hemma when the PC starts, before anyone signs in. Paste this
into PowerShell **as administrator**. It runs Hemma in the background and restarts it if
it stops. Hemma runs as Windows' own limited *Local Service* account, which may change
`C:\Hemma` and nothing else on the PC: if Hemma ever had a security hole, that's all it
could reach.

```powershell
# Windows' "Local Service" account, whatever it's called in your Windows language
$svc = (New-Object System.Security.Principal.SecurityIdentifier "S-1-5-19").Translate([System.Security.Principal.NTAccount]).Value
icacls C:\Hemma /grant "*S-1-5-19:(OI)(CI)M" /T     # it may change C:\Hemma (settings, database, log)
$action = New-ScheduledTaskAction -Execute "C:\Hemma\hemma.exe" -WorkingDirectory "C:\Hemma" `
    -Argument "--data C:\Hemma\data --env C:\Hemma\hemma.env"
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal -UserId $svc -LogonType ServiceAccount -RunLevel Limited
Register-ScheduledTask -TaskName "Hemma" -Action $action -Trigger $trigger -Settings $settings -Principal $principal
New-NetFirewallRule -DisplayName "Hemma" -Direction Inbound -Program "C:\Hemma\hemma.exe" -Action Allow -Profile Private
Start-ScheduledTask -TaskName "Hemma"
```

(`ExecutionTimeLimit` zero matters: by default Windows stops scheduled tasks after
three days.) The task runs without a window, so no firewall question pops up. That's why
the script adds the rule itself.

**Set it up with an older version of this guide?** That task ran Hemma as *SYSTEM*, with
power over the whole PC. Stop it and remove it first (`Unregister-ScheduledTask -TaskName
"Hemma" -Confirm:$false`), then run the lines above, and give it your movie folders (next
paragraph but one).

Hemma keeps its own log in `C:\Hemma\data\hemma.log` (it starts a new one at about 1 MB and
keeps one older file, `hemma.log.1`), and an admin sees it on Hemma's **System** page.
Something not working? See [troubleshooting.md](troubleshooting.md).

Day to day (as administrator):

```powershell
Get-Content C:\Hemma\data\hemma.log -Tail 20 -Wait     # the log (Ctrl+C to stop watching)
Stop-ScheduledTask -TaskName "Hemma"; Stop-Process -Name hemma -ErrorAction SilentlyContinue
Start-ScheduledTask -TaskName "Hemma"
Unregister-ScheduledTask -TaskName "Hemma"         # remove the autostart again
```

**Updating:** stop it (above), copy the new `hemma.exe` over `C:\Hemma\hemma.exe`
(Windows can't replace a running program), then start it again.

**Movies:** in Hemma, open **Watch → Library settings** and enter folders such as
`D:\Movies`, one per line. Hemma runs as *Local Service* (above), so let it into each of
those folders, as administrator:

```powershell
icacls "D:\Movies" /grant "*S-1-5-19:(OI)(CI)M" /T    # M: watch, upload, convert and remove
icacls "D:\Movies" /grant "*S-1-5-19:(OI)(CI)RX" /T   # ...or RX: only watch
```

A task started at boot can't see *mapped* network drives (like `Z:`), so keep movies on a
local or USB disk.

**Backups:** Hemma backs up its database (`C:\Hemma\data\hemma.db`) every night by
itself, keeps the last 7 days, and can go back to one: **System → Backups**. They go to a
hidden `.hemma-backups` folder in your first library folder on another disk, else
`C:\Hemma\data\backups`. **Download** there gives a copy with the password secret, to
keep elsewhere. The [Pi guide](raspberry-pi.md#backups) has the details, and how to put a
backup back on another computer (`hemma --restore`).

## 6. Keep the PC awake

A sleeping PC can't answer phones. Turn sleep off while plugged in:

- *Settings → System → Power* (*& battery*) *→ Screen and sleep*: set **"When plugged
  in, put my device to sleep after"** to **Never**. The screen may still turn off; that's
  fine.
- Or, in PowerShell:

  ```powershell
  powercfg /change standby-timeout-ac 0
  powercfg /change hibernate-timeout-ac 0
  ```

- On a laptop that should run with the lid closed (**as administrator**):

  ```powershell
  powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS LIDACTION 0
  powercfg /setactive SCHEME_CURRENT
  ```

Windows Update restarts the PC now and then. The scheduled task starts Hemma again
by itself.

## For developers

```powershell
.\build.ps1 run          # debug build, http://127.0.0.1:8080, web/ files served live
.\build.ps1 test         # the test suite
```

See [development.md](development.md).

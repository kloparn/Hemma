<#
  build.ps1 - build, test and run Hemma on Windows with `zig cc`.

    ./build.ps1                 debug build          -> build/hemma.exe
    ./build.ps1 run [args]      debug build + run on http://127.0.0.1:8080 (web/ + module CSS served live)
    ./build.ps1 test [filter]   build + run tests    (one process per core; a filter = substring of a test
                                or file name, run in one process)
    ./build.ps1 ci [job]        CI's Linux jobs in WSL (tools/ci/local.sh): test | asan | fuzz [seconds] [conv|mkv|mp4|ac3|dts|mpeg2|mpeg4|hevc|pgs|vobsub|tags]
    ./build.ps1 release         optimised build      -> build/hemma.exe
    ./build.ps1 pi              Raspberry Pi 64-bit  -> build/hemma-pi64   (static, aarch64)
    ./build.ps1 pi32            Raspberry Pi 32-bit  -> build/hemma-pi32   (static, armv6: every Pi)
    ./build.ps1 linux           Linux x86_64         -> build/hemma-linux  (static; pi/pi32 run on any ARM Linux)
    ./build.ps1 mac             macOS 11+            -> build/hemma-mac-arm64 (Apple silicon), build/hemma-mac-x64 (Intel)
    ./build.ps1 app             the Android app      -> build/hemma.apk (built into the program too, when the
                                Android SDK and a JDK are found: see Build-App)
    ./build.ps1 all             tests + every target
    ./build.ps1 clean

  $env:HEMMA_SKIP_MODULES = "media,tv"   leaves those module folders out of the build
  $env:HEMMA_BUILD_DIR = "build-book"    private output folder (parallel workers); SQLite objects stay shared
  $env:HEMMA_TEST_JOBS = "4"             test processes (default: one per core)
  $env:HEMMA_WSL = "Ubuntu-24.04"        the WSL distribution for ci (default: WSL's default)
#>
param(
    [Parameter(Position = 0)][string]$Cmd = "build",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Find-Zig {
    $c = Get-Command zig -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\zig.zig_*\*\zig.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($p) { return $p.FullName }
    throw "zig not found - install it with: winget install zig.zig"
}
$Zig = Find-Zig
$B = if ($env:HEMMA_BUILD_DIR) { $env:HEMMA_BUILD_DIR.TrimEnd('/', '\') } else { "build" }

$Flags = @("-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-Icore", "-Ithird_party/sqlite")
# The commit a binary is built from, for `hemma --version` ("-dirty": with uncommitted changes).
# Nothing outside a git checkout.
$Commit = ""
try {
    $c = & git rev-parse --short=12 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $c) {
        $Commit = "$c".Trim()
        if (& git status --porcelain --untracked-files=no 2>$null) { $Commit += "-dirty" }
    }
} catch { $Commit = "" }
if ($Commit) { $Flags += "-DHEMMA_COMMIT=$Commit" }
# The version: the VERSION file, MAJOR.MINOR.PATCH (semver.org)
$Version = if (Test-Path VERSION) { (Get-Content -Raw VERSION).Trim() } else { "" }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must hold MAJOR.MINOR.PATCH (semver.org), like 0.14.0 - it says '$Version'" }
$Flags += "-DHEMMA_VER=$Version"
$Debug = @("-O0", "-g", "-fsanitize=undefined", "-fsanitize-trap=undefined")
# release builds leave unused code and unwind tables out (the linker side is per target, below)
$Release = @("-O2", "-DNDEBUG", "-s", "-ffunction-sections", "-fdata-sections", "-fno-asynchronous-unwind-tables")
$WinLibs = @("-lws2_32", "-liphlpapi", "-lbcrypt")
# SQLite for size: its data is read into memory once, so -Os costs nothing you'd notice
$SqliteFlags = @("-Os", "-w", "-DSQLITE_DQS=0", "-DSQLITE_THREADSAFE=1", "-DSQLITE_DEFAULT_MEMSTATUS=0",
    "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1", "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS", "-DSQLITE_MAX_EXPR_DEPTH=0",
    "-DSQLITE_OMIT_DEPRECATED", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OMIT_SHARED_CACHE", "-DSQLITE_USE_ALLOCA",
    "-DSQLITE_ENABLE_FTS5", "-DSQLITE_ENABLE_MATH_FUNCTIONS", "-DSQLITE_OMIT_JSON", "-DSQLITE_ENABLE_DBPAGE_VTAB",
    "-ffunction-sections", "-fdata-sections")
$Targets = @{
    win   = @{ Out = "$B/hemma.exe"; Target = @(); Windows = $true }
    pi    = @{ Out = "$B/hemma-pi64"; Target = @("-target", "aarch64-linux-musl"); Windows = $false }
    pi32  = @{ Out = "$B/hemma-pi32"; Target = @("-target", "arm-linux-musleabihf", "-mcpu=arm1176jzf_s"); Windows = $false }
    linux = @{ Out = "$B/hemma-linux"; Target = @("-target", "x86_64-linux-musl"); Windows = $false }
    # macOS: links only libSystem; zig ad-hoc signs the arm64 binary (Apple silicon refuses unsigned ones)
    "mac-arm64" = @{ Out = "$B/hemma-mac-arm64"; Target = @("-target", "aarch64-macos.11.0"); Windows = $false }
    "mac-x64"   = @{ Out = "$B/hemma-mac-x64"; Target = @("-target", "x86_64-macos.11.0"); Windows = $false }
}

function Rel([string]$full) { return $full.Substring($PSScriptRoot.Length + 1).Replace('\', '/') }

$Skip = @(($env:HEMMA_SKIP_MODULES -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Skip -contains "av") { $Skip += "media" }       # media converts with av
if ($Skip -contains "media") { $Skip += "tv" }      # tv plays media's library
function Get-ModuleDirs {
    return @(Get-ChildItem modules -Directory -ErrorAction SilentlyContinue | Where-Object { $Skip -notcontains $_.Name })
}

function Get-Sources([bool]$WithTests) {
    $all = @(Get-ChildItem core -Filter *.c | ForEach-Object { Rel $_.FullName })
    foreach ($d in Get-ModuleDirs) { $all += @(Get-ChildItem $d.FullName -Filter *.c | ForEach-Object { Rel $_.FullName }) }
    $src = @($all | Where-Object { $_ -notmatch '_test\.c$' })
    if (-not $WithTests) { return $src + @("$B/assets.c") }
    $src = @($src | Where-Object { $_ -ne "core/main.c" })
    $tests = @($all | Where-Object { $_ -match '_test\.c$' })
    return $src + $tests + @("tests/runner.c", "tests/harness.c", "$B/assets.c")
}

function Invoke-Zig([string[]]$ZigArgs) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Zig @ZigArgs
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { throw "zig $($ZigArgs[0]) failed (exit $code)" }
}

function Build-Assets {
    New-Item -ItemType Directory -Force $B | Out-Null
    $embed = "$B/embed.exe"
    if (-not (Test-Path $embed) -or (Get-Item tools/embed.c).LastWriteTime -gt (Get-Item $embed).LastWriteTime) {
        Invoke-Zig @("cc", "-O1", "tools/embed.c", "-o", $embed)
    }
    # one stylesheet: web/app.css followed by every module's CSS
    $css = @("web/app.css") + @(Get-ModuleDirs | ForEach-Object { Get-ChildItem $_.FullName -Filter *.css } |
            Sort-Object FullName | ForEach-Object { Rel $_.FullName })
    $bundle = ($css | ForEach-Object { "/* ---- $_ ---- */`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot $_)) }) -join "`n"
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot "$B/app.css"), $bundle)
    $files = @(Get-ChildItem web -Recurse -File | Where-Object { $_.Name -ne "app.css" -and $_.Name -notlike ".*" } |
            ForEach-Object { Rel $_.FullName })
    # what gets served: published path -> file
    $served = @{ "/app.css" = "$B/app.css" }
    foreach ($f in $files) { $served["/" + $f.Substring(4)] = $f }
    if ($Skip -notcontains "av") { $served["/av.wasm"] = Build-Wasm }
    $apk = Build-App $false
    if ($apk) { $served["/hemma.apk"] = $apk }
    # a gzipped twin of each text file and of av.wasm ("/app.css.gz"): served to browsers that
    # accept it - a first page load shrinks from ~120 KB to ~30 KB
    New-Item -ItemType Directory -Force "$B/gz" | Out-Null
    $twins = @()
    $paths = @($served.Keys | Sort-Object) # a stable order: the asset version hashes them in order
    foreach ($p in $paths) {
        if ($p -notmatch '\.(css|js|svg|webmanifest|wasm)$') { continue }
        $raw = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $served[$p]))
        $ms = New-Object IO.MemoryStream
        $gz = New-Object IO.Compression.GZipStream($ms, [IO.Compression.CompressionLevel]::Optimal)
        $gz.Write($raw, 0, $raw.Length)
        $gz.Close()
        $packed = $ms.ToArray()
        if ($packed.Length -ge $raw.Length * 0.9) { continue } # not worth it
        $out = "$B/gz" + $p + ".gz"
        [IO.File]::WriteAllBytes((Join-Path $PSScriptRoot $out), $packed)
        $twins += "$p.gz=$out"
    }
    $list = @($paths | ForEach-Object { "$_=$($served[$_])" }) + $twins
    & "./$embed" "$B/assets.c" web @list
    if ($LASTEXITCODE -ne 0) { throw "embedding web assets failed" }
}

# The converter for browsers (modules/av/wasm/avconv.c + the av sources it uses) -> $B/av.wasm,
# served as /static/av.wasm: uploads convert on the sending device (see media/upload.c).
$WasmSources = @("modules/av/wasm/avconv.c") + @("conv", "mkv", "mpeg2", "mpeg4", "hevc", "hevc_ctu", "hevc_dsp", "deint", "h264enc", "ac3", "dts", "aac", "mp4" |
        ForEach-Object { "modules/av/$_.c" })
function Build-Wasm {
    $out = "$B/av.wasm"
    $newest = ($WasmSources + @(Get-ChildItem modules/av -Filter *.h | ForEach-Object { Rel $_.FullName }) |
        ForEach-Object { (Get-Item $_).LastWriteTime } | Measure-Object -Maximum).Maximum
    if (-not (Test-Path $out) -or $newest -gt (Get-Item $out).LastWriteTime) {
        Invoke-Zig (@("cc", "-std=gnu11", "-target", "wasm32-wasi", "-mexec-model=reactor", "-msimd128", "-O2", "-DNDEBUG",
                "-s", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                "-Wl,-z,stack-size=8388608", "-Imodules/av") + $WasmSources + @("-o", $out))
    }
    return $out
}

# The Hemma app for Android (apps/android, 2.0; docs/decisions/0011) -> $B/hemma.apk, built into the program as
# /static/hemma.apk (Your account -> "Get the app for Android"). Built when Android's tools are here - an SDK
# (ANDROID_HOME, else Android Studio's %LOCALAPPDATA%\Android\Sdk) and a JDK (JAVA_HOME, else Android Studio's own)
# - else left out, like av.wasm without zig. The same steps as apps/android/build.sh (its header: the key). In CI
# only with the release key (HEMMA_APP_KEYSTORE): one made up there would make later versions refuse to install.
function Find-AndroidTools {
    $sdk = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "$env:LOCALAPPDATA\Android\Sdk") | Where-Object { $_ -and (Test-Path $_) } |
        Select-Object -First 1
    $jdk = @($env:JAVA_HOME, "$env:ProgramFiles\Android\Android Studio\jbr") | Where-Object { $_ -and (Test-Path "$_\bin\javac.exe") } |
        Select-Object -First 1
    if (-not $sdk -or -not $jdk) { return $null }
    $bt = Get-ChildItem "$sdk\build-tools" -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path "$($_.FullName)\aapt2.exe" } |
        Sort-Object { [version]($_.Name -replace '[^0-9.].*$', '') } | Select-Object -Last 1
    $plat = Get-ChildItem "$sdk\platforms" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^android-(\d+)' } |
        Sort-Object { [int]($_.Name -replace '^android-(\d+).*$', '$1') } | Select-Object -Last 1
    if (-not $bt -or -not $plat -or -not (Test-Path "$($plat.FullName)\android.jar")) { return $null }
    return @{ Jdk = $jdk; BT = $bt.FullName; Jar = "$($plat.FullName)\android.jar" }
}

function Invoke-Tool([string]$Exe, [string[]]$ToolArgs) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Exe @ToolArgs 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { $out | ForEach-Object { Write-Host $_ }; throw "$(Split-Path $Exe -Leaf) failed (exit $code)" }
    return $out
}

function Build-App([bool]$Force) {
    if ($Skip -contains "app") { return "" }
    $t = Find-AndroidTools
    if (-not $t) { if ($Force) { throw "Android's tools weren't found (ANDROID_HOME, JAVA_HOME)" }; return "" }
    if ($env:CI -and -not $env:HEMMA_APP_KEYSTORE -and -not $Force) { return "" }
    $apk = "$B/hemma.apk"
    $newest = (@(Get-ChildItem apps/android -Recurse -File) + @(Get-Item VERSION) | Measure-Object -Property LastWriteTime -Maximum).Maximum
    if (-not $Force -and (Test-Path $apk) -and (Get-Item $apk).LastWriteTime -ge $newest) { return $apk }
    $out = "$B/app"
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    foreach ($d in "test", "gen", "classes") { New-Item -ItemType Directory -Force "$out/$d" | Out-Null }
    $jb = "$($t.Jdk)\bin"
    $savedHome = $env:JAVA_HOME
    $env:JAVA_HOME = $t.Jdk # (d8.bat and apksigner.bat run Java through it)
    try {
        # 1. the address rules and what's saved for offline (2.3), with a JDK alone
        $net = "apps/android/src/io/github/kloparn/hemma/Net.java"
        $saved = "apps/android/src/io/github/kloparn/hemma/Saved.java"
        Invoke-Tool "$jb\javac.exe" @("-encoding", "UTF-8", "-d", "$out/test", $net, "apps/android/test/NetTest.java",
                $saved, "apps/android/test/SavedTest.java") | Out-Null
        foreach ($test in "NetTest", "SavedTest") {
            Invoke-Tool "$jb\java.exe" @("-cp", "$out/test", $test) | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray }
        }
        # 2. the key: the release key (HEMMA_APP_KEYSTORE + HEMMA_APP_KEY_PASSWORD), else this developer's own
        if ($env:HEMMA_APP_KEYSTORE) {
            $ks = $env:HEMMA_APP_KEYSTORE; $pw = $env:HEMMA_APP_KEY_PASSWORD; $debugMode = @()
            if (-not $pw) { throw "HEMMA_APP_KEY_PASSWORD isn't set" }
        } else {
            $ks = "$env:USERPROFILE\.hemma\hemma-app-dev.p12"; $pw = "hemma-dev"; $debugMode = @("--debug-mode")
            if (-not (Test-Path $ks)) {
                New-Item -ItemType Directory -Force (Split-Path $ks) | Out-Null
                Invoke-Tool "$jb\keytool.exe" @("-genkeypair", "-keystore", $ks, "-storetype", "PKCS12", "-storepass", $pw, "-keypass", $pw,
                    "-alias", "hemma", "-keyalg", "RSA", "-keysize", "3072", "-validity", "10000", "-dname", "CN=Hemma developer build") | Out-Null
            }
        }
        # 3. resources + manifest -> base.apk (and R.java)
        $v = [version]$Version
        $code = $v.Major * 10000 + $v.Minor * 100 + $v.Build
        Invoke-Tool "$($t.BT)\aapt2.exe" @("compile", "--dir", "apps/android/res", "-o", "$out/res.zip") | Out-Null
        Invoke-Tool "$($t.BT)\aapt2.exe" (@("link", "-I", $t.Jar, "--manifest", "apps/android/AndroidManifest.xml",
                "--min-sdk-version", "26", "--target-sdk-version", "36", "--version-code", "$code", "--version-name", $Version,
                "--java", "$out/gen") + $debugMode + @("-o", "$out/base.apk", "$out/res.zip")) | Out-Null
        # 4. Java -> classes.dex
        $src = @(Get-ChildItem apps/android/src, "$out/gen" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
        $boot = "$($t.Jar);$($t.BT)\core-lambda-stubs.jar" # (a lambda needs LambdaMetafactory, which d8 then removes)
        Invoke-Tool "$jb\javac.exe" (@("-source", "8", "-target", "8", "-bootclasspath", $boot, "-Xlint:-options", "-encoding", "UTF-8",
                "-d", "$out/classes") + $src) | Out-Null
        $classes = @(Get-ChildItem "$out/classes" -Recurse -Filter *.class | ForEach-Object { $_.FullName })
        Invoke-Tool "$($t.BT)\d8.bat" (@("--release", "--min-api", "26", "--lib", $t.Jar, "--output", $out) + $classes) | Out-Null
        # 5. classes.dex into the APK, aligned, signed
        Push-Location $out
        try { Invoke-Tool "$($t.BT)\aapt.exe" @("add", "base.apk", "classes.dex") | Out-Null } finally { Pop-Location }
        Invoke-Tool "$($t.BT)\zipalign.exe" @("-f", "-p", "4", "$out/base.apk", "$out/aligned.apk") | Out-Null
        Invoke-Tool "$($t.BT)\apksigner.bat" @("sign", "--ks", $ks, "--ks-pass", "pass:$pw", "--out", $apk, "$out/aligned.apk") | Out-Null
        Remove-Item "$apk.idsig" -ErrorAction SilentlyContinue
    } finally { $env:JAVA_HOME = $savedHome }
    $size = [math]::Round((Get-Item $apk).Length / 1KB)
    Write-Host "built $apk ($size KB, $Version)" -ForegroundColor Green
    return $apk
}

# SQLite and its recovery extension (third_party/sqlite/recover.c): one object each, built again when
# its source or the flags change
function Build-Sqlite([string]$Name, [string[]]$Target) {
    $objs = @()
    foreach ($src in @("sqlite3", "recover")) {
        $obj = "build/obj/$Name/$src.o"
        $stamp = "$obj.flags" # the flags it was built with: new ones mean building it again
        $flags = ($Target + $SqliteFlags) -join " "
        $same = (Test-Path $stamp) -and ([IO.File]::ReadAllText((Join-Path $PSScriptRoot $stamp)) -eq $flags)
        $deps = if ($src -eq "sqlite3") { @("third_party/sqlite/sqlite3.c") } else {
            @("third_party/sqlite/recover.c", "third_party/sqlite/dbdata.c", "third_party/sqlite/sqlite3recover.c") }
        $newer = $deps | Where-Object { (Test-Path $obj) -and (Get-Item $_).LastWriteTime -gt (Get-Item $obj).LastWriteTime }
        if (-not (Test-Path $obj) -or -not $same -or $newer) {
            New-Item -ItemType Directory -Force "build/obj/$Name" | Out-Null
            Write-Host "compiling SQLite ($src) for $Name (one-time, ~1 min)..." -ForegroundColor DarkGray
            Invoke-Zig (@("cc", "-c") + $Target + $SqliteFlags + @("third_party/sqlite/$src.c", "-o", $obj))
            [IO.File]::WriteAllText((Join-Path $PSScriptRoot $stamp), $flags)
        }
        $objs += $obj
    }
    return $objs
}

function Build-Target([string]$Name, [string[]]$Mode) {
    $t = $Targets[$Name]
    Build-Assets
    $sqlite = Build-Sqlite $Name $t.Target
    $a = @("cc") + $t.Target + $Flags + $Mode + (Get-Sources $false) + $sqlite + @("-o", $t.Out)
    if ($Mode -contains "-DNDEBUG") { $a += $(if ($Name -like "mac-*") { "-Wl,-dead_strip" } else { "-Wl,--gc-sections" }) }
    if ($t.Windows) { $a += $WinLibs }
    Invoke-Zig $a
    $size = [math]::Round((Get-Item $t.Out).Length / 1KB)
    Write-Host "built $($t.Out) ($size KB)" -ForegroundColor Green
}

function Build-Tests {
    Build-Assets
    $sqlite = Build-Sqlite "win" @()
    Invoke-Zig (@("cc") + $Flags + $Debug + @("-Itests") + (Get-Sources $true) + $sqlite + @("-o", "$B/test.exe") + $WinLibs)
}

# The whole suite in one process per core (test.exe --shard K/N), then one summary - what
# tests/parallel.sh does for make test. A filter runs in one process, as before.
function Invoke-Tests([string[]]$Filter) {
    if ($Filter) {
        & "./$B/test.exe" @Filter
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        return
    }
    $n = if ($env:HEMMA_TEST_JOBS) { [int]$env:HEMMA_TEST_JOBS } else { [Environment]::ProcessorCount }
    $out = "$B/test-out"
    New-Item -ItemType Directory -Force $out | Out-Null
    $procs = @(for ($k = 0; $k -lt $n; $k++) {
            $p = Start-Process -FilePath "$B/test.exe" -ArgumentList @("--shard", "$k/$n") -NoNewWindow -PassThru `
                -RedirectStandardError "$out/$k.log" -RedirectStandardOutput "$out/$k.out"
            $null = $p.Handle # (so its exit code can still be read once it has ended)
            $p
        })
    foreach ($p in $procs) { $p.WaitForExit() }
    $run = 0; $failed = 0; $checks = 0; $ms = 0; $bad = $false
    for ($k = 0; $k -lt $n; $k++) {
        $sum = $null
        foreach ($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot "$out/$k.log"))) {
            if ($line -match '^shard \d+/\d+: (\d+) run, (\d+) failed, (\d+) checks, (\d+) ms') { $sum = $Matches }
            else { Write-Host $line }
        }
        if (-not $sum) { $failed++; $bad = $true; continue } # (it never got to the end: a crash)
        $run += [int]$sum[1]; $failed += [int]$sum[2]; $checks += [int]$sum[3]
        $ms = [math]::Max($ms, [int]$sum[4])
        if ($procs[$k].ExitCode -ne 0) { $bad = $true }
    }
    if ($failed -or $bad) {
        Write-Host "`n$failed of $run tests failed ($checks checks, $ms ms in $n processes)" -ForegroundColor Red
        exit 1
    }
    Write-Host "`nall $run tests passed ($checks checks, $ms ms in $n processes)" -ForegroundColor Green
}

# CI's Linux jobs (gcc + the suite, ASan + UBSan, the converter's fuzzer) in WSL: this checkout's files
# go over as one tar, and tools/ci/local.sh runs the job in WSL's RAM disk (docs/development.md).
function Invoke-CI([string[]]$JobArgs) {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        throw "WSL isn't installed - see 'Checking before you push' in docs/development.md"
    }
    $distro = if ($env:HEMMA_WSL) { @("-d", $env:HEMMA_WSL) } else { @() }
    New-Item -ItemType Directory -Force $B | Out-Null
    $files = @(& git ls-files -co --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw "git ls-files failed" }
    # (LF line ends: tar would take a CR as part of the name)
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot "$B/ci-files.txt"), ($files -join "`n") + "`n", (New-Object Text.UTF8Encoding $false))
    $tarExe = Join-Path $env:SystemRoot "System32/tar.exe" # (Windows' own: Git's may come first on PATH)
    if (-not (Test-Path $tarExe)) { $tarExe = "tar.exe" }
    & $tarExe -cf "$B/ci-src.tar" -T "$B/ci-files.txt"
    if ($LASTEXITCODE -ne 0) { throw "packing the tree failed" }
    $tar = (& wsl.exe @distro --exec wslpath -a (Join-Path $PSScriptRoot "$B/ci-src.tar")).Trim()
    $sh = (& wsl.exe @distro --exec wslpath -a (Join-Path $PSScriptRoot "tools/ci/local.sh")).Trim()
    & wsl.exe @distro --exec bash $sh @JobArgs --tar $tar
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

switch ($Cmd) {
    "build" { Build-Target "win" $Debug }
    "release" { Build-Target "win" $Release }
    "run" {
        Build-Target "win" $Debug
        & "./$B/hemma.exe" --host 127.0.0.1 --port 8080 --web web --data data @Rest
    }
    "test" {
        Build-Tests
        Invoke-Tests $Rest
    }
    "ci" { Invoke-CI $(if ($Rest) { $Rest } else { @("test") }) }
    "app" { $null = Build-App $true }
    "pi" { Build-Target "pi" $Release }
    "pi32" { Build-Target "pi32" $Release }
    "linux" { Build-Target "linux" $Release }
    "mac" { foreach ($n in "mac-arm64", "mac-x64") { Build-Target $n $Release } }
    "all" {
        Build-Tests
        Invoke-Tests @()
        foreach ($n in "win", "pi", "pi32", "linux", "mac-arm64", "mac-x64") { Build-Target $n $Release }
    }
    "clean" { Remove-Item -Recurse -Force $B -ErrorAction SilentlyContinue; Write-Host "cleaned $B" }
    default { Write-Host "unknown command '$Cmd' - see the header of build.ps1"; exit 2 }
}

<#
  build.ps1 - build, test and run Hemma on Windows with `zig cc`.

    ./build.ps1                 debug build          -> build/hemma.exe
    ./build.ps1 run [args]      debug build + run on http://127.0.0.1:8080 (web/ + module CSS served live)
    ./build.ps1 test [filter]   build + run tests    (one process per core; a filter = substring of a test
                                or file name, run in one process)
    ./build.ps1 ci [job]        CI's Linux jobs in WSL (tools/ci/local.sh): test | asan | fuzz [seconds]
    ./build.ps1 release         optimised build      -> build/hemma.exe
    ./build.ps1 pi              Raspberry Pi 64-bit  -> build/hemma-pi64   (static, aarch64)
    ./build.ps1 pi32            Raspberry Pi 32-bit  -> build/hemma-pi32   (static, armv6: every Pi)
    ./build.ps1 linux           Linux x86_64         -> build/hemma-linux  (static; pi/pi32 run on any ARM Linux)
    ./build.ps1 mac             macOS 11+            -> build/hemma-mac-arm64 (Apple silicon), build/hemma-mac-x64 (Intel)
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
$WasmSources = @("modules/av/wasm/avconv.c") + @("conv", "mkv", "mpeg2", "deint", "h264enc", "ac3", "aac", "mp4" |
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

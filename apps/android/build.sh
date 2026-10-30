#!/bin/sh
# apps/android/build.sh OUT_DIR APK - the Hemma app for Android (2.0), built with Android's own tools and nothing
# else: no Gradle, no libraries (docs/decisions/0011). build.ps1 does the same on Windows (Build-App).
#   needs: ANDROID_HOME (or ANDROID_SDK_ROOT) with a platform (android-36 or newer) and build-tools, a JDK's
#          javac / java / keytool (JAVA_HOME, else on PATH)
#   key:   HEMMA_APP_KEYSTORE + HEMMA_APP_KEY_PASSWORD (a PKCS12 file: the release key - every later version
#          must be signed with the same one, or phones refuse the update), else a developer's own key made
#          once in ~/.hemma/hemma-app-dev.p12 (and the app is debuggable: chrome://inspect shows its pages)
# The version: the VERSION file (2.0.0 -> versionCode 20000).
set -eu
OUT=${1:-build/app}
APK=${2:-build/hemma.apk}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
[ -n "$SDK" ] && [ -d "$SDK" ] || { echo "build.sh: no Android SDK (ANDROID_HOME)" >&2; exit 1; }
JBIN=${JAVA_HOME:+$JAVA_HOME/bin/}
newest() { ls -1 "$1" 2>/dev/null | sort -t. -k1,1n -k2,2n -k3,3n | tail -1; }
BT="$SDK/build-tools/$(newest "$SDK/build-tools")"
PLAT=$(ls -1 "$SDK/platforms" | sed -n 's/^android-\([0-9][0-9]*\).*/\1 &/p' | sort -n | tail -1 | cut -d' ' -f2)
JAR="$SDK/platforms/$PLAT/android.jar"
[ -f "$JAR" ] && [ -x "$BT/aapt2" ] || { echo "build.sh: no platform or build-tools in $SDK" >&2; exit 1; }
VER=$(tr -d ' \r\n' < "$HERE/../../VERSION")
CODE=$(echo "$VER" | awk -F. '{ print $1 * 10000 + $2 * 100 + $3 }')

rm -rf "$OUT" 2>/dev/null || true
mkdir -p "$OUT/test" "$OUT/gen" "$OUT/classes"
SRC=$(find "$HERE/src" -name '*.java' | sort)

# 1. the address rules and what's saved for offline (2.3), with a JDK alone
"${JBIN}javac" -encoding UTF-8 -d "$OUT/test" "$HERE/src/io/github/kloparn/hemma/Net.java" "$HERE/test/NetTest.java" \
    "$HERE/src/io/github/kloparn/hemma/Saved.java" "$HERE/test/SavedTest.java"
"${JBIN}java" -cp "$OUT/test" NetTest
"${JBIN}java" -cp "$OUT/test" SavedTest

# 2. the key
if [ -n "${HEMMA_APP_KEYSTORE:-}" ]; then
    KS=$HEMMA_APP_KEYSTORE
    PW=${HEMMA_APP_KEY_PASSWORD:?HEMMA_APP_KEY_PASSWORD}
    DEBUG=""
else
    KS="$HOME/.hemma/hemma-app-dev.p12"
    PW=hemma-dev
    DEBUG=--debug-mode
    if [ ! -f "$KS" ]; then
        mkdir -p "$HOME/.hemma"
        "${JBIN}keytool" -genkeypair -keystore "$KS" -storetype PKCS12 -storepass "$PW" -keypass "$PW" -alias hemma \
            -keyalg RSA -keysize 3072 -validity 10000 -dname "CN=Hemma developer build" >/dev/null 2>&1
    fi
fi

# 3. resources + manifest -> base.apk (and R.java)
"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/res.zip"
"$BT/aapt2" link -I "$JAR" --manifest "$HERE/AndroidManifest.xml" --min-sdk-version 26 --target-sdk-version 36 \
    --version-code "$CODE" --version-name "$VER" --java "$OUT/gen" $DEBUG -o "$OUT/base.apk" "$OUT/res.zip"

# 4. Java -> classes.dex (Java 8 language: d8 turns its lambdas into what every Android runs)
# shellcheck disable=SC2086
"${JBIN}javac" -source 8 -target 8 -bootclasspath "$JAR:$BT/core-lambda-stubs.jar" -Xlint:-options -encoding UTF-8 -d "$OUT/classes" \
    $SRC $(find "$OUT/gen" -name '*.java')
"$BT/d8" --release --min-api 26 --lib "$JAR" --output "$OUT" $(find "$OUT/classes" -name '*.class')

# 5. classes.dex into the APK, aligned, signed
if command -v zip >/dev/null 2>&1; then (cd "$OUT" && zip -q base.apk classes.dex); else (cd "$OUT" && "$BT/aapt" add base.apk classes.dex >/dev/null); fi
"$BT/zipalign" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KS" --ks-pass "pass:$PW" --out "$APK" "$OUT/aligned.apk"
rm -f "$APK.idsig"
echo "built $APK ($(wc -c < "$APK" | tr -d ' ') bytes, $VER, $CODE)"

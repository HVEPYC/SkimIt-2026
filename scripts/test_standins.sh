#!/usr/bin/env bash
# Runs the offline stand-ins on a plain JVM against a fake UnityPlayer and checks the events
# they send (patches/java/test/OfflineHarness.java). android.jar supplies the Android types;
# none of the tested paths call into Android itself.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/env.sh"

OUT="$ROOT/work/test-standins"
android_jar="$(ls "$ANDROID_HOME"/platforms/android-37*/android.jar | head -1)"
rm -rf "$OUT" && mkdir -p "$OUT"

javac -nowarn -classpath "$android_jar" -d "$OUT" \
    $(find "$ROOT/patches/java/src/com/prime31" "$ROOT/patches/java/test" -name '*.java')
java -cp "$OUT:$android_jar" OfflineHarness

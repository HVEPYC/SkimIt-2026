#!/usr/bin/env bash
# Builds the offline classes.dex (M2):
#   1. copy the decoded app (work/apktool, from extract.sh) and delete the third-party SDKs
#      (Flurry, Facebook, Prime31, Twitter OAuth, billing AIDL, support lib) and Unity's
#      Java WWW helper; only com/unity3d/player and org/fmod remain from the original
#   2. fail if any remaining class still references a removed package
#   3. assemble the remaining smali with apktool, compile our stand-ins
#      (patches/java/src) and merge both with d8 into work/patched/classes.dex
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/env.sh"

SRC="$ROOT/work/apktool"
OUT="$ROOT/work/patched"
STAGE="$OUT/stage"
[[ -d "$SRC/smali" ]] || { echo "run scripts/extract.sh first" >&2; exit 1; }

REMOVE=(com/flurry com/facebook com/prime31 org/scribe com/android/vending android/support)
REPLACED_CLASSES=(com/unity3d/player/WWW com/unity3d/player/b)

rm -rf "$STAGE" "$OUT/java-classes" "$OUT/dex"
mkdir -p "$OUT" "$OUT/java-classes" "$OUT/dex"
cp -r "$SRC" "$STAGE"

for pkg in "${REMOVE[@]}"; do rm -rf "$STAGE/smali/$pkg"; done
for cls in "${REPLACED_CLASSES[@]}"; do rm -f "$STAGE/smali/$cls.smali" "$STAGE/smali/$cls"\$*.smali; done

# Compatibility: the proxy activity starts the plain-Java UnityPlayerActivity
# instead of UnityPlayerNativeActivity (an android.app.NativeActivity, whose native input queue
# the runner doesn't bridge yet). It is the original's own choice for SDK < 9; the manifest sets
# ForwardNativeEventsToDalvik, so input reaches the game through the Java path either way.
proxy="$STAGE/smali/com/unity3d/player/UnityPlayerProxyActivity.smali"
[[ $(grep -c '    const/16 v4, 0x9$' "$proxy") == 1 ]] || { echo "UnityPlayerProxyActivity: unexpected code" >&2; exit 1; }
sed -i 's/^    const\/16 v4, 0x9$/    const v4, 0x7fffffff  # skim port: always UnityPlayerActivity/' "$proxy"

# Remaining original code must not depend on anything removed. Our stand-ins provide
# com/prime31/* and WWW again, so references to those exact classes are allowed only if we
# ship them; everything else in the removed packages must be unreferenced.
provided="$(cd "$ROOT/patches/java/src" && find . -name '*.java' | sed -E 's|^\./||; s|\.java$||')"
bad=0
for pkg in "${REMOVE[@]}"; do
    while IFS= read -r ref; do
        cls="${ref#L}"; cls="${cls%;}"
        grep -qxF "$cls" <<<"$provided" && continue
        echo "dangling reference to removed class: $cls" >&2
        bad=1
    done < <(grep -rhoE "L${pkg}/[A-Za-z0-9_/\$]+;" "$STAGE/smali" | sort -u)
done
((bad == 0)) || exit 1

# Assemble the pruned original code (apktool also rebuilds resources; only classes.dex is used).
java -jar "$APKTOOL_JAR" b -q -f "$STAGE" -o "$OUT/stage.apk"
unzip -q -o -j "$OUT/stage.apk" classes.dex -d "$OUT/dex/original"

# Compile the stand-ins against the Android 17 SDK and merge.
android_jar="$(ls "$ANDROID_HOME"/platforms/android-37*/android.jar | head -1)"
javac -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$android_jar" \
    -d "$OUT/java-classes" $(find "$ROOT/patches/java/src" -name '*.java')
(cd "$OUT/java-classes" && jar cf "$OUT/standins.jar" .)
d8 --release --min-api 24 --lib "$android_jar" --output "$OUT/dex" \
    "$OUT/dex/original/classes.dex" "$OUT/standins.jar"

mv "$OUT/dex/classes.dex" "$OUT/classes.dex"
rm -rf "$STAGE" "$OUT/stage.apk" "$OUT/dex" "$OUT/standins.jar"
echo "wrote $OUT/classes.dex ($(stat -c %s "$OUT/classes.dex") bytes)"

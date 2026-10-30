#!/usr/bin/env bash
# Assembles the Android 17 APK (M4 prototype of port.sh's last stage) from:
#   work/apktool        the decoded original (resources, assets incl. the guest libunity/libmono)
#   work/patched        offline classes.dex + manifest (M2: scripts/patch_java.sh, patch_manifest.py)
#   the runner: prebuilt/runner/<abi>/{libunity.so,libmono.so} (committed, checked against SHA256SUMS),
#               or RUNNER_DIR=<dir with the same layout> (scripts/package_runner.sh refreshes prebuilt/)
# Output: out/SkimIt-android17.apk, signed with a local key (tools/keystore/port.jks).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/env.sh"
MIN_SDK=26
TARGET_SDK=37
ABIS=(arm64-v8a x86_64)
STAGE="$ROOT/work/apkbuild"
OUT="${OUT_APK:-$ROOT/out/SkimIt-android17.apk}"

for f in work/patched/classes.dex work/patched/AndroidManifest.xml; do
    [[ -f "$ROOT/$f" ]] || { echo "missing $f (run scripts/patch_java.sh and scripts/patch_manifest.py)" >&2; exit 1; }
done
RUNNER_DIR="${RUNNER_DIR:-$ROOT/prebuilt/runner}"
[[ -f "$RUNNER_DIR/SHA256SUMS" ]] || { echo "no runner in $RUNNER_DIR (developers: scripts/package_runner.sh)" >&2; exit 1; }
(cd "$RUNNER_DIR" && sha256sum -c --quiet SHA256SUMS) || { echo "runner files in $RUNNER_DIR don't match SHA256SUMS" >&2; exit 1; }

# 1. Resources, manifest and assets with apktool (no smali: the dex comes from M2).
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -r "$ROOT/work/apktool/." "$STAGE/src"
rm -rf "$STAGE/src/smali" "$STAGE/src/original" "$STAGE/src/lib"
python3 -I "$ROOT/scripts/modernize_manifest.py" "$ROOT/work/patched/AndroidManifest.xml" "$STAGE/src/AndroidManifest.xml"
sed -i "s/minSdkVersion: .*/minSdkVersion: $MIN_SDK/; s/targetSdkVersion: .*/targetSdkVersion: $TARGET_SDK/" "$STAGE/src/apktool.yml"
java -jar "$APKTOOL_JAR" b -q "$STAGE/src" -o "$STAGE/base.apk"

# 2. Dex and the runner libraries (stored uncompressed so Android loads them straight from the APK).
mkdir -p "$STAGE/add/lib"
cp "$ROOT/work/patched/classes.dex" "$STAGE/add/classes.dex"
for abi in "${ABIS[@]}"; do
    mkdir -p "$STAGE/add/lib/$abi"
    cp "$RUNNER_DIR/$abi/libunity.so" "$RUNNER_DIR/$abi/libmono.so" "$STAGE/add/lib/$abi/"
done
python3 -I - "$STAGE/base.apk" "$STAGE/add" <<'PY'
import sys, zipfile, pathlib
apk, add = sys.argv[1], pathlib.Path(sys.argv[2])
with zipfile.ZipFile(apk, "a") as z:
    for f in sorted(add.rglob("*")):
        if f.is_file():
            name = f.relative_to(add).as_posix()
            z.write(f, name, zipfile.ZIP_STORED if name.startswith("lib/") else zipfile.ZIP_DEFLATED)
PY

# 3. Align (16 KB for the .so files) and sign with the local key.
mkdir -p "$(dirname "$OUT")" "$ROOT/tools/keystore"
KS="$ROOT/tools/keystore/port.jks"
if [[ ! -f "$KS" ]]; then
    keytool -genkeypair -keystore "$KS" -storepass skimport -keypass skimport -alias port -keyalg RSA -keysize 3072 \
        -validity 36500 -dname "CN=Skim It port (local)" >/dev/null
fi
zipalign -f -P 16 4 "$STAGE/base.apk" "$STAGE/aligned.apk"
apksigner sign --ks "$KS" --ks-pass pass:skimport --ks-key-alias port --out "$OUT" "$STAGE/aligned.apk"
apksigner verify "$OUT"
echo "[apk] $OUT ($(du -h "$OUT" | cut -f1))"

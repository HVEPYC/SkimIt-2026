#!/usr/bin/env bash
# Unpacks the input APK into work/ (port.sh then checks its package name and version):
#   work/apk/     raw zip contents
#   work/apktool/ decoded manifest, resources and smali
#   work/il/      decompiled C# of the game's managed assemblies
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/env.sh"

usage() { echo "usage: $0 <SkimIt.apk> [--no-decompile]" >&2; exit 2; }
[[ $# -ge 1 ]] || usage
APK="$1"
shift
DECOMPILE=1
for a in "$@"; do
    case "$a" in
        --no-decompile) DECOMPILE=0 ;;  # port.sh: the C# decompile (needs .NET) is for development only
        *) usage ;;
    esac
done
[[ -f "$APK" ]] || { echo "no such file: $APK" >&2; exit 1; }

WORK="$ROOT/work"
rm -rf "$WORK/apk" "$WORK/apktool" "$WORK/il"
mkdir -p "$WORK"

unzip -q "$APK" -d "$WORK/apk"
java -jar "$APKTOOL_JAR" d -q -f -o "$WORK/apktool" "$APK"

if ((DECOMPILE)); then
    for dll in Assembly-CSharp Assembly-CSharp-firstpass Assembly-UnityScript; do
        ilspycmd -p -o "$WORK/il/$dll" "$WORK/apk/assets/bin/Data/Managed/$dll.dll" >/dev/null
    done
fi

echo "extracted to $WORK"

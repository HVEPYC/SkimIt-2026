#!/usr/bin/env bash
# Skim It! 1.0.5 -> Android 17 port.
#
#   ./port.sh <SkimIt.apk> [options]
#
# Builds an Android 17 (64-bit) APK from your own copy of Skim It! 1.0.5. Nothing from the game is
# shipped with this script; everything comes from the APK you give it. The script never installs
# anything on a phone and never contacts one: it only writes the new APK.
#
# Options:
#   --out FILE        where to write the APK (default: out/SkimIt-android17.apk)
#   --force           accept an APK that isn't Skim It! (flippinpixels.skim) version 1.0.5
#   --build-runner    rebuild the runner from source instead of using prebuilt/runner
#                     (downloads the Android NDK and more; for developers)
#   -h, --help        this help
#
# Works on Linux and on Windows through WSL (x86_64). Tools are downloaded once into ./tools;
# nothing is installed system-wide and nothing needs root.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_APK="$ROOT/out/SkimIt-android17.apk"
FORCE="" BUILD_RUNNER=0 APK=""
while (($#)); do
    case "$1" in
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --force) FORCE=--force ;;
        --build-runner) BUILD_RUNNER=1 ;;
        --out) shift; OUT_APK="$(realpath -m "${1:?--out needs a file name}")" ;;
        -*) echo "unknown option: $1 (see ./port.sh --help)" >&2; exit 2 ;;
        *) [[ -z "$APK" ]] || { echo "only one APK, please (see ./port.sh --help)" >&2; exit 2; }; APK="$1" ;;
    esac
    shift
done

# ---------------------------------------------------------------------------------------------
# Output helpers: every step says what it does and why; tool output goes to the log file.
if [[ -t 1 ]]; then B=$'\033[1m' DIM=$'\033[2m' G=$'\033[1;32m' Y=$'\033[1;33m' R=$'\033[1;31m' C=$'\033[1;36m' N=$'\033[0m'
else B="" DIM="" G="" Y="" R="" C="" N=""; fi
mkdir -p "$ROOT/out"
LOG="$ROOT/out/port.log"
: >"$LOG"
STEP=0 STEPS=6 T0=$SECONDS
step() { STEP=$((STEP + 1)); printf '\n%s[%d/%d] %s%s\n' "$C" "$STEP" "$STEPS" "$1" "$N"; }
say() { printf '      %s\n' "$*"; }
note() { printf '      %s%s%s\n' "$DIM" "$*" "$N"; }
ok() { printf '      %s✓%s %s\n' "$G" "$N" "$*"; }
warn() { printf '      %s!%s %s\n' "$Y" "$N" "$*"; }
die() {
    printf '\n%s✗ %s%s\n' "$R" "$1" "$N" >&2
    shift
    for l in "$@"; do printf '  %s\n' "$l" >&2; done
    exit 1
}
# run "what" command...: runs quietly (output to the log), reports success with the time taken.
run() {
    local what="$1" t=$SECONDS
    shift
    if [[ -t 1 ]]; then printf '      … %s' "$what"; else printf '      … %s\n' "$what"; fi
    printf '\n===== %s\n$ %s\n' "$what" "$*" >>"$LOG"
    local back=""
    [[ -t 1 ]] && back=$'\r\033[K'
    if "$@" >>"$LOG" 2>&1; then
        printf '%s      %s✓%s %s %s(%ds)%s\n' "$back" "$G" "$N" "$what" "$DIM" $((SECONDS - t)) "$N"
    else
        printf '%s      %s✗%s %s\n' "$back" "$R" "$N" "$what"
        echo "      last lines of $LOG:" >&2
        tail -n 15 "$LOG" | sed 's/^/        /' >&2
        die "Step failed: $what" "Full log: $LOG"
    fi
}

printf '%sSkim It! → Android 17 port%s\n' "$B" "$N"
say "This turns your Skim It! 1.0.5 APK (a 2013 32-bit Unity game) into an app that installs and runs"
say "on current 64-bit Android phones. The original game engine, code and graphics run unchanged inside"
say "a small 32-bit emulator layer (\"the runner\"); online features are made inert and the app gets no"
say "internet permission at all. Detailed tool output: ${LOG#"$ROOT/"}"
[[ -n "$APK" ]] || die "No APK given." "Usage: ./port.sh <SkimIt.apk>   (./port.sh --help for options)"
[[ -f "$APK" ]] || die "No such file: $APK"
APK="$(realpath "$APK")"

# ---------------------------------------------------------------------------------------------
step "Checking this computer"
[[ "$(uname -s)" == Linux ]] || die "This script runs on Linux, or on Windows through WSL." \
    "On Windows: install WSL (https://learn.microsoft.com/windows/wsl/install), open Ubuntu and run it there."
WSL=0
grep -qi microsoft /proc/version 2>/dev/null && WSL=1
. /etc/os-release 2>/dev/null || true
ok "Linux: ${PRETTY_NAME:-unknown distribution}$( ((WSL)) && echo ' (running under WSL)')"
[[ "$(uname -m)" == x86_64 ]] || die "This computer is $(uname -m); the Android build tools for Linux exist only for x86_64 (64-bit Intel/AMD)."
ok "64-bit x86 processor"
if ldd --version 2>&1 | grep -qi musl; then
    die "This distribution uses musl libc (e.g. Alpine); the downloaded Java and Android tools need glibc." \
        "Use a glibc distribution such as Ubuntu, Debian or Fedora (WSL's default Ubuntu works)."
fi

# Commands we rely on, with the package that provides them on each distribution family.
declare -A PKG_DEB=([curl]=curl [unzip]=unzip [tar]=tar [python3]=python3 [sha256sum]=coreutils [awk]=gawk [sed]=sed [find]=findutils [grep]=grep [df]=coreutils)
declare -A PKG_RPM=([curl]=curl [unzip]=unzip [tar]=tar [python3]=python3 [sha256sum]=coreutils [awk]=gawk [sed]=sed [find]=findutils [grep]=grep [df]=coreutils)
declare -A PKG_ARCH=([curl]=curl [unzip]=unzip [tar]=tar [python3]=python [sha256sum]=coreutils [awk]=gawk [sed]=sed [find]=findutils [grep]=grep [df]=coreutils)
declare -A PKG_SUSE=([curl]=curl [unzip]=unzip [tar]=tar [python3]=python3 [sha256sum]=coreutils [awk]=gawk [sed]=sed [find]=findutils [grep]=grep [df]=coreutils)
missing=()
for c in curl unzip tar python3 sha256sum awk sed find grep df; do command -v "$c" >/dev/null || missing+=("$c"); done
if ((${#missing[@]})); then
    family="${ID:-} ${ID_LIKE:-}"
    case "$family" in
        *debian*|*ubuntu*) pk=(); for c in "${missing[@]}"; do pk+=("${PKG_DEB[$c]}"); done
            hint="sudo apt update && sudo apt install -y ${pk[*]}" ;;
        *fedora*|*rhel*|*centos*) pk=(); for c in "${missing[@]}"; do pk+=("${PKG_RPM[$c]}"); done
            hint="sudo dnf install -y ${pk[*]}" ;;
        *arch*) pk=(); for c in "${missing[@]}"; do pk+=("${PKG_ARCH[$c]}"); done
            hint="sudo pacman -S --needed ${pk[*]}" ;;
        *suse*) pk=(); for c in "${missing[@]}"; do pk+=("${PKG_SUSE[$c]}"); done
            hint="sudo zypper install ${pk[*]}" ;;
        *) hint="install the packages providing: ${missing[*]}" ;;
    esac
    die "Missing programs: ${missing[*]}" "These are standard tools; install them with:" "  $hint" "then run ./port.sh again."
fi
ok "required programs present (curl, unzip, tar, python3, coreutils)"
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' \
    || die "Python $(python3 -c 'import platform; print(platform.python_version())') is too old; 3.8 or newer is needed."
ok "Python $(python3 -c 'import platform; print(platform.python_version())')"
free_kb=$(df -Pk "$ROOT" | awk 'NR==2 {print $4}')
need_kb=$((2 * 1024 * 1024))
[[ -d "$ROOT/tools/jdk" ]] && need_kb=$((600 * 1024))
((free_kb >= need_kb)) || die "Not enough free disk space in $ROOT: $((free_kb / 1024)) MB free, about $((need_kb / 1024)) MB needed."
ok "disk space: $((free_kb / 1024 / 1024)) GB free"
if ((WSL)) && [[ "$ROOT" == /mnt/[a-z]/* ]]; then
    warn "This folder is on a Windows drive ($ROOT). It works, but file access from WSL is slow"
    warn "there (several minutes extra). For speed, copy the project into your Linux home (e.g. ~/skim-port)."
fi

# ---------------------------------------------------------------------------------------------
step "Tools (downloaded once into ./tools)"
if [[ -x "$ROOT/tools/jdk/bin/java" && -f "$ROOT/tools/apktool/apktool.jar" && -d "$ROOT/tools/android-sdk/build-tools" ]]; then
    say "Already downloaded on an earlier run; checking they're all there."
else
    say "First run: downloading about 450 MB (Java 21, apktool, Google's Android build tools; about 850 MB"
    say "once unpacked) into $ROOT/tools."
    say "Nothing is installed outside this folder; delete it to remove them. Later runs skip this step."
fi
note "Java runs the Android tools; apktool unpacks/repacks the APK; the build tools align and sign it."
if ((BUILD_RUNNER)); then
    say "--build-runner: also downloading the developer tools (Android NDK, .NET, Boost; several GB)."
    "$ROOT/scripts/bootstrap.sh" --dev || die "Downloading the tools failed." "Details: $ROOT/tools/bootstrap.log"
else
    "$ROOT/scripts/bootstrap.sh" --user || die "Downloading the tools failed." "Check your internet connection; details: $ROOT/tools/bootstrap.log"
fi
source "$ROOT/tools/env.sh"
ok "tools ready"

# ---------------------------------------------------------------------------------------------
step "Checking and unpacking your APK"
say "File: $APK ($(du -h "$APK" | cut -f1))"
run "unpacking the APK and decoding its resources (work/apk, work/apktool)" "$ROOT/scripts/extract.sh" "$APK" --no-decompile
# Identify the game from its own manifest (package name and version), not from the file name.
pkg=$(sed -n 's/.*<manifest[^>]* package="\([^"]*\)".*/\1/p' "$ROOT/work/apktool/AndroidManifest.xml" | head -1)
ver=$(sed -n 's/^ *versionName: *//p' "$ROOT/work/apktool/apktool.yml" | tr -d "'\"" | head -1)
say "Package: ${pkg:-?}, version ${ver:-?}"
if [[ "$pkg" == flippinpixels.skim && "$ver" == 1.0.5 ]]; then
    ok "recognised: Skim It! 1.0.5"
elif [[ -n "$FORCE" ]]; then
    warn "not Skim It! 1.0.5; continuing because of --force (the result may not work)"
else
    die "This APK isn't Skim It! 1.0.5 (found ${pkg:-unknown package}, version ${ver:-unknown})." \
        "The port is made for flippinpixels.skim version 1.0.5. If you're sure, rerun with --force."
fi

# ---------------------------------------------------------------------------------------------
step "Making the game fully offline"
say "The original talked to Google Play billing, Flurry ads/analytics, Facebook and Twitter, all long gone."
say "Their code is removed and replaced by small offline stand-ins that answer exactly as the original"
say "did without internet: the shop says the store is unavailable, ads never load, logins fail politely."
say "The game's own C# code is not modified; only the Java side changes, plus a few compatibility fixes:"
note "• the engine's CPU check reports the emulated 32-bit ARM processor (it would refuse a 64-bit phone)"
note "• the game starts through Unity's plain Java screen (the native-input variant isn't bridged yet)"
run "building the offline Java code (classes.dex)" "$ROOT/scripts/patch_java.sh"
run "removing network/billing permissions from the manifest" python3 -I "$ROOT/scripts/patch_manifest.py"
run "verifying: no networking code, no network permissions, every plugin method still answered" python3 -I "$ROOT/scripts/verify_offline.py"
ok "offline: the app will have no internet permission (Android blocks all network access for it)"

# ---------------------------------------------------------------------------------------------
step "Building the Android 17 app"
say "Packs the original assets, engine and game code together with:"
note "• the runner (prebuilt/runner): a 64-bit library that runs the original 32-bit engine on an"
note "  ARM32 emulator (dynarmic) and connects it to the phone's Java runtime and graphics driver"
note "• a modern manifest: Android 8.0+, target Android 17, immersive full screen, the notification"
note "  permission prompt for the game's reminder, and the original Back-button exit dialog"
if ((BUILD_RUNNER)); then
    say "--build-runner: compiling the runner from source (runner/), then packaging it into prebuilt/runner."
    run "compiling the runner for arm64-v8a and x86_64 (several minutes)" "$ROOT/scripts/package_runner.sh"
fi
if [[ -f "$ROOT/tools/keystore/port.jks" ]]; then
    note "Signing with your existing key (tools/keystore/port.jks), so the result can update an earlier install."
else
    note "Creating your personal signing key: tools/keystore/port.jks. Keep it: Android only accepts updates"
    note "of the app signed with the same key (otherwise uninstall first, which deletes saved progress)."
fi
run "assembling, aligning and signing the APK" env OUT_APK="$OUT_APK" "$ROOT/scripts/build_apk.sh"

# ---------------------------------------------------------------------------------------------
step "Done"
ok "$OUT_APK ($(du -h "$OUT_APK" | cut -f1), built in $((SECONDS - T0))s)"
say ""
say "${B}How to install it${N} (this script doesn't touch any phone):"
say "  • copy the APK to the phone (USB, cloud drive...), open it and allow installing from that source, or"
say "  • with USB debugging on: adb install \"$OUT_APK\""
if ((WSL)); then
    say "  • from WSL, copy it to Windows first, e.g.:  cp \"$OUT_APK\" /mnt/c/Users/<you>/Downloads/"
fi
say "Needs a 64-bit Android phone with Android 8.0 or newer; made for Android 17."
note "The APK contains the game's files from your copy; it's for your own use, please don't share it."

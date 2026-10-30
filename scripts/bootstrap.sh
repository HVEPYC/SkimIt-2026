#!/usr/bin/env bash
# Installs the tools the port pipeline needs into ./tools (no global installs, no root).
#   scripts/bootstrap.sh --user   what port.sh needs: JDK 21, apktool, Android build-tools + platform (~700 MB)
#   scripts/bootstrap.sh [--dev]  everything for development: also the NDK, .NET 8 + ilspycmd, Boost headers
# Idempotent: re-running skips anything already present. Downloaded archives are deleted once
# unpacked. Detailed tool output goes to tools/bootstrap.log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/tools"
DL="$TOOLS/.downloads"
LOG="$TOOLS/bootstrap.log"
MODE=dev
[[ "${1:-}" == "--user" ]] && MODE=user
mkdir -p "$TOOLS" "$DL"
: >"$LOG"

# Pinned versions
CMDLINE_TOOLS_BUILD=16111833
ANDROID_PLATFORM="platforms;android-37.0"
BUILD_TOOLS="build-tools;37.0.0"
NDK="ndk;29.0.14206865"
APKTOOL_VERSION=3.0.3
DOTNET_CHANNEL=8.0
ILSPYCMD_VERSION=9.1.0.7988
BOOST_VERSION=1.86.0
BOOST_SHA256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b

if [[ -t 1 ]]; then DOT=$'\033[1;34m•\033[0m' PROGRESS=--progress-bar; else DOT='•' PROGRESS=-sS; fi
log() { printf '      %s %s\n' "$DOT" "$*"; }
have() { log "$1: already installed"; }

fetch() { # url dest description
    [[ -f "$2" ]] && return
    log "downloading $3"
    curl -fL --retry 3 "$PROGRESS" -o "$2.part" "$1" && mv "$2.part" "$2"
}

# JDK 21 (Temurin): runs apktool, javac (our offline Java stand-ins), d8, apksigner and keytool.
if [[ -x "$TOOLS/jdk/bin/java" ]]; then
    have "Java 21 (Eclipse Temurin)"
else
    fetch "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse" "$DL/jdk21.tar.gz" \
        "Java 21 (Eclipse Temurin JDK, ~200 MB) - runs the Android tools"
    mkdir -p "$TOOLS/jdk"
    tar -xzf "$DL/jdk21.tar.gz" -C "$TOOLS/jdk" --strip-components=1
    rm -f "$DL/jdk21.tar.gz"
fi
export JAVA_HOME="$TOOLS/jdk"
export PATH="$JAVA_HOME/bin:$PATH"

# Android SDK pieces (Google's command-line tools, then sdkmanager for the packages).
SDK="$TOOLS/android-sdk"
SDKMANAGER="$SDK/cmdline-tools/latest/bin/sdkmanager"
if [[ -x "$SDKMANAGER" ]]; then
    have "Android command-line tools"
else
    fetch "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS_BUILD}_latest.zip" "$DL/cmdline-tools.zip" \
        "Android command-line tools (~150 MB) - Google's SDK package manager"
    rm -rf "$SDK/cmdline-tools/latest" "$DL/cmdline-tools"
    unzip -q "$DL/cmdline-tools.zip" -d "$DL"
    mkdir -p "$SDK/cmdline-tools"
    mv "$DL/cmdline-tools" "$SDK/cmdline-tools/latest"
    rm -f "$DL/cmdline-tools.zip"
fi
sdk_pkgs=("$ANDROID_PLATFORM" "$BUILD_TOOLS")
[[ $MODE == dev ]] && sdk_pkgs+=("$NDK")
need_pkgs=()
for p in "${sdk_pkgs[@]}"; do
    if [[ -d "$SDK/${p//;//}" ]]; then have "Android SDK package ${p}"; else need_pkgs+=("$p"); fi
done
if ((${#need_pkgs[@]})); then
    log "installing Android SDK packages: ${need_pkgs[*]}"
    log "  (build-tools: zipalign, apksigner, d8; platform: android.jar to compile against; by installing"
    log "   them you accept the Android SDK License: https://developer.android.com/studio/terms)"
    yes | "$SDKMANAGER" --sdk_root="$SDK" --licenses >>"$LOG" 2>&1 || true
    "$SDKMANAGER" --sdk_root="$SDK" "${need_pkgs[@]}" >>"$LOG" 2>&1 \
        || { echo "sdkmanager failed; see $LOG" >&2; exit 1; }
fi

# apktool (decodes and rebuilds the APK's resources; bundles smali/baksmali).
mkdir -p "$TOOLS/apktool"
if [[ -f "$TOOLS/apktool/apktool.jar" ]]; then
    have "apktool $APKTOOL_VERSION"
else
    fetch "https://github.com/iBotPeaches/Apktool/releases/download/v${APKTOOL_VERSION}/apktool_${APKTOOL_VERSION}.jar" "$TOOLS/apktool/apktool.jar" \
        "apktool $APKTOOL_VERSION (~25 MB) - unpacks and repacks APK resources"
fi

export DOTNET_ROOT="$TOOLS/dotnet"
export DOTNET_CLI_HOME="$TOOLS/dotnet-home"
if [[ $MODE == dev ]]; then
    # .NET SDK + ilspycmd (decompiles the game's C# for analysis and the offline-patch checks).
    export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
    if [[ ! -x "$DOTNET_ROOT/dotnet" ]]; then
        fetch "https://dot.net/v1/dotnet-install.sh" "$DL/dotnet-install.sh" ".NET installer"
        bash "$DL/dotnet-install.sh" --channel "$DOTNET_CHANNEL" --install-dir "$DOTNET_ROOT" --no-path >>"$LOG" 2>&1
    fi
    if [[ ! -x "$TOOLS/dotnet-tools/ilspycmd" ]]; then
        "$DOTNET_ROOT/dotnet" tool install ilspycmd --version "$ILSPYCMD_VERSION" --tool-path "$TOOLS/dotnet-tools" >>"$LOG" 2>&1
    fi
    # Boost headers (dynarmic needs them; the Android runner build must not use the host's /usr/include)
    if [[ ! -f "$TOOLS/boost/include/boost/version.hpp" ]]; then
        fetch "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION//./_}.tar.bz2" "$DL/boost_${BOOST_VERSION//./_}.tar.bz2" "Boost $BOOST_VERSION headers"
        echo "$BOOST_SHA256  $DL/boost_${BOOST_VERSION//./_}.tar.bz2" | sha256sum -c --quiet
        log "unpacking Boost headers"
        mkdir -p "$TOOLS/boost/include"
        tar -xjf "$DL/boost_${BOOST_VERSION//./_}.tar.bz2" -C "$TOOLS/boost/include" --strip-components=1 "boost_${BOOST_VERSION//./_}/boost"
        rm -f "$DL/boost_${BOOST_VERSION//./_}.tar.bz2"
    fi
    log "development tools ready (NDK, .NET, ilspycmd, Boost)"
fi

# Environment file for the other scripts (entries for tools that aren't installed are harmless).
cat > "$TOOLS/env.sh" <<EOF
export JAVA_HOME="$TOOLS/jdk"
export ANDROID_HOME="$SDK"
export ANDROID_NDK_HOME="$SDK/${NDK//;//}"
export DOTNET_ROOT="$DOTNET_ROOT"
export DOTNET_CLI_HOME="$DOTNET_CLI_HOME"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
export BUILD_TOOLS="$SDK/${BUILD_TOOLS//;//}"
export APKTOOL_JAR="$TOOLS/apktool/apktool.jar"
export PATH="\$JAVA_HOME/bin:\$DOTNET_ROOT:$TOOLS/dotnet-tools:\$BUILD_TOOLS:\$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin:\$PATH"
EOF

rm -rf "$DL"
log "tools are in $TOOLS ($(du -sh "$TOOLS" 2>/dev/null | cut -f1))"

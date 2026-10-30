#!/usr/bin/env bash
# Cross-builds the runner (libskimrunner.so, skimtest, skimrun) with the local NDK for the 64-bit ABIs.
#   scripts/build_runner_android.sh [abi...]   (default: arm64-v8a x86_64)
# Output: runner/build/android-<abi>/
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/env.sh"
API=26  # minSdk of the output app; the runner only needs a 64-bit bionic
ABIS=("${@:-arm64-v8a x86_64}")
[[ $# -eq 0 ]] && ABIS=(arm64-v8a x86_64)
for abi in "${ABIS[@]}"; do
    out="$ROOT/runner/build/android-$abi"
    cmake -S "$ROOT/runner" -B "$out" -G Ninja -Wno-dev \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DBoost_INCLUDE_DIR="$ROOT/tools/boost/include" >/dev/null
    ninja -C "$out" skimrunner mono_stub skimtest skimrun
    llvm-readelf -lW "$out/libskimrunner.so" | awk '$1=="LOAD" && $NF!="0x4000" {bad=1} END {exit bad}' \
        || { echo "libskimrunner.so ($abi): LOAD segments not 16 KB aligned" >&2; exit 1; }
    echo "[android] $abi: $(du -h "$out/libskimrunner.so" | cut -f1) libskimrunner.so, 16 KB aligned"
done

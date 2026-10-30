#!/usr/bin/env bash
# Developer step: refreshes the prebuilt runner that port.sh ships (prebuilt/runner/, committed).
# Builds the runner with the NDK (scripts/build_runner_android.sh), strips it, and writes
#   prebuilt/runner/<abi>/libunity.so   the runner (loaded by the original Java as "unity")
#   prebuilt/runner/<abi>/libmono.so    empty stub (the original also loads "mono")
#   prebuilt/runner/SHA256SUMS          checked by build_apk.sh
#   prebuilt/runner/BUILDINFO           source revision and toolchain
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/env.sh"
ABIS=(arm64-v8a x86_64)
OUT="$ROOT/prebuilt/runner"

"$ROOT/scripts/build_runner_android.sh" "${ABIS[@]}"
rm -rf "$OUT"
for abi in "${ABIS[@]}"; do
    mkdir -p "$OUT/$abi"
    llvm-strip -o "$OUT/$abi/libunity.so" "$ROOT/runner/build/android-$abi/libskimrunner.so"
    llvm-strip -o "$OUT/$abi/libmono.so" "$ROOT/runner/build/android-$abi/libmono.so"
done
(cd "$OUT" && sha256sum */*.so > SHA256SUMS)
dirty=""
git -C "$ROOT" diff --quiet HEAD -- runner || dirty=" (+ uncommitted runner changes)"
cat > "$OUT/BUILDINFO" <<INFO
source: $(git -C "$ROOT" rev-parse --short HEAD)$dirty
ndk: $(basename "$ANDROID_NDK_HOME")
abis: ${ABIS[*]}
INFO
echo "[package] $OUT:"
cat "$OUT/SHA256SUMS" "$OUT/BUILDINFO"

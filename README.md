# Skim It! for Modern Android Devices.

This is a repository that contains an all-in-one build script that turns your copy of Skim It! by Flippin Pixels, released in 2013, to run on modern Android devices.

Due to the age of the game, and the last version only being playable on 32-bit ARM phones, I set out to find whether there was a way a wrapper/emulator could be developed that encapsulates the game into a format that becomes playable on modern 64-bit only Android devices, all the way upto Android 17!

This repository takes **your own copy of the original APK**, which runs on the original Unity3.5 made for 32-bit ARM devices, and gets it running on 64-bit Android devices. This project was largely developed with direction from HVEPYC, directing Claude on Claude Opus 5.5, enabling for a working port of the same. Major props to Anthropic for making such a competent LLM capable of such tasks.

Features of this repo:

- **The original game, unchanged.** Its engine, C# code, graphics and sounds run as shipped, inside a small 32-bit ARM emulator layer (the *runner*). Nothing is remade or rebalanced.
- **Fully offline.** The app has no internet permission. The old Google Play billing, Flurry ads and analytics, Facebook and Twitter code is replaced by stand-ins that respond as the original did without a connection: the shop says the store is unavailable, and ads never load.
- **Bring your own APK.** This repository contains no game files. Everything comes from the APK you supply, and the result is for your own use.

Tested on a Pixel 6a with Android 17: menus, gameplay, sound, touch, saves, the shop's offline message, the reminder notification and pause/resume all work.

## What you need
- **Linux on a 64-bit Intel/AMD PC**, or **Windows 10/11 with WSL**
- Standard tools: `curl`, `unzip`, `tar`, `python3` (3.8+).
- About 2 GB of free disk space and an internet connection for the first run. It downloads Java 21, apktool and Google's Android build tools (about 450 MB) into `./tools`. Nothing is installed system-wide and nothing needs root.
- *Skim It!* **version 1.0.5** as an APK file (tested on v1.0.5, users are free to try on other versions and let me know, PLEASE BRING YOUR OWN APK, no download links will be provided)

## Usage
```
git clone https://github.com/HVEPYC/SkimIt-2026.git skim-port && cd skim-port
./port.sh "/path/to/Skim It!_1.0.5.apk"
```
The script explains each of its six steps as it runs them. Detailed tool output goes to `out/port.log`. The result is:

```
out/SkimIt-android17.apk
```

`port.sh` never installs anything on a phone. To install the result:
- copy the APK to the phone and open it there (allow installing from that source), or
- with USB debugging enabled: `adb install out/SkimIt-android17.apk`.

**On WSL,** keep the project in your Linux home folder (for example `~/skim-port`), not under `/mnt/c`, which is much slower. Copy the result to Windows with `cp out/SkimIt-android17.apk /mnt/c/Users/<you>/Downloads/`.

**Signing key:** the first run creates `tools/keystore/port.jks`. Keep it if you want to install updated builds over an existing install later. With a different key, Android requires uninstalling first, which deletes your saved progress.

Options: `--out FILE`, `--force` (accept an APK that isn't the known 1.0.5 build) and `--build-runner` (see below). See `./port.sh --help`.

## What gets changed (compatibility only)
| Change | Why |
|---|---|
| Runner (`lib/<abi>/libunity.so`) runs the original 32-bit engine | 64-bit phones can't run 32-bit code |
| Offline stand-ins for the Prime31, Flurry, Facebook and Twitter plugins; no network permissions | the services are gone; zero internet access |
| The engine's CPU check reports the emulated ARMv7 CPU | the original refuses 64-bit CPUs |
| Starts through Unity's plain Java activity | the native-input activity isn't bridged yet (touch works the same) |
| Modern manifest: Android 8.0+, target Android 17, immersive full screen, notification permission prompt, original Back-button dialog kept | required or expected on current Android |

## How it works
The game is Unity 3.5: a Java front end, plus `libunity.so` and `libmono.so` (32-bit ARM). The port keeps all of them and adds a 64-bit **runner**:

- **CPU:** [dynarmic](https://github.com/azahar-emu/dynarmic) JIT-compiles the original ARMv7 code (VFPv3/NEON) to arm64 or x86_64. Each guest thread gets its own JIT inside a 4 GiB guest address space.
- **Loader:** an ELF32 loader maps the original libraries and binds their imports to host implementations (HLE): libc, pthread, libm, zlib, GLES 2.0 and logging. Sockets and DNS always fail.
- **JNI bridge:** the original Java code loads the runner as `libunity.so`. The runner boots the guest and runs Unity's own `JNI_OnLoad` against the real Android runtime, translating JNI calls, references, arrays and strings between 32-bit guest and 64-bit host.
- **Graphics:** GLES calls go to the phone's driver, with guest pointers translated and buffer mappings mirrored.
- **Mono:** the game's original C# assemblies run on the original Mono runtime (JIT included), with Boehm GC signals emulated.

Source layout:
- `runner/`: the runner (C++, CMake)
- `patches/java/`: the offline Java stand-ins
- `scripts/`: the pipeline steps
- `prebuilt/runner/`: the runner `port.sh` uses

## Building the runner from source
Needs `git clone --recurse-submodules` (for dynarmic) and the full developer toolchain (the Android NDK and more, several GB, downloaded into `./tools`):
```
./port.sh --build-runner "/path/to/Skim It!_1.0.5.apk"     # or: scripts/bootstrap.sh && scripts/package_runner.sh
```
The desktop build (`cmake -S runner -B runner/build/core -G Ninja && ninja -C runner/build/core`) runs the core tests (`skimtest`). It also provides a headless driver (`skimrun`) that boots the original engine on a PC; it needs gcc, CMake, Ninja, Boost headers and zlib from your distribution.

## Troubleshooting
- **"Missing programs"**: run the install command the script prints, then run it again.
- **"isn't a known Skim It! 1.0.5 build"**: the port is made for version 1.0.5. Other files may not work. `--force` builds anyway.
- **App won't install over an older build**: it was signed with a different key. Uninstall first; this loses progress.
- A step failed: the last lines of `out/port.log` are printed. The full log has every tool's output.

## License
The code in this repository is MIT-licensed; see [LICENSE](LICENSE). The prebuilt runner includes third-party code (dynarmic and its dependencies) under their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

*Skim It!* and its assets are © Flippin Pixels. This project isn't affiliated with or endorsed by Flippin Pixels or Unity Technologies. It contains no part of the game and is meant for preserving a copy you already own.


Thank you so much.

A hobby project by HVEPYC, powered by Claude Opus 5.5

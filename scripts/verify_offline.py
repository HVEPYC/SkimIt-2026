#!/usr/bin/env python3
"""Verifies the M2 offline build (work/patched/classes.dex + AndroidManifest.xml).

Checks:
  1. every public method of the original plugin classes exists with the same descriptor in
     the stand-ins (methods whose descriptor names a removed SDK type are exempt, since such
     types no longer exist and nothing can call them)
  2. every method the game's C# wrappers call on those classes exists
  3. WWW keeps the native methods UnityPlayer.nativeInitWWW registers
  4. no class from a removed SDK package is left, other than our stand-ins
  5. no class references networking APIs (java.net, javax.net, android.webkit, Apache HTTP)
  6. the manifest requests no network, billing or task permissions
Exits non-zero on any failure.
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
DEX = WORK / "patched/classes.dex"
MANIFEST = WORK / "patched/AndroidManifest.xml"
ORIGINAL_SMALI = WORK / "apktool/smali"
CSHARP = WORK / "il/Assembly-CSharp-firstpass"

REMOVED_PACKAGES = ("com/flurry/", "com/facebook/", "com/prime31/", "org/scribe/", "com/android/vending/", "android/support/")
STANDINS = {
    "com/prime31/GoogleIABPlugin": "GoogleIAB",
    "com/prime31/FlurryPlugin": "FlurryAndroid",
    "com/prime31/FacebookPlugin": "FacebookAndroid",
    "com/prime31/TwitterPlugin": "TwitterAndroid",
    "com/prime31/EtceteraPlugin": "EtceteraAndroid",
    "com/flurry/android/FlurryAgent": "FlurryAndroid",
}
# Our own helper classes that legitimately live in a removed package.
OWN_HELPERS = {"com/prime31/Bridge", "com/prime31/Bridge$1", "com/prime31/AlarmManagerReceiver"}
NETWORK = re.compile(r"L(?:java/net/(?!URI;|URISyntaxException;)|javax/net/|android/webkit/|org/apache/http/|android/net/http/)[\w/$]+;")
# java.net.URI / URISyntaxException are pure string parsing (used by Unity's dormant licensing
# policy class com/unity3d/player/a/i), not network I/O.
FORBIDDEN_PERMISSIONS = {"android.permission.INTERNET", "android.permission.ACCESS_NETWORK_STATE",
                         "android.permission.ACCESS_WIFI_STATE", "com.android.vending.BILLING",
                         "android.permission.GET_TASKS"}

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL", msg)


def dex_methods():
    """{class: {(name, descriptor, access)}} parsed from dexdump."""
    out = subprocess.run([os.path.join(os.environ["BUILD_TOOLS"], "dexdump"), str(DEX)],
                         capture_output=True, text=True, check=True).stdout
    methods, cls, name = defaultdict(set), None, None
    for line in out.splitlines():
        if m := re.match(r"\s+Class descriptor\s+: 'L(.+);'", line):
            cls = m.group(1)
            methods[cls]
        elif m := re.match(r"\s+name\s+: '(.+)'", line):
            name = m.group(1)
        elif (m := re.match(r"\s+type\s+: '(\(.*)'", line)) and name:
            methods[cls].add((name, m.group(1)))
            name = None
    return methods, out


def original_public_methods(cls):
    path = ORIGINAL_SMALI / f"{cls}.smali"
    sigs = set()
    for line in path.read_text().splitlines():
        if m := re.match(r"\.method (public|protected)( static)?( final)?( native)? (\S+)\((.*)$", line):
            name, rest = m.group(5), m.group(6)
            if name in ("<init>", "<clinit>"):
                continue
            sigs.add((name, "(" + rest))
    return sigs


def main():
    methods, dump = dex_methods()

    # 1 + 2: stand-in signatures
    for cls, wrapper in STANDINS.items():
        ours = methods.get(cls)
        if ours is None:
            fail(f"{cls} missing from dex")
            continue
        original = original_public_methods(cls)
        if cls.startswith("com/prime31/") and cls.endswith("Plugin"):
            original.add(("instance", f"()L{cls};"))
        if cls == "com/flurry/android/FlurryAgent":
            original = {s for s in original if s[0] in
                        {"onError", "onPageView", "setAge", "setContinueSessionMillis", "setLogEnabled", "setUserId"}}
        missing = [s for s in original if s not in ours and not any(p in s[1] for p in REMOVED_PACKAGES if p != "com/prime31/")
                   and "Lcom/prime31/util/" not in s[1]]
        for name, desc in sorted(missing):
            fail(f"{cls}.{name}{desc} missing (present in original)")
        if not CSHARP.exists():  # user tier (port.sh): no decompiled C#; the signature checks above still ran
            print(f"ok   {cls}: {len(original)} original signatures (C# call-site check skipped: no work/il)")
            continue
        source = (CSHARP / f"{wrapper}.cs").read_text()
        # The AndroidJavaObject field the wrapper uses for this Java class (_plugin, _facebookPlugin, ...).
        receiver = "_flurryAgent" if cls == "com/flurry/android/FlurryAgent" else \
            re.search(r"AndroidJavaObject (_\w+);", source).group(1)
        called = set(re.findall(receiver + r'\.(?:Call|CallStatic)(?:<[A-Za-z]+>)?\("([A-Za-z]+)"', source))
        if cls == "com/prime31/FlurryPlugin":
            called.discard("getAndroidId")  # absent in the original plugin too
        names = {n for n, _ in ours}
        for name in sorted(called - names):
            fail(f"{cls}.{name} called from C# ({wrapper}.cs) but missing")
        print(f"ok   {cls}: {len(original)} original signatures, {len(called)} C# call names covered")

    # 3: WWW natives
    www = methods.get("com/unity3d/player/WWW", set())
    for sig in [("doneCallback", "(I)V"), ("errorCallback", "(ILjava/lang/String;)V"),
                ("headerCallback", "(ILjava/lang/String;)Z"), ("progressCallback", "(IFFDI)V"),
                ("readCallback", "(I[BI)Z"), ("<init>", "(ILjava/lang/String;[BLjava/util/Map;)V")]:
        if sig not in www:
            fail(f"WWW{sig[0]}{sig[1]} missing")
    print("ok   com/unity3d/player/WWW natives and constructor")

    # 4: leftovers from removed SDKs
    leftovers = [c for c in methods if c.startswith(REMOVED_PACKAGES) and c not in STANDINS and c not in OWN_HELPERS]
    for c in leftovers:
        fail(f"removed-SDK class still present: {c}")
    print(f"ok   {len(methods)} classes in dex, no removed-SDK leftovers" if not leftovers else "")

    # 5: network references, per class
    per_class, cls = defaultdict(set), None
    for line in dump.splitlines():
        if m := re.match(r"\s+Class descriptor\s+: 'L(.+);'", line):
            cls = m.group(1)
        for ref in NETWORK.findall(line):
            per_class[cls].add(ref)
    for c, refs in sorted(per_class.items()):
        fail(f"{c} references networking API: {', '.join(sorted(refs))}")
    if not per_class:
        print("ok   no references to java.net / javax.net / android.webkit / Apache HTTP")

    # 6: manifest
    android = "{http://schemas.android.com/apk/res/android}name"
    perms = {p.get(android) for p in ET.parse(MANIFEST).getroot().findall("uses-permission")}
    for p in sorted(perms & FORBIDDEN_PERMISSIONS):
        fail(f"manifest requests {p}")
    print(f"ok   manifest permissions: {', '.join(sorted(perms))}" if not perms & FORBIDDEN_PERMISSIONS else "")

    print(f"\n{'PASSED' if not failures else f'FAILED ({len(failures)})'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

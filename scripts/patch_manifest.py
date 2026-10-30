#!/usr/bin/env python3
"""Offline manifest transform (M2).

Reads the apktool-decoded manifest (work/apktool/AndroidManifest.xml) and writes
work/patched/AndroidManifest.xml with:
  - no network, billing, task-list or legacy storage permissions
  - no activities/meta-data of the removed SDKs (Prime31 web/video/IAB/Facebook proxies,
    Flurry takeover, Facebook login, Facebook app id)
  - POST_NOTIFICATIONS for the game's local reminder
  - the reminder receiver kept, not exported
Modernisation for Android 17 (targetSdk, launcher, exported flags) happens in M4.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ANDROID = "http://schemas.android.com/apk/res/android"
NAME = f"{{{ANDROID}}}name"
EXPORTED = f"{{{ANDROID}}}exported"

REMOVED_PERMISSIONS = {
    "android.permission.INTERNET",
    "android.permission.ACCESS_NETWORK_STATE",
    "android.permission.ACCESS_WIFI_STATE",
    "com.android.vending.BILLING",
    "android.permission.GET_TASKS",
    "android.permission.WRITE_EXTERNAL_STORAGE",
}
ADDED_PERMISSIONS = ["android.permission.POST_NOTIFICATIONS"]
REMOVED_COMPONENTS = {
    "com.prime31.EtceteraProxyActivity",
    "com.prime31.WebViewActivity",
    "com.prime31.P31VideoPlayerActivity",
    "com.prime31.GoogleIABProxyActivity",
    "com.prime31.FacebookProxyActivity",
    "com.flurry.android.FlurryFullscreenTakeoverActivity",
    "com.facebook.LoginActivity",
    "com.facebook.sdk.ApplicationId",
}
RECEIVER = "com.prime31.AlarmManagerReceiver"


def main(src: Path, dst: Path) -> None:
    ET.register_namespace("android", ANDROID)
    tree = ET.parse(src)
    manifest = tree.getroot()

    for perm in manifest.findall("uses-permission"):
        if perm.get(NAME) in REMOVED_PERMISSIONS:
            manifest.remove(perm)
    for name in ADDED_PERMISSIONS:
        ET.SubElement(manifest, "uses-permission", {NAME: name})

    application = manifest.find("application")
    for child in list(application):
        if child.get(NAME) in REMOVED_COMPONENTS:
            application.remove(child)
    receiver = next(r for r in application.findall("receiver") if r.get(NAME) == RECEIVER)
    receiver.set(EXPORTED, "false")

    ET.indent(tree)
    dst.parent.mkdir(parents=True, exist_ok=True)
    tree.write(dst, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "work/apktool/AndroidManifest.xml"
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "work/patched/AndroidManifest.xml"
    main(src, dst)
    print(f"wrote {dst}")

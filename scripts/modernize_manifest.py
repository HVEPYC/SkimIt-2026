#!/usr/bin/env python3
"""Android 17 manifest step (M4): takes the offline manifest from M2 (work/patched/AndroidManifest.xml)
and makes it valid for a modern targetSdk: every component gets an explicit android:exported
(only the launcher activity is exported), and the original
Back-key handling is kept (predictive back off). The SDK levels are set through apktool.yml by
scripts/build_apk.sh. Gameplay-relevant attributes (orientation, configChanges) stay as shipped.

    modernize_manifest.py <in> <out>
"""
import sys
import xml.etree.ElementTree as ET

ANDROID = "http://schemas.android.com/apk/res/android"
A = lambda n: f"{{{ANDROID}}}{n}"


def main(src, dst):
    ET.register_namespace("android", ANDROID)
    tree = ET.parse(src)
    app = tree.getroot().find("application")
    # Back key: the original handles KEYCODE_BACK itself (exit confirmation dialog). With a modern
    # targetSdk, predictive back would finish the activity without delivering the key.
    app.set(A("enableOnBackInvokedCallback"), "false")
    # Immersive full screen (gesture bar hidden): our Application class, patches/java/src/org/skimport.
    if A("name") in app.attrib:
        raise SystemExit("unexpected: the original manifest already names an Application class")
    app.set(A("name"), "org.skimport.PortApplication")
    # No title bar: a modern targetSdk's default theme adds an action bar the 2013 player never had.
    if A("theme") not in app.attrib:
        app.set(A("theme"), "@android:style/Theme.Black.NoTitleBar.Fullscreen")
    for comp in app:
        if comp.tag not in ("activity", "receiver", "service", "provider"):
            continue
        launcher = any(c.get(A("name")) == "android.intent.category.LAUNCHER" for f in comp.findall("intent-filter") for c in f.findall("category"))
        if A("exported") not in comp.attrib:
            comp.set(A("exported"), "true" if launcher else "false")
    ET.indent(tree)
    tree.write(dst, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

#!/usr/bin/env python3
"""Puts the library art of a Steam shortcut in place.
   set-art.py --appid ID --art DIR [--grid GRIDDIR] [--icon-dest FILE] [--live]
 DIR holds p.png (portrait), wide.png, hero.png, logo.png, icon.png.
 --grid copies them where Steam looks on start (<appid>p.png, <appid>.png, <appid>_hero.png, <appid>_logo.png,
        <appid>_icon.png).
 --live sets them in the running Steam through its CEF port (steam-cef.py: SetCustomArtworkForApp, no restart)
        and, with --icon-dest, copies the icon there and points the shortcut's icon at it (SetShortcutIcon).
 Origin: the art calls of the Frame art notes (types 0 portrait, 1 hero, 2 logo, 3 wide)."""
import argparse, base64, os, shutil, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
KINDS = [("p", "p", 0), ("wide", "", 3), ("hero", "_hero", 1), ("logo", "_logo", 2), ("icon", "_icon", None)]

ap = argparse.ArgumentParser()
ap.add_argument("--appid", required=True)
ap.add_argument("--art", required=True)
ap.add_argument("--grid")
ap.add_argument("--icon-dest")
ap.add_argument("--live", action="store_true")
a = ap.parse_args()
appid = int(a.appid)

if a.grid:
    os.makedirs(a.grid, exist_ok=True)
    for name, suffix, _ in KINDS:
        src = os.path.join(a.art, name + ".png")
        if os.path.isfile(src):
            dst = os.path.join(a.grid, "%d%s.png" % (appid, suffix))
            shutil.copyfile(src, dst + ".new")
            os.replace(dst + ".new", dst)
            print("art: " + dst)
if a.live:
    from importlib.machinery import SourceFileLoader
    cef = SourceFileLoader("steam_cef", os.path.join(os.path.dirname(os.path.abspath(__file__)), "steam-cef.py")).load_module()
    for name, _, kind in KINDS:
        src = os.path.join(a.art, name + ".png")
        if kind is None or not os.path.isfile(src):
            continue
        b64 = base64.b64encode(open(src, "rb").read()).decode()
        cef.evaluate("(async () => { await SteamClient.Apps.SetCustomArtworkForApp(%d, '%s', 'png', %d); return 1; })()" % (appid, b64, kind))
        print("art live: %s" % name)
    if a.icon_dest and os.path.isfile(os.path.join(a.art, "icon.png")):
        os.makedirs(os.path.dirname(a.icon_dest), exist_ok=True)
        if os.path.abspath(a.icon_dest) != os.path.abspath(os.path.join(a.art, "icon.png")):
            shutil.copyfile(os.path.join(a.art, "icon.png"), a.icon_dest)
        cef.evaluate("(() => { SteamClient.Apps.SetShortcutIcon(%d, '%s'); return 1; })()" % (appid, a.icon_dest))
        print("icon live: " + a.icon_dest)

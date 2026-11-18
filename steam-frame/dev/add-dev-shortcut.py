#!/usr/bin/env python3
"""Adds (or updates) the "AwakeningFrame (Dev)" Steam library entry: it runs run.sh of this kit, has the DEV art
(art-dev/, made by make-dev-art.sh) and the same SteamVR per-app settings as the released entry (steam.app.<appid>.*: 120 Hz,
no throttling, no motion smoothing; the launcher sets them again at every launch). Steam must be running. Run it on the headset.
   add-dev-shortcut.py [--no-art]
Environment: AF_CEF_PORT (Steam's debugging port, 8080), AF_VRCMD (SteamVR's vrcmd), AF_STEAM_DIR (Steam's folder, for the grid art).
Uses steam-cef.py and set-art.py of the repository's steam-frame/ (copied next to this script in the kit)."""
import glob, os, subprocess, sys
from importlib.machinery import SourceFileLoader

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = next((d for d in (HERE, os.path.join(HERE, "app"), os.path.join(HERE, "..")) if os.path.isfile(os.path.join(d, "steam-cef.py"))), None)
if not TOOLS:
    sys.exit("add-dev-shortcut: steam-cef.py is not next to this script")
cef = SourceFileLoader("steam_cef", os.path.join(TOOLS, "steam-cef.py")).load_module()

NAME = "AwakeningFrame (Dev)"
EXE = os.path.join(HERE, "run.sh")
ART = os.path.join(HERE, "art-dev")
VRCMD = os.environ.get("AF_VRCMD", "/opt/steamvr/bin/linuxarm64/vrcmd")
STEAM = os.environ.get("AF_STEAM_DIR", os.path.expanduser("~/.local/share/Steam"))
q = lambda s: "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"

try:
    existing = cef.evaluate("(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === %s && a.app_type === 1073741824) return a.appid; } return 0; })()" % q(NAME))
except Exception as e:
    sys.exit("add-dev-shortcut: Steam's debugging port is not reachable (is Steam running?): %s" % e)
if existing:
    appid = existing
    print("Steam already has '%s' (appid %s), updating it" % (NAME, appid))
    cef.evaluate("(() => { SteamClient.Apps.SetShortcutExe(%d, %s); SteamClient.Apps.SetShortcutStartDir(%d, %s); return 1; })()"
                 % (appid, q('"' + EXE + '"'), appid, q('"' + HERE + '"')))
else:
    appid = cef.evaluate("(async () => { const id = await SteamClient.Apps.AddShortcut(%s, %s, %s, ''); SteamClient.Apps.SetShortcutName(id, %s); return id; })()"
                         % (q(NAME), q(EXE), q(HERE), q(NAME)))
    print("added '%s' (appid %s)" % (NAME, appid))

# SteamVR's per-app settings, as the launcher sets them for its own appid (SteamVR drops them at times; needs SteamVR running)
if os.access(VRCMD, os.X_OK):
    for kv in ("preferredRefreshRate=120", "framesToThrottle=0", "additionalFramesToPredict=0", "motionSmoothingOverride=2"):
        k, v = kv.split("=")
        if subprocess.run([VRCMD, "--set-settings-int", "steam.app.%s.%s" % (appid, k), v], capture_output=True).returncode != 0:
            print("warning: could not set SteamVR's %s (is SteamVR running? the launcher sets it again at every start)" % k)

if "--no-art" not in sys.argv:
    if not os.path.isfile(os.path.join(ART, "p.png")):
        sys.exit("add-dev-shortcut: no art in %s (make-dev-art.sh %s)" % (ART, ART))
    args = [sys.executable, "-I", os.path.join(TOOLS, "set-art.py"), "--appid", str(appid), "--art", ART, "--live", "--icon-dest", os.path.join(ART, "icon.png")]
    for cfg in sorted(glob.glob(os.path.join(STEAM, "userdata", "*", "config"))):
        if cfg.endswith(os.sep + "userdata" + os.sep + "0" + os.sep + "config"):
            continue
        args += ["--grid", os.path.join(cfg, "grid")]
        if os.path.isfile(os.path.join(cfg, "shortcuts.vdf")):
            break
    if subprocess.run(args).returncode != 0:
        print("warning: could not set the library art")
print("appid", appid)

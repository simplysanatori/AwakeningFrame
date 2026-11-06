#!/usr/bin/env python3
"""Runs Frametanium's own install and remove code on a local release archive, in a scratch home folder.

  frametanium-sandbox.py MANAGER.py ARCHIVE.tar.gz install|remove [TAG]

HOME (and the XDG folders) must already point at the scratch home: Frametanium's manager.py reads its catalog,
installed list and folders from there, so the real ones are never read or written. The release list GitHub would
return is replaced by one release whose download is the local archive (a file:// URL); everything else is
Frametanium's code as it is: the asset choice, the unpacking, its installer mode (install.sh with stdin from
/dev/null, cwd = the app's folder, its 15-minute limit) and, on remove, uninstall.sh. Prints the job's final state
as JSON and exits 1 when it is an error."""
import importlib.util, json, os, sys, time

manager_py, archive, action = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
tag = sys.argv[4] if len(sys.argv) > 4 else "v0.0.0-test"
home = os.environ["HOME"]
if not home.startswith(("/tmp/", os.environ.get("AF_SANDBOX_ROOT") or "/nonexistent")):
    sys.exit("HOME must be a scratch folder (under /tmp or AF_SANDBOX_ROOT), not " + home)

spec = importlib.util.spec_from_file_location("manager", manager_py)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
assert str(m.CONFIG_DIR).startswith(home), m.CONFIG_DIR

PID = "awakeningframe"
os.makedirs(m.CONFIG_DIR, exist_ok=True)
if not os.path.exists(m.USER_CATALOG):
    # (no asset pattern: Frametanium picks the download itself, as for a repo added by name)
    json.dump([{"id": PID, "name": "AwakeningFrame", "repo": "simplysanatori/AwakeningFrame", "mode": "auto"}],
              open(m.USER_CATALOG, "w"), indent=1)

name = os.path.basename(archive)
fake = [{"tag_name": tag, "name": tag, "published_at": "2026-11-06T00:00:00Z", "prerelease": False, "draft": False,
         "body": "", "html_url": "https://github.com/simplysanatori/AwakeningFrame/releases/tag/" + tag,
         "assets": [{"name": name, "size": os.path.getsize(archive), "url": "file://" + archive,
                     "browser_download_url": "file://" + archive},
                    {"name": name.replace("awakeningframe-", "AwakeningFrame-").replace("-linux-arm64.tar.gz",
                     "-framedrop-linux-arm64.zip"), "size": 1, "url": "file:///dev/null", "browser_download_url": "file:///dev/null"},
                    {"name": "awakeningframe.framedrop.json", "size": 1, "url": "file:///dev/null", "browser_download_url": "file:///dev/null"},
                    {"name": "SHA256SUMS", "size": 300, "url": "file:///dev/null", "browser_download_url": "file:///dev/null"}]}]
real_gh_json = m.gh_json
m.gh_json = lambda url: fake if "/releases" in url else real_gh_json(url)

t0 = time.time()
if action == "install":
    m.install(PID)
elif action == "remove":
    m.remove(PID)
else:
    sys.exit("install or remove")
st = m.jobs.state.get(PID) or m.jobs.state.get("_list") or {}
st["seconds"] = round(time.time() - t0, 1)
st["installed"] = m.load_json(m.INSTALLED_FILE, {}).get(PID)
st["chosen"] = m.choose_asset(m.catalog().get(PID, {}), fake[0]["assets"])["name"] if PID in m.catalog() else None
print(json.dumps(st, indent=1))
sys.exit(1 if st.get("status") == "error" else 0)

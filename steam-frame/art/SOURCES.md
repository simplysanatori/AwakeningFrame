# Library art sources

The AwakeningFrame library entry uses community artwork from [SteamGridDB](https://www.steamgriddb.com) for
"The Legend of Zelda: Link's Awakening", the 2019 Nintendo Switch remake (SteamGridDB game id 5253316; not the
Game Boy / DX entries). The images are NOT committed here (they are Nintendo artwork); this file records which
assets were used so the set can be fetched again. All are static, SFW and official or official-style.

| File | Kind | SGDB asset id | Author | Size | URL |
|---|---|---|---|---|---|
| `p.png` | portrait capsule | 692854 | Jorolesvaldo | 600 x 900 | https://cdn2.steamgriddb.com/grid/62e42d5128071bb96fc3a269019e0504.png |
| `wide.png` | wide capsule | 692848 | Jorolesvaldo | 920 x 430 | https://cdn2.steamgriddb.com/grid/ad82189644758f22c3c842e87bcaccf3.png |
| `hero.png` | hero | 36304 | Supreme Ricardo | 3840 x 1240 | https://cdn2.steamgriddb.com/hero/5dbafbb281f14d2e2608f35e74d97b46.png |
| `logo.png` | logo (white, transparent) | 164714 | Celthium | 2053 x 825 | https://cdn2.steamgriddb.com/logo/7aec550dfc20510345a10985bd3a0862.png |
| `icon.png` | icon (Link) | 103221 | Celthium | 1024 x 1024 | https://cdn2.steamgriddb.com/icon/26bb3de612b829fb5b33aea3193b0eba.png |

## Frame version

`make-art.py SRC OUT` turns the originals into the Frame set: a small "FRAME" pill on the portrait and the wide
capsule, and a letterspaced FRAME line under the logo (the same place other ports' logos carry PRIME, LEGEND or
COMBAT EVOLVED). The hero is used as is, and the icon is the full-square Link portrait resized to 512 x 512.

To install them: download the URLs above into a folder under the names in the first column, run
`python3 make-art.py THAT_FOLDER steam-frame/art/`, then `install.sh`, or apply them to a running Steam with
`set-art.py --appid ID --art OUT --live`. Without them, `install.sh` uses the placeholder `*.png` in this folder
(original shapes and the app name only).

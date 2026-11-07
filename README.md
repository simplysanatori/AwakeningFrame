# AwakeningFrame

The Legend of Zelda: Link's Awakening in VR, as a tabletop diorama, running natively on the Valve Steam Frame.
It is a port of [LAS Recomp](https://github.com/peiikai/LASRecomp) by peiikai, the static recompilation of the 2019 Switch game.
Version 0.1.0 is a pre-release: see the [known issues](docs/USER_GUIDE.md#known-issues).

## Features

- The island as a diorama on a table in front of you
- 120 Hz, both eyes drawn in one pass
- Steam Frame controllers as a Switch Pro Controller, with rumble
- Eye-tracked foveation and dynamic resolution
- HUD and menus on a floating panel

## How it works

The game's own code, recompiled, runs on the Frame itself, inside SteamVR.
The installer builds it from your own game files.
Nothing is streamed from a PC.

## Requirements

- A Steam Frame with SteamOS and SteamVR, and about 14 GB free
- Your own game files and keys: the base game 1.0.0 as an NSP or XCI, and `prod.keys`
- Steam running while you install

## Install

1. Put your NSP or XCI and `prod.keys` in `~/Downloads`.

2. Download `awakeningframe-0.1.0-linux-arm64.tar.gz` and `SHA256SUMS` from [Releases](https://github.com/simplysanatori/AwakeningFrame/releases) to `~/Downloads`. In a terminal, check and install:

   ```sh
   cd ~/Downloads
   sha256sum -c --ignore-missing SHA256SUMS
   mkdir af && tar -xzf awakeningframe-0.1.0-linux-arm64.tar.gz -C af
   af/install.sh
   ```

   It builds the game code from your files in up to 15 minutes.

3. Launch **AwakeningFrame** from your Steam library.

Other installers (you still need step 1):

- [Frametanium](https://github.com/Cogential/frametanium): add `simplysanatori/AwakeningFrame`.
- [FrameDrop](https://framedropvr.com): [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FAwakeningFrame%2Freleases%2Fdownload%2Fv0.1.0%2Fawakeningframe.framedrop.json).

## First run

The first start prepares the shaders and takes longer. The table appears a little below eye height. To move it, press **View** and **Menu** together and use the **VR** tab.

## Controls

| Steam Frame | In the game |
|---|---|
| Left stick | Walk |
| Bottom face button | Sword (B) |
| Right face button | Talk, lift (A) |
| Left, top face button | Items (Y, X) |
| Left bumper | Pegasus Boots (L) |
| Right bumper or grip | Shield (R) |
| **Menu**, **View**, both together | Pause menu, map, LAS Recomp's menu |
| Right stick | Turn and tilt the table |
| Left grip + D-pad | Table height and size |
| Both stick clicks | Reset the view. Hold 2 s for a full reset |

Buttons match a Switch Pro Controller by position. All controls: [user guide](docs/USER_GUIDE.md#controls).

## Update

Update in Frametanium, or install the new tarball as in step 2.

## Uninstall

Quit the game first. Add `--purge` to also delete game files, saves and settings.

```sh
~/Games/AwakeningFrame/app/uninstall.sh
```

## Troubleshooting

- **Steam's debugging port is not reachable.** Start Steam and run `install.sh` again.
- **The start screen shows instead of the game.** Put your game files and keys in `~/Downloads` and launch again.
- **Motion ghosts.** In SteamVR, check 120 Hz and motion smoothing off.
- **Frametanium says no game files were found.** Put them in `~/Downloads` and launch **AwakeningFrame**.

More fixes: [user guide](docs/USER_GUIDE.md#troubleshooting).

## Reporting a problem

Attach the newest `run-<time>.log` and `lasrecomp-<time>.log` from `~/Games/AwakeningFrame/logs` to an [issue](https://github.com/simplysanatori/AwakeningFrame/issues).
They hold paths under your home folder. Check them before sharing.

## More

- [User guide](docs/USER_GUIDE.md)
- [AGENTS.md](AGENTS.md): for developers and AI agents
- [Building from source](AGENTS.md#build-from-source)
- [Changelog](docs/CHANGELOG.md)

## Support

Support peiikai's [LAS Recomp](https://github.com/peiikai/LASRecomp) first: star it, report bugs there, send improvements upstream.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori)
- [LAS Recomp](https://github.com/peiikai/LASRecomp) by peiikai and contributors: the recompilation this is built on
- [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus) by Nycolazs: Turnip and Adreno fixes
- [hactool](https://github.com/SciresM/hactool) by SciresM: game extraction

Full list: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Unofficial fan project, not affiliated with Nintendo, Valve or the LAS Recomp authors.

## Licence

MIT, same as upstream. See [LICENSE](LICENSE). Third-party parts keep their own licences.

## How this was made

I started this project and used AI (Claude) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. LAS Recomp, the upstream, is also AI-written.

"""Native settings regression checks; no game files or player settings are used."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    exe = directory / ('settings-test.exe' if os.name == 'nt' else 'settings-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-Iruntime', 'runtime/settings.c', 'runtime/cosmetic_colour.c', 'runtime/vr_settings.c', 'tools/test_settings.c', '-o', str(exe), '-lm'] +
                   (['-framework', 'CoreFoundation'] if sys.platform == 'darwin' else []),
                   cwd=ROOT, check=True)
    cases = [(setting, {}, code) for setting, code in
             [('zh-CN', 'zh-Hans'), ('zh-TW', 'zh-Hant'), ('zh-Hans', 'zh-Hans'),
              ('zh-Hant', 'zh-Hant'), ('en-US', 'en-US'), ('ja', 'ja'), ('unknown', 'en-US'),
              ('pt-BR', 'en-US')]]
    if sys.platform.startswith('linux'):
        for locale in ['zh_CN.UTF-8', 'zh_SG.UTF-8', 'zh-Hans', 'zh-Hans-HK', 'zh_Hans_CN', 'zh']:
            cases.append(('auto', {'LC_ALL': locale}, 'zh-Hans'))
        for locale in ['zh_TW.UTF-8', 'zh_HK.UTF-8', 'zh_MO.UTF-8', 'zh-HK', 'zh-MO',
                       'zh-Hant', 'zh-Hant-TW', 'zh_Hant_CN', 'zh_hant', 'zh_hk.UTF-8']:
            cases.append(('auto', {'LC_ALL': locale}, 'zh-Hant'))
        cases += [('auto', {'LC_MESSAGES': 'zh_HK.UTF-8', 'LANG': 'zh_CN.UTF-8'}, 'zh-Hant'),
                  ('auto', {'LANG': 'zh_MO.UTF-8'}, 'zh-Hant'),
                  ('auto', {'LC_ALL': 'zh_CN.UTF-8', 'LC_MESSAGES': 'zh_HK.UTF-8'}, 'zh-Hans'),
                  ('auto', {'LC_ALL': 'ja_JP.UTF-8'}, 'ja'), ('auto', {'LC_ALL': 'C'}, 'en-US'),
                  ('auto', {'LC_ALL': 'pt_BR.UTF-8'}, 'en-US')]
    count = 0
    for newline in ['\n', '\r\n']:
        for setting, locale, code in cases:
            for seed in ['Regression seed', '']:
                ini = directory / 'test.ini'
                lines = [f'language = {setting}', 'aspect = original', 'upscaler = bilinear',
                         'prompts = xbox', 'volume = 1.25', 'volume =', 'language =', f'rando_seed = {seed}']
                ini.write_bytes((newline.join(lines) + newline).encode())
                env = dict(os.environ, LARECOMP_SETTINGS=str(ini), LC_ALL='', LC_MESSAGES='', LANG='C')
                env.update(locale)
                subprocess.run([str(exe), code, seed], env=env, check=True, capture_output=True)
                saved = ini.read_text()
                expected = 'auto' if setting in ['auto', 'unknown'] else setting if setting == 'pt-BR' else code  # (the menus' only)
                assert f'language = {expected}\n' in saved
                assert f'rando_seed = {seed}\n' in saved
                count += 1
    # the upscaler's default follows the GPU until the player picks one (settings_resolve_defaults)
    for lines, mode in [(['upscaler = auto'], 'upscaler-auto'), (['volume = 1'], 'upscaler-auto'),
                        (['upscaler = fsr1'], 'upscaler-chosen'), (['upscaler = metalfx'], 'upscaler-chosen')]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines) + '\n')
        subprocess.run([str(exe), mode], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True, capture_output=True)
        if mode == 'upscaler-auto':
            assert 'upscaler = auto\n' in ini.read_text()
        count += 1
    # likewise the resolution: Match display until the player picks one (also on integrated GPUs); a saved one is kept
    for lines, mode, saved in [(['resolution = auto'], 'res-auto', 'auto'), (['volume = 1'], 'res-auto', 'auto'),
                               (['resolution = match'], 'res-chosen', 'match'), (['resolution = 1x'], 'res-chosen', '1x'),
                               (['resolution = 75%'], 'res-chosen', '75%'), (['resolution = dynamic'], 'res-chosen', 'dynamic')]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines) + '\n')
        env = dict(os.environ, LARECOMP_SETTINGS=str(ini))
        env.pop('LARECOMP_INTEGRATED', None)
        subprocess.run([str(exe), mode], env=env, check=True, capture_output=True)
        assert f'resolution = {saved}\n' in ini.read_text(), (lines, ini.read_text())
        count += 1
    # VR tab's Refresh Rate: vr_refresh (what the wrapper reads) + vr_fps; no keys = 120 Hz (60 FPS); the third choice
    # (90 Hz, 60 FPS, Test) puts LARECOMP_XR_FPS=60, unless the environment has it; either order of the two keys
    for lines, index, fps, saved in [([], 0, '-', ['vr_refresh = 120', 'vr_fps = 60']),
                                     (['vr_refresh = 90'], 1, '-', ['vr_refresh = 90', 'vr_fps = 30']),
                                     (['vr_refresh = 120'], 0, '-', ['vr_refresh = 120', 'vr_fps = 60']),
                                     (['vr_refresh = 90', 'vr_fps = 60'], 2, '60', ['vr_refresh = 90', 'vr_fps = 60']),
                                     (['vr_fps = 60', 'vr_refresh = 90'], 2, '60', ['vr_refresh = 90', 'vr_fps = 60']),
                                     (['vr_refresh = 90', 'vr_fps = 30'], 1, '-', ['vr_refresh = 90', 'vr_fps = 30']),
                                     (['vr_refresh = 120', 'vr_fps = 60'], 0, '-', ['vr_refresh = 120', 'vr_fps = 60'])]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines + ['volume = 1']) + '\n')
        env = {k: v for k, v in os.environ.items() if k not in ('LARECOMP_XR_FPS', 'LARECOMP_XR_HZ')}
        env['LARECOMP_SETTINGS'] = str(ini)
        subprocess.run([str(exe), 'vr-refresh', str(index), fps], env=env, check=True, capture_output=True)
        for line in saved:
            assert line + '\n' in ini.read_text(), (lines, ini.read_text())
        count += 1
    # (the environment's LARECOMP_XR_FPS wins over the ini's vr_fps)
    ini = directory / 'test.ini'
    ini.write_text('vr_refresh = 90\nvr_fps = 60\n')
    env = {k: v for k, v in os.environ.items() if k != 'LARECOMP_XR_HZ'}
    env.update(LARECOMP_SETTINGS=str(ini), LARECOMP_XR_FPS='30')
    subprocess.run([str(exe), 'vr-refresh', '2', '30'], env=env, check=True, capture_output=True)
    count += 1
    # VR tab's Turning, Table Edge, Foveation and Dynamic Resolution: the defaults (smooth, off, high, on: foveation and
    # dynamic resolution are passed on always, for the code's own defaults are medium and off); a saved choice; the new
    # edge key (vr_edge_style) only: the old vr_edge_mode / vr_edge values (haze, sky only) and vr_culling are dropped
    vr_env = ('LARECOMP_VR_EDGE', 'LARECOMP_VR_TURN', 'LARECOMP_VR_FOVEATION', 'LARECOMP_VR_DYNRES', 'LARECOMP_VR_CULL')
    for lines, edge, turn, evars, saved in [
            ([], 0, 0, ['-', '-', 'high', '1'], ['vr_turn = smooth', 'vr_edge_style = black', 'vr_foveation = high', 'vr_dynamic_resolution = 1']),
            (['vr_edge_style = fade', 'vr_turn = snap', 'vr_foveation = low', 'vr_dynamic_resolution = 0'], 1, 1,
             ['fade', 'snap', 'low', '0'], ['vr_turn = snap', 'vr_edge_style = fade', 'vr_foveation = low', 'vr_dynamic_resolution = 0']),
            (['vr_edge_style = passthrough'], 2, 0, ['passthrough', '-', 'high', '1'], ['vr_edge_style = passthrough']),
            (['vr_edge_mode = fog', 'vr_edge = gradient', 'vr_culling = wide', 'vr_foveation = off'], 0, 0,
             ['-', '-', 'off', '1'], ['vr_edge_style = black']),
            (['vr_edge_style = haze', 'vr_turn = spin'], 0, 0, ['-', '-', 'high', '1'], ['vr_edge_style = black', 'vr_turn = smooth'])]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines + ['volume = 1']) + '\n')
        env = {k: v for k, v in os.environ.items() if k not in vr_env}
        env['LARECOMP_SETTINGS'] = str(ini)
        subprocess.run([str(exe), 'vr-opts', str(edge), str(turn)] + evars, env=env, check=True, capture_output=True)
        for line in saved:
            assert line + '\n' in ini.read_text(), (lines, ini.read_text())
        assert 'vr_culling' not in ini.read_text() and 'vr_edge_mode' not in ini.read_text()
        count += 1
    # (the environment wins over the ini for each of them)
    ini = directory / 'test.ini'
    ini.write_text('vr_edge_style = fade\nvr_turn = snap\nvr_foveation = low\nvr_dynamic_resolution = 0\n')
    env = {k: v for k, v in os.environ.items() if k not in vr_env}
    env.update(LARECOMP_SETTINGS=str(ini), LARECOMP_VR_EDGE='passthrough', LARECOMP_VR_TURN='smooth', LARECOMP_VR_FOVEATION='medium',
               LARECOMP_VR_DYNRES='1')
    subprocess.run([str(exe), 'vr-opts', '1', '1', 'passthrough', 'smooth', 'medium', '1'], env=env, check=True, capture_output=True)
    count += 1
    # Cosmetics colour rows (shared palette): older names map onto it, Custom is a hex value, unknown -> Default
    ini = directory / 'test.ini'
    ini.write_text('tunic = Yellow\nhearts = Pink\nhair = #12ab34\nshield = Sparkly\nskin = Random\n')
    subprocess.run([str(exe), 'colours'], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True, capture_output=True)
    saved = ini.read_text()
    for line in ['tunic = Gold', 'hearts = Pink', 'hair = #12AB34', 'shield = Default', 'skin = Random']:
        assert line + '\n' in saved, (line, saved)
    count += 1
    # the Randomizer tab's New seed options: an older file without the key stays without it (the defaults)
    for lines, value in [(['rando_options = 50000-00004-20000-40G20-04000-0G11R', 'rando_seed = X'],
                          '50000-00004-20000-40G20-04000-0G11R'), (['rando_seed = X'], '')]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines) + '\n')
        subprocess.run([str(exe), 'rando-options', value], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True,
                       capture_output=True)
        saved = ini.read_text()
        assert (f'rando_options = {value}\n' in saved) if value else 'rando_options' not in saved, saved
        count += 1
    print(f'{count} parser, locale, upscaler / resolution default and roundtrip cases passed (LF/CRLF, empty/nonempty seeds)')

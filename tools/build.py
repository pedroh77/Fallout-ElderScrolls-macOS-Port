#!/usr/bin/env python3
"""Build a native macOS arm64 .app bundle with Xcode / Command Line Tools."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile
from state_probes import normalize, instrument
from native_codecs import validate as validate_codecs
from app_icon import compile_icon

ROOT = Path(__file__).resolve().parents[1]
R = ROOT / 'upstream/recomp'
H = R / 'host'
M = ROOT / 'macos'


def run(cmd):
    subprocess.run(cmd, check=True)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def host_deployment_target():
    ver = platform.mac_ver()[0]
    try:
        major = int(ver.split('.')[0])
    except (ValueError, IndexError):
        return '14.0'
    return f'{max(major, 14)}.0'


def find_unicorn(explicit):
    if explicit:
        return explicit
    for candidate in ['/opt/homebrew/opt/unicorn', '/opt/homebrew', '/usr/local/opt/unicorn', '/usr/local']:
        p = Path(candidate)
        if (p / 'include/unicorn/unicorn.h').is_file() and (p / 'lib/libunicorn.a').is_file():
            return p
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('target', choices=['probe', 'game', 'host-check'])
    p.add_argument('--game', choices=['fnv', 'fallout3'], default='fnv', help='target game profile')
    p.add_argument('--generated', type=Path, help='full-game emit.py output')
    p.add_argument('--exe', type=Path, help='exact executable used to generate these units')
    p.add_argument('--out', type=Path, default=Path('build'))
    p.add_argument('--app-name', default=None, help='bundle display name without .app')
    p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    p.add_argument('--cache', type=Path, help='object cache keyed by compiler, SDK, flags, headers and content')
    p.add_argument('--state-probes', default='[]', help='bounded numeric entry/receiver-field diagnostic specification')
    p.add_argument('--codec-prefix', type=Path, help='verified pinned macOS Ogg/Vorbis static libraries')
    p.add_argument('--unicorn-prefix', type=Path, help='optional Unicorn engine prefix for fallback interpreter')
    p.add_argument('--no-unicorn', action='store_true', help='disable Unicorn fallback interpreter')
    p.add_argument('--steam-api-dylib', type=Path, help='optional Valve ARM64 libsteam_api.dylib to bundle in Frameworks/')
    p.add_argument('--steam-appid', default='22380', help='Steam AppID written to steam_appid.txt when bundling libsteam_api.dylib')
    p.add_argument('--no-call-trace', action='store_true', help='omit per-function diagnostic ring writes')
    p.add_argument('--deployment-target', default=None, help='macOS deployment target (default: host major.0, min 14.0)')
    p.add_argument('--version', default=os.environ.get('GITHUB_RUN_NUMBER', '1'), help='positive numeric app build version')
    args = p.parse_args()

    if not args.version.isdecimal() or not 0 < int(args.version) <= 2147483647:
        p.error('--version must be a positive 32-bit integer')
    probes = normalize(args.state_probes)
    matched_probes = set()
    probe = args.target == 'probe'
    host_check = args.target == 'host-check'
    codec = validate_codecs(args.codec_prefix, 'macos') if args.codec_prefix else None
    if args.target == 'game' and not codec:
        p.error('game requires --codec-prefix for native sound decoding')
    if args.target == 'game' and not (args.exe and args.exe.is_file() and args.generated and
                                      (args.generated / 'table.c').is_file() and
                                      (args.generated / 'stubs.c').is_file() and
                                      any(args.generated.glob('tu_*.c'))):
        p.error('game requires --exe and --generated with full translated C sources')
    if not shutil.which('xcrun'):
        p.exit(1, 'Apple build required: install Xcode Command Line Tools (xcode-select --install).\n')

    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    cc = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--find', 'clang'], text=True).strip()

    unicorn = None if (probe or args.no_unicorn) else find_unicorn(args.unicorn_prefix)
    if unicorn:
        print(f'Unicorn fallback enabled: {unicorn}', flush=True)

    signature = hashlib.sha256(subprocess.check_output([cc, '--version']) + sdk.encode())
    for header in sorted({hdr for directory in [R, M] for hdr in directory.rglob('*.h')}):
        signature.update(str(header.relative_to(ROOT)).encode() + header.read_bytes())
    config = signature.digest()

    old_manifest = {}
    if args.cache:
        args.cache.mkdir(parents=True, exist_ok=True)
        if (args.cache / 'manifest.json').exists():
            old_manifest = json.loads((args.cache / 'manifest.json').read_text())

    default_app = 'Fallout 3' if args.game == 'fallout3' else 'Fallout New Vegas'
    app_name = args.app_name or ('FNVProbe' if probe else default_app)
    exe_name = 'FNVProbe' if probe else ('Fallout3' if args.game == 'fallout3' else 'FalloutNewVegas')

    sources = [M / 'FNVApp.m', M / 'FNVPad.c', M / 'FNVDisplay.c', H / 'd3d9_metal.m']
    if probe:
        sources += [M / 'FNVProbe.c', M / 'ProbeRuntime.c']
        sources += [H / (s + '.c') for s in ['d3d9', 'dinput', 'com', 'gheap', 'input', 'sm2msl']]
    else:
        sources += [M / 'FNVAudioMixer.c', M / 'FNVNativeMedia.m', M / 'FNVNativeVideo.m']
        sources += [f for f in H.glob('*.c') if f.name not in ('d3d9_null.c', 'fallback.c')]
        if unicorn:
            sources.append(H / 'fallback.c')
        sources += [H / 'dshow.m', R / 'rt_dispatch.c']
        if not host_check:
            sources += [args.generated / 'table.c', args.generated / 'stubs.c']
            sources += sorted(args.generated.glob('tu_*.c'))

    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out) as temp:
        tmp = Path(temp)
        bundle = tmp / (app_name + '.app')
        contents = bundle / 'Contents'
        macos_dir = contents / 'MacOS'
        resources_dir = contents / 'Resources'
        macos_dir.mkdir(parents=True)
        resources_dir.mkdir(parents=True)

        deploy_target = args.deployment_target or host_deployment_target()
        base = [cc, '-target', f'arm64-apple-macos{deploy_target}', '-isysroot', sdk]

        def compile_one(item):
            idx, source = item
            out = tmp / f'{idx}.o'
            content = source.read_bytes()
            matched = set()
            if probes and args.generated and source.parent.resolve() == args.generated.resolve():
                content, matched = instrument(content, probes)
                matched_probes.update(matched)
            compiled_source = source
            if matched:
                compiled_source = tmp / ('probe-' + source.name)
                compiled_source.write_bytes(content)
            flags = ['-O1', '-I', str(H), '-I', str(R), '-I', str(M)]
            if args.game == 'fallout3':
                flags.append('-DFNV_GAME_FALLOUT3')
            if codec:
                flags += ['-DFNV_HAVE_VORBIS', '-I', str(codec / 'include')]
            if unicorn and source.name == 'fallback.c':
                flags += ['-I', str(unicorn / 'include')]
            if source.suffix == '.m':
                flags.append('-fobjc-arc')
            if probe:
                flags.append('-DFNV_PROBE')
            elif not host_check and not args.no_call_trace:
                flags.append('-DRT_CALLTRACE')
            key = hashlib.sha256(config + json.dumps(base + flags).encode() + source.name.encode() + content).hexdigest()
            stored = args.cache / (key + '.o') if args.cache else None
            if stored and stored.exists() and digest(stored) == old_manifest.get(stored.name):
                return str(stored), True
            run(base + flags + ['-c', str(compiled_source), '-o', str(out)])
            if stored:
                os.replace(out, stored)
                out = stored
            return str(out), False

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            compiled = list(pool.map(compile_one, enumerate(sources)))
        objects = [path for path, _ in compiled]
        if probes and set(probes) != matched_probes:
            raise ValueError('Requested diagnostic entries absent from generated source')
        print(f'macOS objects: {sum(reused for _, reused in compiled)} reused, {sum(not reused for _, reused in compiled)} compiled', flush=True)
        if args.cache:
            (args.cache / 'manifest.json').write_text(json.dumps({Path(path).name: digest(Path(path)) for path in objects}, indent=2))
        if host_check:
            print(f'PASS: compiled {len(objects)} public game-host sources for arm64 macOS; no game inputs')
            return

        frameworks = [
            'Foundation', 'AppKit', 'Metal', 'QuartzCore', 'CoreGraphics',
            'AVFoundation', 'CoreVideo', 'CoreMedia', 'ImageIO',
            'UniformTypeIdentifiers', 'GameController',
        ]
        libraries = [str(codec / 'lib' / ('lib' + n + '.a')) for n in ['vorbisfile', 'vorbis', 'ogg']] if codec else []
        if unicorn:
            libraries.append(str(unicorn / 'lib/libunicorn.a'))

        link_flags = ['-Wl,-stack_size,0x4000000']
        run(base + objects + libraries + [v for f in frameworks for v in ('-framework', f)] + link_flags + ['-o', str(macos_dir / exe_name)])

        bundle_exe = exe_name
        if codec:
            shutil.copytree(codec / 'licenses', resources_dir / 'Licenses')
        if not probe and args.steam_api_dylib and args.steam_api_dylib.is_file():
            frameworks_dir = contents / 'Frameworks'
            frameworks_dir.mkdir(exist_ok=True)
            shutil.copy2(args.steam_api_dylib, frameworks_dir / 'libsteam_api.dylib')
            (macos_dir / 'steam_appid.txt').write_text(str(args.steam_appid).strip() + '\n')
            launcher_name = exe_name + 'Launcher'
            run(base + ['-O2', '-Wall', '-Wextra',
                        '-DFNV_STEAM_GAME_NAME="' + exe_name + '"',
                        '-DFNV_STEAM_DEFAULT_APPID="' + str(args.steam_appid).strip() + '"',
                        str(M / 'FNVSteamLauncher.c'), '-o', str(macos_dir / launcher_name)])
            bundle_exe = launcher_name

        # a dedicated Fallout 3 identifier: macOS keys its per-app icon caches by bundle id,
        # and the old shared dev.f3mac.game identity kept serving a stale Game Mode icon
        bundle_id = ('dev.f3mac.fallout3' if args.game == 'fallout3'
                     else 'dev.fnvmac.' + args.target)
        info = dict(
            CFBundleDevelopmentRegion='en',
            CFBundleExecutable=bundle_exe,
            CFBundleName=app_name,
            CFBundleDisplayName=app_name,
            CFBundleIdentifier=bundle_id,
            CFBundlePackageType='APPL',
            CFBundleVersion=args.version,
            CFBundleShortVersionString='0.1',
            CFBundleInfoDictionaryVersion='6.0',
            LSMinimumSystemVersion=deploy_target,
            LSApplicationCategoryType='public.app-category.games',
            NSHighResolutionCapable=True,
            GCSupportedGameControllers=[{'ProfileName': 'ExtendedGamepad'}],
        )
        if not probe:
            info['FNVExecutableSHA256'] = digest(args.exe)
        info['FNVGuestCallTraceEnabled'] = bool(not probe and not args.no_call_trace)
        info['FNVOptimizationLevel'] = 'O1'
        icon_source = M / 'Assets' / ('AppIcon-Fallout3.png' if args.game == 'fallout3' else 'AppIcon.png')
        info.update(compile_icon(tmp, resources_dir, icon_source))
        try:
            info['FNVBuildSourceCommit'] = subprocess.check_output(
                ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            pass

        (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
        (contents / 'PkgInfo').write_text('APPL????')

        subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(bundle)], check=True)

        final_app = args.out / (app_name + '.app')
        if final_app.exists():
            shutil.rmtree(final_app)
        shutil.move(str(bundle), str(final_app))
        print(final_app.resolve())


if __name__ == '__main__':
    main()

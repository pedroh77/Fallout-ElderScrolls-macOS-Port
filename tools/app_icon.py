"""Compile the macOS .icns app icon from a PNG asset (default: macos/Assets/AppIcon.png)."""
from pathlib import Path
import shutil
import struct
import subprocess

ASSETS_DIR = Path(__file__).resolve().parents[1] / 'macos/Assets'
DEFAULT_SOURCE = ASSETS_DIR / 'AppIcon.png'
SRGB_PROFILE = '/System/Library/ColorSync/Profiles/sRGB Profile.icc'

_ICONSET_SIZES = [
    (16, 'icon_16x16.png'),
    (32, 'icon_16x16@2x.png'),
    (32, 'icon_32x32.png'),
    (64, 'icon_32x32@2x.png'),
    (128, 'icon_128x128.png'),
    (256, 'icon_128x128@2x.png'),
    (256, 'icon_256x256.png'),
    (512, 'icon_256x256@2x.png'),
    (512, 'icon_512x512.png'),
    (1024, 'icon_512x512@2x.png'),
]


def compile_icon(work, resources_dir, source=None):
    work, resources_dir = Path(work), Path(resources_dir)
    source = Path(source) if source else DEFAULT_SOURCE
    resources_dir.mkdir(parents=True, exist_ok=True)
    icns = resources_dir / 'AppIcon.icns'
    if source.suffix.lower() == '.icns':
        data = source.read_bytes()
        assert data[:4] == b'icns', f'Expected valid .icns file at {source}'
        shutil.copy2(source, icns)
        assert icns.is_file() and icns.stat().st_size > 0
        return {'CFBundleIconFile': 'AppIcon'}
    data = source.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', f'Expected a PNG icon at {source}'
    width, height = struct.unpack_from('>II', data, 16)
    assert width == height and 1024 <= width <= 8192, 'Expected a >=1024x1024 square icon'

    iconset = work / 'AppIcon.iconset'
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True, exist_ok=True)
    for px, name in _ICONSET_SIZES:
        # match the tagged sRGB profile explicitly so the icon does not shift colour
        # in Finder vs. the source image
        subprocess.run(
            ['sips', '--matchTo', SRGB_PROFILE, '-s', 'format', 'png',
             '--resampleHeightWidth', str(px), str(px),
             str(source), '--out', str(iconset / name)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
    subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(icns)], check=True)
    assert icns.is_file() and icns.stat().st_size > 0
    return {'CFBundleIconFile': 'AppIcon'}

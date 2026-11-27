"""Exercise Metal hardware occlusion & event queries through the D3D9 COM shim."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
host = root / 'upstream/recomp/host'
with tempfile.TemporaryDirectory(prefix='fnv-occ-query-') as temp:
    work = Path(temp)
    binary = work / 'occlusion-query-test'
    common = ['xcrun', 'clang', '-O1', '-DFNV_PROBE', '-I', str(root / 'macos'),
              '-I', str(host), '-I', str(root / 'upstream/recomp')]
    objects = []
    sources = [root / 'tests/occlusion_query_test.m', root / 'macos/FNVDisplay.c',
               *[host / (name + '.c') for name in ['d3d9', 'gheap', 'com', 'sm2msl']]]
    for index, source in enumerate(sources):
        obj = work / f'{index}.o'
        language = ['-fobjc-arc'] if source.suffix == '.m' else []
        subprocess.run([*common, *language, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    frameworks = ['Foundation', 'Metal', 'MetalFX', 'QuartzCore', 'CoreGraphics', 'AVFoundation',
                  'CoreVideo', 'CoreMedia', 'ImageIO', 'UniformTypeIdentifiers']
    subprocess.run(['xcrun', 'clang', *objects,
                    *[arg for name in frameworks for arg in ['-framework', name]], '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], cwd=work, timeout=45)
    if result.returncode == 77:
        print('SKIP: host has no Metal device; occlusion query GPU test skipped.', flush=True)
    else:
        result.check_returncode()

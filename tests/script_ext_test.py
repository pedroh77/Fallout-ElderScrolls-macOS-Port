"""Native unit tests for script_bridge.c and script_ext.c."""
import subprocess
import tempfile
from pathlib import Path

from native_test_toolchain import COMPILER, PLATFORM_FLAGS, LIBRARIES

root = Path(__file__).resolve().parents[1]
recomp = root / 'upstream/recomp'
host = recomp / 'host'
macos = root / 'macos'

with tempfile.TemporaryDirectory(prefix='fnv-script-ext-') as temp:
    binary = Path(temp) / 'script_ext_test'
    sources = [
        str(root / 'tests/script_ext_test.c'),
        str(recomp / 'rt_dispatch.c'),
        str(host / 'script_bridge.c'),
        str(host / 'script_ext.c'),
        str(host / 'gheap.c'),
        str(host / 'input.c'),
    ]
    subprocess.run(
        [
            *COMPILER,
            *PLATFORM_FLAGS,
            '-std=gnu11',
            '-O1',
            '-I', str(macos),
            '-I', str(recomp),
            '-I', str(host),
            *sources,
            *LIBRARIES,
            '-o', str(binary),
        ],
        check=True,
    )
    res = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
    print(res.stdout.strip())

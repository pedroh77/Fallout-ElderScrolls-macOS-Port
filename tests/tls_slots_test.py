import subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    binary=Path(temp)/'tls-slots-test'
    subprocess.run(['xcrun','clang','-O1','-ffunction-sections','-fdata-sections','-I',str(root/'macos'),'-I',str(root/'upstream/recomp/host'),'-I',str(root/'upstream/recomp'),
        str(root/'tests/tls_slots_test.c'),'-Wl,-dead_strip','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)

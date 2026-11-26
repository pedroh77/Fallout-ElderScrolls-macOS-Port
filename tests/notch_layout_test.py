import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    binary = Path(temp) / "notch-layout-test"
    subprocess.run(
        [
            "xcrun", "clang", "-O1", "-Wall", "-Wextra", "-fobjc-arc", "-I", str(root / "macos"),
            str(root / "tests/notch_layout_test.m"),
            str(root / "macos/FNVGameView.m"),
            "-framework", "AppKit",
            "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=30)

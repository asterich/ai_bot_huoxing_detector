"""Test the exact extension produced by each compiler without replacing installed wheels."""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("compiler", choices=["gcc", "clang"])
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = root / "build" / args.compiler
subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"], check=True)
package_root = build / "python"
package = package_root / "huoxing"
package.mkdir(parents=True, exist_ok=True)
for path in (root / "python/huoxing").glob("*.py"):
    shutil.copy2(path, package / path.name)
extension, = build.glob("_native*.so")
shutil.copy2(extension, package / extension.name)
env = dict(os.environ, PYTHONPATH=str(package_root))
# -S avoids any editable-install finder selecting another compiler's extension.
code = "import sys; sys.path.insert(0, sys.argv.pop(1)); import pytest; raise SystemExit(pytest.main(sys.argv[1:]))"
site = Path(sys.executable).resolve().parents[1] / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}" / "site-packages"
subprocess.run([sys.executable, "-S", "-c", code, str(site), str(root / "tests/python"), "-q"],
               cwd=package_root, env=env, check=True)

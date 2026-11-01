"""CI-only: build a wheel and an immutable, checksummed AstrBot deployment bundle."""
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile

from deploy import runtime_records

root = Path(__file__).resolve().parents[1]
dist = root / "dist"
dist.mkdir(exist_ok=True)
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
prefix = Path(sys.prefix)
env = dict(os.environ, CXX=str(prefix / "bin/x86_64-conda-linux-gnu-c++"), CMAKE_BUILD_PARALLEL_LEVEL="2")
subprocess.run([sys.executable, "-m", "pip", "wheel", "--no-deps", "--no-build-isolation",
                "--wheel-dir", str(dist), "-Ccmake.define.BUILD_TESTING=OFF", str(root)], env=env, check=True)
wheel, = dist.glob("*.whl")
if "-cp312-cp312-linux_x86_64.whl" not in wheel.name:
    raise RuntimeError(f"Unexpected ABI/platform: {wheel.name}")
with zipfile.ZipFile(wheel) as archive:
    native, = [name for name in archive.namelist() if name.startswith("huoxing/_native") and name.endswith(".so")]
    binary = dist / "wheel-native.so"
    binary.write_bytes(archive.read(native))
dynamic = subprocess.check_output(["readelf", "-d", str(binary)], text=True)
rpaths = [line for line in dynamic.splitlines() if "RPATH" in line or "RUNPATH" in line]
if not rpaths or any("[$ORIGIN/../../..]" not in line for line in rpaths):
    raise RuntimeError(f"Wheel must use only a relative locked-prefix RPATH: {rpaths}")
binary.unlink()

name = f"huoxing-linux-x86_64-cp312-{commit[:12]}"
bundle = dist / name
bundle.mkdir()
shutil.copy2(wheel, bundle / wheel.name)
shutil.copy2(root / "tools/deploy.py", bundle / "deploy.py")
shutil.copytree(root / "plugins/astrbot", bundle / "astrbot-plugin", ignore=shutil.ignore_patterns("__pycache__"))
manifest = {"format": 1, "commit": commit, "wheel": wheel.name,
            "version": wheel.name.split("-")[1], "python": list(sys.version_info[:2]),
            "platform": "linux_x86_64", "glibc_min": platform.libc_ver()[1],
            "pixi_lock_sha256": hashlib.sha256((root / "pixi.lock").read_bytes()).hexdigest(),
            "conda_packages": runtime_records(prefix),
            "python_packages": {name: importlib.metadata.version(name) for name in ("numpy", "Pillow")}}
(bundle / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
checks = []
for path in sorted(bundle.rglob("*")):
    if path.is_file():
        checks.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(bundle).as_posix()}")
(bundle / "SHA256SUMS").write_text("\n".join(checks) + "\n")
archive_path = dist / (name + ".tar.gz")
with tarfile.open(archive_path, "w:gz") as archive:
    archive.add(bundle, arcname=name)
archive_path.with_suffix(".gz.sha256").write_text(f"{hashlib.sha256(archive_path.read_bytes()).hexdigest()}  {archive_path.name}\n")
(dist / "bundle-path.txt").write_text(str(bundle) + "\n")
print(f"Built {archive_path}", flush=True)

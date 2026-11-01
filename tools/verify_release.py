"""CI-only: install/test the published wheel in a relocated locked Pixi prefix."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("bundle", type=Path)
args = parser.parse_args()
bundle = args.bundle.resolve()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="huoxing-wheel-") as temp:
    isolated = Path(temp)
    shutil.copy2(root / "pixi.toml", isolated / "pixi.toml")
    shutil.copy2(root / "pixi.lock", isolated / "pixi.lock")
    subprocess.run(["pixi", "install", "--locked", "--manifest-path", str(isolated / "pixi.toml")], check=True)
    prefix = isolated / ".pixi/envs/default"
    python = prefix / "bin/python"
    env = {key: value for key, value in os.environ.items()
           if key not in ("PYTHONPATH", "PYTHONHOME", "LD_LIBRARY_PATH", "CONDA_PREFIX")}
    env.update(CC="/bin/false", CXX="/bin/false", CMAKE_COMMAND="/bin/false")
    # Fake service control only. This suite never contacts a live AstrBot or SSH host.
    fakebin = isolated / "fakebin"
    fakebin.mkdir()
    control = fakebin / "systemctl"
    control.write_text("#!/bin/sh\nexit 0\n")
    control.chmod(0o755)
    env["PATH"] = str(fakebin) + ":" + env["PATH"]
    astrbot, state = isolated / "AstrBot", isolated / "state"
    plugin = astrbot / "data/plugins/astrbot_plugin_huoxing"
    plugin.mkdir(parents=True)
    (plugin / "old-marker").write_text("old plugin")
    data = astrbot / "data/plugin_data/astrbot_plugin_huoxing"
    data.mkdir(parents=True)
    (data / "keep-marker").write_text("database untouched")
    deploy = [str(python), str(bundle / "deploy.py")]
    options = ["--astrbot", str(astrbot), "--state-dir", str(state)]
    def run(*command):
        subprocess.run(command, check=True, cwd=isolated, env=env)
    run(*deploy, "check")
    run(*deploy, "install", *options)
    assert not (plugin / "old-marker").exists()
    assert (data / "keep-marker").read_text() == "database untouched"
    # The whole existing Python regression suite imports the installed wheel.
    shutil.copytree(root / "tests", isolated / "tests", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(root / "plugins", isolated / "plugins", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(root / "tools", isolated / "tools", ignore=shutil.ignore_patterns("__pycache__"))
    run(str(python), "-I", "-c", "import huoxing,pathlib,sys; assert pathlib.Path(huoxing.__file__).resolve().is_relative_to(pathlib.Path(sys.prefix)); import pytest; raise SystemExit(pytest.main(['tests/python','-q']))")
    # Reproduce the server's existing editable install, on this CI runner only.
    editable_env = dict(env, CC=str(prefix / "bin/x86_64-conda-linux-gnu-cc"),
                        CXX=str(prefix / "bin/x86_64-conda-linux-gnu-c++"),
                        CONDA_PREFIX=str(prefix), CMAKE_PREFIX_PATH=str(prefix),
                        CMAKE_BUILD_PARALLEL_LEVEL="2")
    editable_env.pop("CMAKE_COMMAND", None)
    editable_env["PATH"] = str(prefix / "bin") + ":" + env["PATH"]
    subprocess.run([str(python), "-m", "pip", "install", "--no-deps", "--no-build-isolation", "-e", str(root)],
                   check=True, cwd=isolated, env=editable_env)
    # The actual deployment remains compiler-disabled and must remove editable metadata.
    run(*deploy, "install", *options)
    current = json.loads((state / "current.json").read_text())
    run(*deploy, "rollback", "--backup", current["backup"], *options)
    run(str(python), "-I", "-c", "import huoxing,pathlib; from huoxing import _native; assert pathlib.Path(huoxing.__file__).resolve() == pathlib.Path(" + repr(str(root / "python/huoxing/__init__.py")) + "); print('Editable rollback import:',huoxing.__file__)")
    assert (data / "keep-marker").read_text() == "database untouched"
    print("Verified installed wheel, relocated runtime, installer and rollback", flush=True)


"""Install/roll back a verified wheel bundle without building or changing dependencies."""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import sysconfig
import time
import uuid


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def runtime_records(prefix):
    records = {}
    for path in (prefix / "conda-meta").glob("*.json"):
        entry = json.loads(path.read_text())
        records[entry["name"]] = [entry["version"], entry["build"]]
    return records


def inside(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or path == root.resolve():
        raise ValueError(f"Path outside expected directory: {relative}")
    return path


def validate_bundle(bundle, prefix):
    checks = {}
    for line in (bundle / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split("  ", 1)
        path = inside(bundle, name)
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing bundle file: {name}")
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
                raise ValueError(f"Checksum mismatch: {name}")
        checks[name] = digest
    manifest = json.loads((bundle / "manifest.json").read_text())
    if manifest.get("format") != 1 or not manifest.get("conda_packages"):
        raise ValueError("Unsupported/incomplete manifest")
    required = {"manifest.json", "deploy.py", manifest["wheel"]}
    required.update(p.relative_to(bundle).as_posix() for p in (bundle / "astrbot-plugin").rglob("*") if p.is_file())
    if not required <= checks.keys() or not (bundle / "astrbot-plugin/main.py").is_file():
        raise ValueError("Incomplete bundle or checksum list")
    wheel = inside(bundle, manifest["wheel"])
    if wheel.suffix != ".whl" or wheel.parent != bundle:
        raise ValueError("Only a bundled wheel can be installed")
    if sys.platform != "linux" or platform.machine() != "x86_64":
        raise ValueError("This bundle requires Linux x86_64")
    if platform.python_implementation() != "CPython" or list(sys.version_info[:2]) != manifest["python"]:
        raise ValueError("Python ABI mismatch; use the existing Pixi Python 3.12")
    libc, version = platform.libc_ver()
    if libc != "glibc" or tuple(map(int, version.split("."))) < tuple(map(int, manifest["glibc_min"].split("."))):
        raise ValueError(f"Requires glibc >= {manifest['glibc_min']}")
    installed = runtime_records(prefix)
    mismatches = [name for name, value in manifest["conda_packages"].items() if installed.get(name) != value]
    if mismatches:
        raise ValueError("Locked Pixi environment mismatch: " + ", ".join(mismatches))
    for name, version in manifest["python_packages"].items():
        if importlib.metadata.version(name) != version:
            raise ValueError(f"Python dependency mismatch: {name} requires {version}")
    return manifest


def installed_files(prefix):
    try:
        dist = importlib.metadata.distribution("huoxing-detector")
    except importlib.metadata.PackageNotFoundError:
        return []
    files = []
    for item in dist.files or []:
        path = Path(dist.locate_file(item)).resolve()
        if not path.is_relative_to(prefix) or path == prefix:
            raise ValueError(f"Installed package file outside runtime: {path}")
        if path.is_file():
            files.append(path)
    return files


def copy_files(paths, root, destination):
    for path in paths:
        target = inside(destination, path.relative_to(root))
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def restore(backup, prefix, astrbot, service):
    if not (backup / "complete").is_file():
        raise ValueError("Backup is incomplete; refusing rollback")
    meta = json.loads((backup / "backup.json").read_text())
    if meta["prefix"] != str(prefix) or meta["astrbot"] != str(astrbot) or meta["service"] != service:
        raise ValueError("Backup belongs to a different runtime, AstrBot path or service")
    run("systemctl", "--user", "stop", service)
    for path in installed_files(prefix):
        path.unlink(missing_ok=True)
    # Remove bytecode as well; only this package's bounded directories are removed.
    site = Path(sysconfig.get_path("purelib")).resolve()
    for path in [site / "huoxing", *site.glob("huoxing_detector-*.dist-info")]:
        if path.exists():
            checked = inside(prefix, path.relative_to(prefix))
            shutil.rmtree(checked)
    files = backup / "runtime-files"
    copy_files([p for p in files.rglob("*") if p.is_file()], files, prefix)
    plugin = astrbot / "data/plugins/astrbot_plugin_huoxing"
    if plugin.exists():
        shutil.rmtree(inside(astrbot, "data/plugins/astrbot_plugin_huoxing"))
    if (backup / "plugin").exists():
        shutil.copytree(backup / "plugin", plugin)
    if meta["was_active"]:
        run("systemctl", "--user", "start", service)
    print(f"Restored package and plugin from {backup}; database was not overwritten", flush=True)


def install(bundle, prefix, astrbot, state, service):
    manifest = validate_bundle(bundle, prefix)
    plugin = astrbot / "data/plugins/astrbot_plugin_huoxing"
    data = astrbot / "data/plugin_data/astrbot_plugin_huoxing"
    if plugin.is_symlink() or data.is_symlink():
        raise ValueError("Plugin/data paths must not be symlinks")
    paths = installed_files(prefix)
    backup = state / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    backup.mkdir(parents=True)
    active = subprocess.run(["systemctl", "--user", "is-active", "--quiet", service]).returncode == 0
    meta = {"prefix": str(prefix), "astrbot": str(astrbot), "service": service,
            "was_active": active, "new_commit": manifest["commit"]}
    (backup / "backup.json").write_text(json.dumps(meta, indent=2))
    print(f"Validated commit {manifest['commit']}; backup: {backup}", flush=True)
    run("systemctl", "--user", "stop", service)
    try:
        copy_files(paths, prefix, backup / "runtime-files")
        if plugin.exists():
            shutil.copytree(plugin, backup / "plugin")
        if data.exists():
            shutil.copytree(data, backup / "plugin-data")
        (backup / "complete").touch()
    except Exception:
        if active:
            run("systemctl", "--user", "start", service)
        raise
    try:
        # Exact local wheel only: never resolve dependencies or fall back to source.
        run(sys.executable, "-m", "pip", "--disable-pip-version-check", "install",
            "--no-index", "--no-deps", "--only-binary=:all:", "--force-reinstall",
            str(bundle / manifest["wheel"]))
        if plugin.exists():
            shutil.rmtree(inside(astrbot, "data/plugins/astrbot_plugin_huoxing"))
        shutil.copytree(bundle / "astrbot-plugin", plugin)
        smoke = "import huoxing, pathlib, sys; p=pathlib.Path(huoxing.__file__).resolve(); assert p.is_relative_to(pathlib.Path(sys.prefix).resolve()), p; print('Installed:', p); from huoxing import _native; print('Native:', _native.__file__)"
        run(sys.executable, "-I", "-c", smoke, cwd=state)
        run("systemctl", "--user", "restart", service)
        time.sleep(3)
        run("systemctl", "--user", "is-active", "--quiet", service)
    except Exception:
        print("Deployment failed; restoring previous package/plugin", file=sys.stderr, flush=True)
        restore(backup, prefix, astrbot, service)
        raise
    (state / "current.json").write_text(json.dumps({"commit": manifest["commit"], "backup": str(backup)}, indent=2))
    print(f"Installed {manifest['commit']}; rollback: python {bundle / 'deploy.py'} rollback --backup {backup}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "rollback", "check"])
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--backup", type=Path)
    parser.add_argument("--astrbot", type=Path, default=Path.home() / "qqbot/AstrBot")
    parser.add_argument("--state-dir", type=Path, default=Path.home() / "qqbot/huoxing-deploy")
    parser.add_argument("--service", default="astrbot.service")
    args = parser.parse_args()
    prefix, astrbot, state = Path(sys.prefix).resolve(), args.astrbot.resolve(), args.state_dir.resolve()
    if not (prefix / "conda-meta").is_dir():
        parser.error("Invoke this script with the existing Pixi environment's Python")
    if args.action == "check":
        manifest = validate_bundle(args.bundle.resolve(), prefix)
        print(f"Bundle/runtime compatible: {manifest['commit']}")
        return
    import fcntl
    state.mkdir(parents=True, exist_ok=True)
    with (state / ".lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.action == "install":
            install(args.bundle.resolve(), prefix, astrbot, state, args.service)
        elif args.backup:
            restore(args.backup.resolve(), prefix, astrbot, args.service)
        else:
            parser.error("rollback requires --backup")


if __name__ == "__main__":
    main()

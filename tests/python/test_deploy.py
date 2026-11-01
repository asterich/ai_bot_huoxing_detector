import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import subprocess
import sys

import pytest

spec = importlib.util.spec_from_file_location("huoxing_deploy", Path(__file__).parents[2] / "tools/deploy.py")
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


def bundle_fixture(tmp_path):
    prefix = tmp_path / "runtime"
    (prefix / "conda-meta").mkdir(parents=True)
    (prefix / "conda-meta/runtime.json").write_text(json.dumps({"name": "runtime", "version": "1", "build": "test"}))
    bundle = tmp_path / "bundle"
    (bundle / "astrbot-plugin").mkdir(parents=True)
    (bundle / "astrbot-plugin/main.py").write_text("# new plugin")
    (bundle / "deploy.py").write_text("# installer")
    (bundle / "example.whl").write_bytes(b"wheel")
    manifest = {"format": 1, "commit": "test-commit", "wheel": "example.whl",
                "python": list(sys.version_info[:2]), "glibc_min": platform.libc_ver()[1] or "2.17",
                "conda_packages": {"runtime": ["1", "test"]}, "python_packages": {}}
    (bundle / "manifest.json").write_text(json.dumps(manifest))
    checks = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(bundle).as_posix()}"
              for p in bundle.rglob("*") if p.is_file()]
    (bundle / "SHA256SUMS").write_text("\n".join(checks))
    return bundle, prefix


@pytest.mark.skipif(sys.platform != "linux", reason="Linux deployment contract")
def test_bundle_integrity_and_runtime_mismatch(tmp_path):
    bundle, prefix = bundle_fixture(tmp_path)
    assert deploy.validate_bundle(bundle, prefix)["commit"] == "test-commit"
    (prefix / "conda-meta/runtime.json").write_text(json.dumps({"name": "runtime", "version": "2", "build": "test"}))
    with pytest.raises(ValueError, match="environment mismatch"):
        deploy.validate_bundle(bundle, prefix)
    (bundle / "example.whl").write_bytes(b"tampered")
    with pytest.raises(ValueError, match="Checksum mismatch"):
        deploy.validate_bundle(bundle, prefix)


def test_paths_cannot_escape_runtime(tmp_path):
    with pytest.raises(ValueError, match="outside"):
        deploy.inside(tmp_path, "../escape")
    with pytest.raises(ValueError, match="outside"):
        deploy.inside(tmp_path, ".")


def test_failed_install_restores_files_without_restoring_database(monkeypatch, tmp_path):
    bundle, prefix = bundle_fixture(tmp_path)
    site = prefix / "lib/python/site-packages"
    original = site / "huoxing/_native.so"
    original.parent.mkdir(parents=True)
    original.write_bytes(b"old extension")
    astrbot = tmp_path / "AstrBot"
    plugin = astrbot / "data/plugins/astrbot_plugin_huoxing"
    plugin.mkdir(parents=True)
    (plugin / "main.py").write_text("old plugin")
    data = astrbot / "data/plugin_data/astrbot_plugin_huoxing"
    data.mkdir(parents=True)
    (data / "db").write_text("keep live data")
    state = tmp_path / "state"
    state.mkdir()
    calls = []
    monkeypatch.setattr(deploy, "validate_bundle", lambda *args: {"commit": "test", "wheel": "example.whl"})
    monkeypatch.setattr(deploy, "installed_files", lambda *args: [original] if original.exists() else [])
    monkeypatch.setattr(deploy.sysconfig, "get_path", lambda *args: str(site))
    monkeypatch.setattr(deploy.subprocess, "run", lambda *args, **kwargs: type("Result", (), {"returncode": 0})())
    def operation(*args, **kwargs):
        calls.append(args)
        if "pip" in args:
            assert "--no-index" in args and "--no-deps" in args and "--only-binary=:all:" in args
            original.write_bytes(b"partial new install")
            raise subprocess.CalledProcessError(1, args)
    monkeypatch.setattr(deploy, "run", operation)
    with pytest.raises(subprocess.CalledProcessError):
        deploy.install(bundle, prefix, astrbot, state, "astrbot.service")
    assert original.read_bytes() == b"old extension"
    assert (plugin / "main.py").read_text() == "old plugin"
    assert (data / "db").read_text() == "keep live data"
    assert calls[-1] == ("systemctl", "--user", "start", "astrbot.service")
    assert not (state / "current.json").exists()


def test_incompatible_bundle_does_not_stop_service(monkeypatch, tmp_path):
    def reject(*args):
        raise ValueError("incompatible")
    monkeypatch.setattr(deploy, "validate_bundle", reject)
    def unexpected(*args, **kwargs):
        raise AssertionError("Touched service before validation")
    monkeypatch.setattr(deploy, "run", unexpected)
    with pytest.raises(ValueError, match="incompatible"):
        deploy.install(tmp_path, tmp_path, tmp_path, tmp_path, "astrbot.service")


def test_incomplete_backup_does_not_stop_service(monkeypatch, tmp_path):
    monkeypatch.setattr(deploy, "run", lambda *args: pytest.fail("Stopped service"))
    with pytest.raises(ValueError, match="incomplete"):
        deploy.restore(tmp_path, tmp_path, tmp_path, "astrbot.service")

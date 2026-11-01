"""Format every native source, including C++ named module interfaces."""
import argparse
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
files = sorted(str(path) for directory in ("src", "tests")
               for path in (root / directory).rglob("*")
               if path.suffix in {".cpp", ".cppm", ".hpp"})
command = ["clang-format", "--style=file"]
command += ["--dry-run", "--Werror"] if args.check else ["-i"]
subprocess.run(command + files, cwd=root, check=True)

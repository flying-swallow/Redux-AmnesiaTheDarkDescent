#!/usr/bin/env python3
"""Run the repository's Python tests from any working directory.

Unittest returns 5 when the suite is empty; that is expected while the suite
is being repopulated, so map only that status to success. Real test failures
and discovery/import errors remain fatal.
"""

from pathlib import Path
import subprocess
import sys


def main():
    repo_root = Path(__file__).resolve().parent.parent
    command = [
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-t",
        str(repo_root),
        "-s",
        str(repo_root / "tests"),
        "-p",
        "*_test.py",
        *sys.argv[1:],
    ]
    # Discovery imports test modules relative to the repo root, so pin the
    # child's working directory instead of inheriting the caller's.
    result = subprocess.run(command, cwd=str(repo_root))
    return 0 if result.returncode == 5 else result.returncode


if __name__ == "__main__":
    sys.exit(main())

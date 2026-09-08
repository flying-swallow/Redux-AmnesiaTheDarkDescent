#!/usr/bin/env bash
# Run the premake build directly on the native Linux host.
#
# build-linux-docker.sh is the containerized equivalent and canonical path.
# This wrapper runs `premake5 gmake2` + `make` (+ optional `premake5 deploy`)
# without Docker or Podman.

set -euo pipefail

CONFIG="release"
CLEAN=0
DEPLOY=1
GAME_DIR=""
EXTRA_ARGS=()

usage() {
    cat <<'EOF'
Usage: ./build-linux.sh [release|debug] [options] [-- <extra premake args>]

Options:
    --clean              Remove build-premake/ before generating
    --no-deploy          Skip `premake5 deploy` (copying game assets next to
                         the built executable)
    --game-dir <path>    Path to your Amnesia: The Dark Descent install
                         (falls back to $AMNESIA_GAME_DIRECTORY; if unset,
                         deployment is skipped)
    -h, --help           Show this help

Anything after `--` is forwarded verbatim to `premake5 gmake2`.

Examples:
    ./build-linux.sh                                # native release
    ./build-linux.sh debug                          # native debug
    ./build-linux.sh release --clean                # wipe build-premake/ and rebuild
    ./build-linux.sh release --game-dir "$HOME/atdd"
    ./build-linux.sh release -- --with-fsr=no
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        release|debug)  CONFIG="$1"; shift ;;
        --clean)        CLEAN=1; shift ;;
        --no-deploy)    DEPLOY=0; shift ;;
        --game-dir)
            if [[ $# -lt 2 ]]; then
                echo "error: --game-dir requires a path" >&2
                usage >&2
                exit 1
            fi
            GAME_DIR="$2"
            shift 2
            ;;
        -h|--help)      usage; exit 0 ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
        *)              echo "error: unknown argument '$1'" >&2; usage >&2; exit 1 ;;
    esac
done

GAME_DIR="${GAME_DIR:-${AMNESIA_GAME_DIRECTORY:-}}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: build-linux.sh runs on Linux hosts" >&2
    exit 1
fi

if [[ ! -f HPL2/extern/SDL/CMakeLists.txt ]]; then
    echo "==> Initialising git submodules"
    git submodule update --init --recursive
fi

if ! command -v premake5 >/dev/null 2>&1; then
    echo "error: premake5 5.0.0-beta8 is required on PATH (the version pinned by CI and Dockerfile)" >&2
    exit 1
fi

if [[ "$DEPLOY" == "1" && -z "$GAME_DIR" ]]; then
    echo "warning: no game dir set; skipping deploy" >&2
    DEPLOY=0
fi

if [[ "$CLEAN" == "1" ]]; then
    echo "==> Cleaning build-premake"
    rm -rf build-premake
fi

echo "==> Generating gmake2 project files"
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    premake5 gmake2 "${EXTRA_ARGS[@]}"
else
    premake5 gmake2
fi

echo "==> Building ($CONFIG)"
make -C build-premake config="$CONFIG" -j"$(nproc 2>/dev/null || echo 4)"

# Premake postbuild only runs when the target relinks, so a Python-only
# edit would otherwise leave these tests untested. They need no game install,
# GPU, or display.
echo "==> Running python tests"
python3 scripts/run_python_tests.py

if [[ "$DEPLOY" == "1" ]]; then
    echo "==> Deploying game assets from $GAME_DIR"
    premake5 deploy --game-dir="$GAME_DIR"
fi

echo "==> Build complete: build-premake/amnesia/"

#!/usr/bin/env bash
# Linux build wrapper for Amnesia64.
#
# Configures and builds the CMake project, then runs the `deploy` target so the
# binaries are copied next to the game assets ready to launch.

set -euo pipefail

CONFIG="release"
CLEAN=0
DEPLOY=1
GAME_DIR="${AMNESIA_GAME_DIRECTORY:-}"
EXTRA_ARGS=()

usage() {
    cat <<'EOF'
Usage: ./build-linux.sh [release|debug] [options] [-- <extra cmake args>]

Options:
    --clean              Remove build/ before configuring
    --no-deploy          Skip the 'deploy' target (copying built binaries into
                         the game folder)
    --game-dir <path>    Path to your Amnesia: The Dark Descent install
                         (default: $AMNESIA_GAME_DIRECTORY or
                         ~/.local/share/Steam/steamapps/common/Amnesia The Dark Descent)
    -h, --help           Show this help

Examples:
    ./build-linux.sh                                # native release
    ./build-linux.sh debug                          # native debug
    ./build-linux.sh release --clean                # wipe build dir and rebuild
    ./build-linux.sh release --game-dir "$HOME/atdd"
    ./build-linux.sh release -- -DUSE_SYSTEM_SDL2=ON
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        release|debug)  CONFIG="$1"; shift ;;
        --clean)        CLEAN=1; shift ;;
        --no-deploy)    DEPLOY=0; shift ;;
        --game-dir)     GAME_DIR="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
        *)              echo "error: unknown argument '$1'" >&2; usage; exit 1 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [[ ! -f extern/SDL/CMakeLists.txt ]]; then
    echo "==> Initialising git submodules"
    git submodule update --init --recursive
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: build-linux.sh runs on Linux hosts" >&2
    exit 1
fi

# Generator, build type and backend flags live in CMakePresets.json.
PRESET="linux-${CONFIG}"
BUILD_DIR="$ROOT/build"

if [[ "$CLEAN" == "1" && -d "$BUILD_DIR" ]]; then
    echo "==> Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

CONFIGURE_ARGS=(--preset "$PRESET")
if [[ -n "$GAME_DIR" ]]; then
    CONFIGURE_ARGS+=(-D "AMNESIA_GAME_DIRECTORY=$GAME_DIR")
fi
CONFIGURE_ARGS+=("${EXTRA_ARGS[@]}")

echo "==> Configuring (preset: $PRESET)"
cmake "${CONFIGURE_ARGS[@]}"
cmake --build --preset "$PRESET"
[[ "$DEPLOY" == "1" ]] && cmake --build --preset "$PRESET" --target deploy

echo "==> Build complete: build/bin/"

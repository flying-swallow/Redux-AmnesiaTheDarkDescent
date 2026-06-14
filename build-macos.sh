#!/usr/bin/env bash
# macOS build wrapper for Amnesia64.
#
# Configures and builds the CMake project (Ninja generator), then runs the
# `deploy` target so the binaries are copied next to the game assets ready to
# launch.

set -euo pipefail

CONFIG="release"
CLEAN=0
DEPLOY=1
GAME_DIR="${AMNESIA_GAME_DIRECTORY:-}"
EXTRA_ARGS=()

usage() {
    cat <<'EOF'
Usage: ./build-macos.sh [release|debug] [options] [-- <extra cmake args>]

Options:
    --clean              Remove build/ before configuring
    --no-deploy          Skip the 'deploy' target (copying built binaries into
                         the game folder)
    --game-dir <path>    Path to your Amnesia: The Dark Descent install
                         (default: $AMNESIA_GAME_DIRECTORY or
                         ~/Library/Application Support/Steam/steamapps/common/Amnesia The Dark Descent)
    -h, --help           Show this help

Requirements:
    - Ninja (brew install ninja)
    - Builds the native Metal backend (ENABLE_METAL, via the macos-* presets);
      no Vulkan loader / MoltenVK needed.

Examples:
    ./build-macos.sh                                # native release
    ./build-macos.sh debug                          # native debug
    ./build-macos.sh release --clean                # wipe build dir and rebuild
    ./build-macos.sh release --game-dir "$HOME/atdd"
    ./build-macos.sh release -- -DUSE_SYSTEM_SDL2=ON
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

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "error: build-macos.sh runs on macOS hosts" >&2
    exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
    echo "error: ninja not found on PATH. Install it with 'brew install ninja'." >&2
    exit 1
fi

if [[ ! -f extern/SDL/CMakeLists.txt ]]; then
    echo "==> Initialising git submodules"
    git submodule update --init --recursive
fi

# Generator (Ninja), build type and backend flags (Metal) live in CMakePresets.json.
PRESET="macos-${CONFIG}"
BUILD_DIR="$ROOT/build"

if [[ "$CLEAN" == "1" && -d "$BUILD_DIR" ]]; then
    echo "==> Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

CONFIGURE_ARGS=(--preset "$PRESET")
if [[ -n "$GAME_DIR" ]]; then
    CONFIGURE_ARGS+=(-D "AMNESIA_GAME_DIRECTORY=$GAME_DIR")
fi
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    CONFIGURE_ARGS+=("${EXTRA_ARGS[@]}")
fi

echo "==> Configuring (preset: $PRESET)"
cmake "${CONFIGURE_ARGS[@]}"
cmake --build --preset "$PRESET"
[[ "$DEPLOY" == "1" ]] && cmake --build --preset "$PRESET" --target deploy

echo "==> Build complete: build/bin/"

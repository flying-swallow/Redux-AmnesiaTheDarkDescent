#!/usr/bin/env bash
# Run the premake build inside a Docker / Podman container.
#
# Bind-mounts the project tree at its REAL host path inside the container so
# absolute paths in the generated build-premake/ makefiles line up identically
# between host and container runs (you can switch between a native premake
# build and this wrapper without a full clean).
#
# The wrapper accepts the same options as a native premake build and runs
# `premake5 gmake2` + `make` (+ optional `premake5 deploy`) in the container.
# Examples:
#   ./build-linux-docker.sh                          # release
#   ./build-linux-docker.sh debug --clean
#   ./build-linux-docker.sh release --game-dir "/path/to/Amnesia TDD"
#   ./build-linux-docker.sh release -- --slangc=/path/to/slangc
#
# Options:
#   release | debug     Build configuration (default: release)
#   --clean             Remove build-premake/ before generating
#   --no-deploy         Skip `premake5 deploy` (copying game assets next to the
#                       built executable)
#   --compile-commands  Run `premake5 export-compile-commands` and symlink its
#                       output to compile_commands.json in the repo root (for
#                       clangd/LSP tooling). Reads the project model directly,
#                       so it doesn't need a build and every path in the
#                       output is absolute.
#   --game-dir <path>   Path to your installed Amnesia: The Dark Descent folder
#                       (falls back to $AMNESIA_GAME_DIRECTORY)
#   -- <args>           Extra args forwarded to `premake5 gmake2`
#
# Extra bind mounts (for tools that live outside the project tree, e.g. a
# locally-built slangc):
#
#   AMNESIA_DOCKER_MOUNTS="$HOME/projects/slang" \
#       ./build-linux-docker.sh release -- \
#       --slangc="$HOME/projects/slang/build/Release/bin/slangc"
#
# Multiple paths can be colon-separated. Each is mounted at the same path
# inside the container, so args referring to host paths "just work".

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${AMNESIA_DOCKER_IMAGE:-amnesia64-build:ubuntu-24.04}"

# Rootless podman maps container-root back to the host user via subuid/subgid
# — passing --user there would shift to a different in-container uid that
# CAN'T touch files owned by the host user. Real docker (rootful) needs
# --user or every artifact ends up owned by root on the host.
# Probe for the runtime. The previous `docker --version | grep podman` test
# wasn't reliable across podman-docker shim variants, so check for the
# `podman` binary on PATH directly. Override with AMNESIA_DOCKER_RUNTIME=docker
# if both podman and real docker are installed and you want real docker.
RUNTIME="${AMNESIA_DOCKER_RUNTIME:-}"
if [[ -z "$RUNTIME" ]]; then
    if command -v podman >/dev/null 2>&1; then
        RUNTIME="podman"
    else
        RUNTIME="docker"
    fi
fi

"$RUNTIME" build -t "$IMAGE" -f "$ROOT/Dockerfile" "$ROOT"

USER_ARGS=()
if [[ "$RUNTIME" == "podman" ]]; then
    echo "==> Runtime: podman (rootless) — container root maps to host uid $(id -u)."
else
    echo "==> Runtime: docker — running as --user $(id -u):$(id -g) to keep artifacts host-owned."
    USER_ARGS+=(--user "$(id -u):$(id -g)")
fi

# Allocate a TTY only when stdout is one, so CI / pipes still work.
TTY_ARGS=()
if [ -t 1 ]; then
    TTY_ARGS+=("-t")
fi

# Project tree mounted at its real host path so absolute paths in the generated
# build-premake/ makefiles line up identically inside and out.
MOUNT_ARGS=(
    --mount "type=bind,source=$ROOT,target=$ROOT"
)

# Parse build options. The premake `deploy` action copies game assets *out of*
# the game dir into build-premake/, so the game dir must be mounted (read
# access) whenever we deploy; --game-dir falls back to the AMNESIA_GAME_DIRECTORY
# env var. Everything after `--` is forwarded verbatim to `premake5 gmake2`.
#
# When --no-deploy is passed, the game dir is never read, so don't bother
# resolving/mounting it (and don't require it to exist on the host).
CONFIG="release"
CLEAN=0
GAME_DIR=""
NO_DEPLOY=0
COMPILE_COMMANDS=0
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        release|debug)       CONFIG="$1"; shift ;;
        --clean)             CLEAN=1; shift ;;
        --no-deploy)         NO_DEPLOY=1; shift ;;
        --compile-commands)  COMPILE_COMMANDS=1; shift ;;
        --game-dir)          GAME_DIR="${2:-}"; shift 2 ;;
        --)            shift; EXTRA_ARGS=("$@"); break ;;
        *)             echo "error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done
GAME_DIR="${GAME_DIR:-${AMNESIA_GAME_DIRECTORY:-}}"

ENV_ARGS=(-e HOME=/tmp)
if [[ "$NO_DEPLOY" == "1" ]]; then
    echo "==> --no-deploy: skipping game dir mount."
elif [[ -n "$GAME_DIR" ]]; then
    if [[ -d "$GAME_DIR" ]]; then
        # Resolve symlinks before mounting. Steam libraries often live behind
        # a symlink (external drives, Flatpak Steam, etc.); podman binds the
        # source you give it as-is, so if we passed a symlinked path the
        # target inside would shadow the real directory but contain nothing.
        # Mount the resolved real path AT the original path inside, so the
        # `premake5 deploy` action — which is handed the original path string
        # via --game-dir — reads through the bind mount and sees the real
        # contents.
        GAME_DIR_REAL="$(realpath "$GAME_DIR")"
        MOUNT_ARGS+=(--mount "type=bind,source=$GAME_DIR_REAL,target=$GAME_DIR")
        if [[ "$GAME_DIR_REAL" != "$GAME_DIR" ]]; then
            echo "==> Mounting game dir: $GAME_DIR (resolved: $GAME_DIR_REAL)"
        else
            echo "==> Mounting game dir: $GAME_DIR"
        fi
    else
        echo "warning: game dir does not exist on host, skipping mount: $GAME_DIR" >&2
    fi
fi

# Any extra host paths the user wants visible — typically a locally-built
# slangc tree referenced via -- --slangc=...
if [[ -n "${AMNESIA_DOCKER_MOUNTS:-}" ]]; then
    IFS=':' read -r -a _extra <<<"$AMNESIA_DOCKER_MOUNTS"
    for p in "${_extra[@]}"; do
        [[ -e "$p" ]] || { echo "error: AMNESIA_DOCKER_MOUNTS path does not exist: $p" >&2; exit 1; }
        MOUNT_ARGS+=(--mount "type=bind,source=$p,target=$p")
    done
fi

# Deploy unless --no-deploy and only when a game dir is available.
DEPLOY=1
[[ "$NO_DEPLOY" == "1" || -z "$GAME_DIR" ]] && DEPLOY=0

ENV_ARGS+=(
    -e "PM_CONFIG=$CONFIG"
    -e "PM_CLEAN=$CLEAN"
    -e "PM_DEPLOY=$DEPLOY"
    -e "PM_GAME_DIR=$GAME_DIR"
    -e "PM_COMPILE_COMMANDS=$COMPILE_COMMANDS"
)

# Run the premake build inside the container. Build options arrive via PM_*
# env vars; the `--`-forwarded extra args are passed as positional args to the
# inner shell ($0 is a label, $@ is the extra args) and handed to gmake2.
exec "$RUNTIME" run --rm "${TTY_ARGS[@]}" "${USER_ARGS[@]}" \
    "${ENV_ARGS[@]}" \
    "${MOUNT_ARGS[@]}" \
    -w "$ROOT" \
    "$IMAGE" \
    bash -lc '
        set -euo pipefail
        [[ "$PM_CLEAN" == 1 ]] && rm -rf build-premake
        JOBS="$(nproc 2>/dev/null || echo 4)"
        echo "==> Generating gmake2 project files"
        premake5 gmake2 "$@"
        if [[ "$PM_COMPILE_COMMANDS" == 1 ]]; then
            # Reads the already-resolved premake project model directly, so
            # it needs no compile step and every path it emits (`directory`
            # and each `-I`) is absolute -- no base-directory ambiguity for
            # tools consuming the database from outside build-premake/.
            echo "==> Exporting compile_commands.json ($PM_CONFIG)"
            premake5 export-compile-commands
            ln -sf "build-premake/compile_commands/$PM_CONFIG.json" compile_commands.json
        fi
        echo "==> Building ($PM_CONFIG)"
        make -C build-premake config="$PM_CONFIG" -j"$JOBS"
        if [[ "$PM_DEPLOY" == 1 ]]; then
            echo "==> Deploying game assets from $PM_GAME_DIR"
            premake5 deploy --game-dir="$PM_GAME_DIR"
        fi
        echo "==> Build complete: build-premake/amnesia/"
    ' premake-build "${EXTRA_ARGS[@]}"

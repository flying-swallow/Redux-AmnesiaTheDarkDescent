#!/usr/bin/env bash
# Run build-linux.sh inside a Docker / Podman container.
#
# Bind-mounts the project tree at its REAL host path inside the container so
# the CMakeCache.txt source path matches between host and container runs (you
# can switch between ./build-linux.sh and this wrapper without --clean).
#
# All args are forwarded to build-linux.sh. Examples:
#   ./build-linux-docker.sh                          # release
#   ./build-linux-docker.sh debug --clean
#   ./build-linux-docker.sh release -- -DAMNESIA_GAME_DIRECTORY=/path
#
# Extra bind mounts (for tools that live outside the project tree, e.g. a
# locally-built slangc):
#
#   AMNESIA_DOCKER_MOUNTS="$HOME/projects/slang" \
#       ./build-linux-docker.sh release -- \
#       -DSLANGC_EXECUTABLE="$HOME/projects/slang/build/Release/bin/slangc"
#
# Multiple paths can be colon-separated. Each is mounted at the same path
# inside the container, so cmake args referring to host paths "just work".

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${AMNESIA_DOCKER_IMAGE:-amnesia64-build:ubuntu-24.04}"

docker build -t "$IMAGE" -f "$ROOT/Dockerfile" "$ROOT"

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

USER_ARGS=()
if [[ "$RUNTIME" == "podman" ]]; then
    echo "==> Runtime: podman (rootless) — container root maps to host uid $(id -u)."
else
    echo "==> Runtime: docker — running as --user $(id -u):$(id -g) to keep artifacts host-owned."
fi
USER_ARGS+=(--user "$(id -u):$(id -g)")

# Allocate a TTY only when stdout is one, so CI / pipes still work.
TTY_ARGS=()
if [ -t 1 ]; then
    TTY_ARGS+=("-t")
fi

# Project tree mounted at its real host path so absolute paths in
# CMakeCache.txt / build.ninja line up identically inside and out.
MOUNT_ARGS=(
    --mount "type=bind,source=$ROOT,target=$ROOT"
)

# Auto-mount the game directory: the `deploy` target copies binaries into it,
# so it has to be writable from inside the container. Peek at the forwarded
# args for `--game-dir <path>` (the arg form build-linux.sh accepts), falling
# back to the AMNESIA_GAME_DIRECTORY env var that build-linux.sh defaults to.
# Args are not consumed — build-linux.sh re-parses them inside the container.
#
# When --no-deploy is passed, nothing gets written to the game dir, so don't
# bother resolving/mounting it (and don't require it to exist on the host).
GAME_DIR=""
NO_DEPLOY=0
_prev=""
for _a in "$@"; do
    if [[ "$_prev" == "--game-dir" ]]; then
        GAME_DIR="$_a"
    fi
    [[ "$_a" == "--no-deploy" ]] && NO_DEPLOY=1
    _prev="$_a"
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
        # Mount the resolved real path AT the original path inside, so cmake
        # — which references the original path string via AMNESIA_GAME_DIRECTORY
        # — reads through the bind mount and sees the real contents.
        GAME_DIR_REAL="$(realpath "$GAME_DIR")"
        MOUNT_ARGS+=(--mount "type=bind,source=$GAME_DIR_REAL,target=$GAME_DIR")
        # Forward the env var too so the in-container build-linux.sh picks it
        # up as its default when --game-dir wasn't passed explicitly.
        ENV_ARGS+=(-e "AMNESIA_GAME_DIRECTORY=$GAME_DIR")
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
# slangc tree referenced via -DSLANGC_EXECUTABLE=...
if [[ -n "${AMNESIA_DOCKER_MOUNTS:-}" ]]; then
    IFS=':' read -r -a _extra <<<"$AMNESIA_DOCKER_MOUNTS"
    for p in "${_extra[@]}"; do
        [[ -e "$p" ]] || { echo "error: AMNESIA_DOCKER_MOUNTS path does not exist: $p" >&2; exit 1; }
        MOUNT_ARGS+=(--mount "type=bind,source=$p,target=$p")
    done
fi

exec docker run --rm "${TTY_ARGS[@]}" "${USER_ARGS[@]}" \
    "${ENV_ARGS[@]}" \
    "${MOUNT_ARGS[@]}" \
    -w "$ROOT" \
    "$IMAGE" \
    ./build-linux.sh "$@"

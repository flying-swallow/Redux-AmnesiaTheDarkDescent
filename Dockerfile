# Ubuntu 24.04 build environment for Amnesia64.
#
# Provides every system dev library the in-tree builds of SDL2, openal-soft,
# and the HPL2 engine pull in via #include. The Linux CI workflow
# (.github/workflows/build.yml) installs a smaller set because GitHub's
# ubuntu-latest image already ships some headers; this Dockerfile lists the
# full set explicitly so a clean container can build.
#
# Not a runtime image — intended only as a build sandbox. Invoke via
# ./build-linux-docker.sh, which bind-mounts the source tree and runs
# build-linux.sh inside the container so build outputs land back on the host.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang ninja-build cmake \
        git ca-certificates curl \
        libx11-dev libxext-dev libxi-dev libxcursor-dev libxrandr-dev libxss-dev \
        libgl-dev libglu1-mesa-dev libegl1-mesa-dev \
        libwayland-dev libxkbcommon-dev libdecor-0-dev \
        libasound2-dev libpulse-dev libdbus-1-dev libsamplerate0-dev \
        liblcms2-dev \
    && rm -rf /var/lib/apt/lists/*

# The wrapper bind-mounts the host source tree at its real host path. Whichever
# uid ends up owning the files inside the container, treat any mounted repo as
# trusted so submodule init doesn't trip the safe.directory guard.
RUN git config --system --add safe.directory '*'

CMD ["bash"]

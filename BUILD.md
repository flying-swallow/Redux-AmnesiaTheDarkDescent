# Building Amnesia64

Amnesia64 uses CMake (>= 3.18) defined in [`CMakeLists.txt`](CMakeLists.txt), with the per-platform build configurations captured in [`CMakePresets.json`](CMakePresets.json) (generator, build type, RI backend, architecture). The wrapper scripts below cover the common paths so you don't have to remember per-platform build commands — under the hood they drive the CMake presets, which you can also invoke directly (see [§5](#5-cmake-presets-dropping-the-wrappers)).

A Visual Studio solution ([`Amnesia.sln`](Amnesia.sln)) is also shipped as an alternative Windows path — see [README.md](README.md) for details.

## Quick start

One script per platform — pick the one matching your host:

| Host                  | Command                       | RI backend |
| --------------------- | ----------------------------- | ---------- |
| Linux                 | `./build-linux.sh`            | Vulkan     |
| macOS                 | `./build-macos.sh`            | Metal      |
| Linux (containerized) | `./build-linux-docker.sh`     | Vulkan     |
| Windows (PowerShell)  | `.\build-windows.ps1`         | Vulkan     |

They all default to a release build, init submodules on first run, select the matching CMake preset, build, and stage assets via the `deploy` target. Output ends up in `build/bin/`. The RI rendering backend is chosen by the preset (`ENABLE_VULKAN` / `ENABLE_METAL`) — Vulkan on Linux/Windows, Metal on macOS.

## 1. Clone the repository

The build pulls in third-party dependencies as git submodules. The wrapper scripts will run `git submodule update --init --recursive` automatically on first run, but you can also clone recursively up front:

```bash
git clone --recurse-submodules https://github.com/<your-fork>/Amnesia64.git
cd Amnesia64
```

## 2. Game assets (required for `deploy`)

The `deploy` target copies the freshly built binaries into your Amnesia: The Dark Descent install folder so the executable can find its assets. You need a legitimate copy of **Amnesia: The Dark Descent** (e.g. via Steam).

Point the build at it with `-DAMNESIA_GAME_DIRECTORY=...` (the wrapper scripts also accept `--game-dir` / `-GameDir`):

```bash
./build-linux.sh   release --game-dir "$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent"
.\build-windows.ps1 release -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
```

On Linux, `AMNESIA_GAME_DIRECTORY` defaults to `~/.local/share/Steam/steamapps/common/Amnesia The Dark Descent`. On Windows the path must be set explicitly (the wrapper also accepts the `ATDD_DIR` env var used by the VS solution).

## 3. `build-linux.sh`

```
./build-linux.sh [release|debug] [options] [-- <extra cmake args>]

  --clean              Remove build/ before configuring
  --no-deploy          Skip the 'deploy' target (assets staging)
  --game-dir <path>    Path to your Amnesia: The Dark Descent install
  -h, --help           Show help
```

Examples:

```bash
./build-linux.sh                                # native release
./build-linux.sh debug                          # native debug
./build-linux.sh release --clean                # wipe build dir and rebuild
./build-linux.sh release --game-dir "$HOME/atdd"
./build-linux.sh release -- -DUSE_SYSTEM_SDL2=ON -DUSE_GRAPHICS_WAYLAND=OFF
```

The script selects the `linux-release` / `linux-debug` CMake preset (`cmake --preset … && cmake --build --preset …`) and forwards any args after `--` straight to the configure step.

### macOS (`build-macos.sh`)

Mirrors `build-linux.sh` (same `release|debug`, `--clean`, `--no-deploy`, `--game-dir` flags) but selects the `macos-release` / `macos-debug` presets, which use the **Ninja** generator and build the native **Metal** RI backend (`ENABLE_METAL=ON`) — no Vulkan loader / MoltenVK required.

```bash
brew install ninja                              # required generator
./build-macos.sh                                # native release (Metal)
./build-macos.sh debug --clean
./build-macos.sh release --game-dir "$HOME/Library/Application Support/Steam/steamapps/common/Amnesia The Dark Descent"
```

### Containerized build (`build-linux-docker.sh`)

`build-linux-docker.sh` runs `build-linux.sh` inside an Ubuntu 24.04 container so the build doesn't need host-installed dev libraries. The image is defined by [`Dockerfile`](Dockerfile) at the repo root and includes everything the in-tree builds of SDL2, openal-soft, and the HPL2 engine pull in via `#include`.

Works with either Docker or rootless Podman (auto-detected via `command -v podman`; force one with `AMNESIA_DOCKER_RUNTIME=podman|docker`). All args are forwarded to `build-linux.sh` unchanged:

```bash
./build-linux-docker.sh                                # release
./build-linux-docker.sh debug --clean
./build-linux-docker.sh release --game-dir "$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent" --no-deploy
```

The project tree is bind-mounted at its **real host path** inside the container, so `build/`, `compile_commands.json`, and the source paths recorded in `CMakeCache.txt` line up between containerized and native runs — you can switch between `./build-linux.sh` and the wrapper without `--clean` (though the first switch is worth wiping `build/` to avoid `.o` files compiled against a different libstdc++).

The `--game-dir` path (or the `AMNESIA_GAME_DIRECTORY` env var) is automatically bind-mounted at the same host path inside the container so the `deploy` target can stage binaries straight into the game folder. Use `--no-deploy` if you don't have the game installed.

#### Exposing host tools to the container

Anything outside the project tree — e.g. a locally-built `slangc` — has to be bind-mounted explicitly via `AMNESIA_DOCKER_MOUNTS` (colon-separated host paths; each is mounted at the same path inside, so cmake args referencing host paths "just work"):

```bash
AMNESIA_DOCKER_MOUNTS=/home/me/projects/slang \
    ./build-linux-docker.sh release \
    -- -DSLANGC_EXECUTABLE=/home/me/projects/slang/build/Release/bin/slangc
```

#### Rootless Podman: cleaning a `build/` owned by a subuid

Rootless podman maps your host uid → container root. The wrapper relies on this and does **not** pass `--user` in podman mode. If you have a `build/` directory left over from a wrapper version that did pass `--user` (or from any other process running as a different uid), it'll contain files owned by a subuid your host shell can't touch. Wipe it via `podman unshare`:

```bash
podman unshare rm -rf build/
```

## 4. `build-windows.ps1`

Run from a *Developer PowerShell for VS 2022* so MSBuild and CMake are on `PATH`:

```powershell
.\build-windows.ps1                                  # release
.\build-windows.ps1 debug                            # debug
.\build-windows.ps1 release -Clean                   # wipe build dir
.\build-windows.ps1 release -NoDeploy                # skip asset staging
.\build-windows.ps1 release -GameDir "C:\...\Amnesia The Dark Descent"
.\build-windows.ps1 release -- -DUSE_SYSTEM_ZLIB=ON
```

The script selects the `windows` configure preset (VS 17 2022 / x64) and the `windows-release` / `windows-debug` build preset. Output goes to `build\bin\`.

## 5. CMake presets (dropping the wrappers)

The wrapper scripts just add submodule init, `--clean`, `--game-dir`, and the `deploy` step around the CMake presets. For IDE integration (VS, VS Code, CLion all read `CMakePresets.json`) or manual builds, use the presets directly:

```bash
cmake --preset macos-debug                 # configure (Ninja, Metal, Debug)
cmake --build  --preset macos-debug        # build
cmake --build  --preset macos-debug --target deploy
```

| Configure preset | Generator           | Backend | Build type |
| ---------------- | ------------------- | ------- | ---------- |
| `linux-release` / `linux-debug`   | Unix Makefiles      | Vulkan  | Release / Debug |
| `macos-release` / `macos-debug`   | Ninja               | Metal   | Release / Debug |
| `windows`        | Visual Studio 17 2022 (x64) | Vulkan  | (multi-config — pick at build time) |

`cmake --list-presets` shows the configure presets available on your host (the `windows` preset only appears on Windows). Build presets mirror the names (`windows-release` / `windows-debug` select the config for the multi-config VS generator). The game directory stays out of the committed presets — pass it as an extra `-D` (`cmake --preset linux-release -D AMNESIA_GAME_DIRECTORY=...`) or keep personal overrides in a gitignored `CMakeUserPresets.json`. The presets also set `CMAKE_POLICY_VERSION_MINIMUM=3.5` so the older bundled submodules (e.g. vorbis) configure under CMake 4.x; pass that yourself if you bypass the presets.

### Linux dependencies

GCC 10+ or Clang 11+, CMake >= 3.18 (>= 3.21 to use the presets), GNU Make or Ninja, an assembler (NASM/GAS), plus X11/Wayland headers if those backends are enabled. Bundled dependency sources under `extern/` and `HPL2/dependencies/` cover the rest, so a system-wide install of SDL2/OpenAL/etc. is optional.

### Shader compilers

GLSL shaders (`.vert`, `.frag`, `.comp`, `.rgen`, etc.) compile to SPIR-V via the bundled `glslang` submodule. Slang shaders (`.slang`) compile via a prebuilt `slangc` automatically downloaded from the [Slang releases page](https://github.com/shader-slang/slang/releases) at configure time. Override with `-DSLANGC_EXECUTABLE=/path/to/slangc` (e.g. from the Vulkan SDK) to skip the download and use a local install. The pinned Slang version is set in [`cmake/slang.cmake`](cmake/slang.cmake) (`SLANG_VERSION`).

Fully manual (no preset) — set the backend and the policy-min explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_VULKAN=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DAMNESIA_GAME_DIRECTORY="$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent"
cmake --build build -j"$(nproc)"
cmake --build build --target deploy -j"$(nproc)"
```

### Windows

From a *Developer Command Prompt for VS 2022*:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DAMNESIA_GAME_DIRECTORY="C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
cmake --build build --config Release
cmake --build build --target deploy --config Release
```

The Visual Studio solution `Amnesia.sln` is an alternative path on Windows — see the [README](README.md#building-with-visual-studio-amnesiasln) for the VS-specific setup (notably the `ATDD_DIR` env var).

## 6. Useful CMake options

Defined in [`CMakeLists.txt`](CMakeLists.txt). Pass via `-D<NAME>=<VALUE>` (after `--` if using the wrapper scripts).

| Option                | Default | Purpose                                              |
| --------------------- | ------- | ---------------------------------------------------- |
| `AMNESIA_GAME_DIRECTORY` | (see above) | Path to the installed Amnesia: TDD game folder   |
| `ENABLE_VULKAN`       | ON      | Build the Vulkan RI backend (`DEVICE_SUPPORT_VULKAN`) |
| `ENABLE_METAL`        | OFF     | Build the Metal RI backend (Apple only). Enable exactly one backend — the renderer compiles one at a time |
| `USE_GRAPHICS_X11`    | ON      | (Linux) X11 backend                                  |
| `USE_GRAPHICS_WAYLAND`| ON      | (Linux) Wayland backend                              |
| `USE_SYSTEM_ZLIB`     | OFF     | Use system zlib instead of bundled                   |
| `USE_SYSTEM_OPENAL`   | OFF     | Use system OpenAL Soft instead of bundled            |
| `USE_SYSTEM_TINYXML`  | OFF     | Use system TinyXML2 instead of bundled               |
| `USE_SYSTEM_SDL2`     | OFF     | Use system SDL2 instead of bundled                   |
| `USE_SYSTEM_OGG`      | OFF     | Use system Ogg instead of bundled                    |
| `USE_SYSTEM_VORBIS`   | OFF     | Use system Vorbis instead of bundled                 |
| `USE_SYSTEM_DEVIL`    | OFF     | Use system DevIL instead of bundled                  |

## 7. Running

The `deploy` target drops the freshly built binaries into your Amnesia: The Dark Descent install folder (the one you pointed `-DAMNESIA_GAME_DIRECTORY` at), so you can launch them straight from there.

If the game complains about missing files on startup, re-run the deploy step (`cmake --build build --target deploy`) after pointing `-DAMNESIA_GAME_DIRECTORY=...` at a valid game install.

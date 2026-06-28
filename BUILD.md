# Building Amnesia64

Amnesia64 has two maintained build paths: CMake (primarily for Linux/native builds) and a Premake-generated Visual Studio solution for Windows. The wrapper scripts below cover the common paths so you don't have to remember per-platform build commands.

A Visual Studio solution ([`Amnesia.sln`](Amnesia.sln)) is also shipped as an alternative Windows path — see [README.md](README.md) for details. macOS builds are not yet supported.

## Quick start

One script per platform — pick the one matching your host:

| Host                  | Command                       |
| --------------------- | ----------------------------- |
| Linux                 | `./build-linux.sh`            |
| Linux (containerized) | `./build-linux-docker.sh`     |
| Windows (PowerShell)  | `.\build-windows.ps1`         |

Each defaults to a release build, initializes submodules on first run, and builds the chosen configuration. Linux native builds use CMake and output under `build/`; the Windows wrapper generates `build-premake\Amnesia.sln` with Premake and outputs under `build-premake\amnesia\<Config>\`.

## 1. Clone the repository

The build pulls in third-party dependencies as git submodules. The wrapper scripts will run `git submodule update --init --recursive` automatically on first run, but you can also clone recursively up front:

```bash
git clone --recurse-submodules https://github.com/<your-fork>/Amnesia64.git
cd Amnesia64
```

## 2. Game assets (required for `deploy`)

The `deploy` target copies your installed Amnesia: The Dark Descent assets next to the freshly built executable so it can find its configs, maps, scripts, and core data. You need a legitimate copy of **Amnesia: The Dark Descent** (e.g. via Steam).

Point the build at it with `-DAMNESIA_GAME_DIRECTORY=...` (the wrapper scripts also accept `--game-dir` / `-GameDir`):

```bash
./build-linux.sh   release --game-dir "$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent"
.\build-windows.ps1 release -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
```

On Linux, `AMNESIA_GAME_DIRECTORY` defaults to `~/.local/share/Steam/steamapps/common/Amnesia The Dark Descent`. On Windows the wrapper accepts `-GameDir`, `ATDD_DIR`, or `AMNESIA_GAME_DIRECTORY`; when a path is available it passes `ATDD_DIR` through to MSBuild for the generated VS projects.

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

The script invokes plain CMake with `-DCMAKE_BUILD_TYPE=<Release|Debug>` and forwards any args after `--` straight to the configure step.

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

Requires `premake5.exe` on `PATH` and a VS 2026 install (MSBuild is auto-located via `vswhere`, so any PowerShell works — not just a Developer PowerShell):

```powershell
.\build-windows.ps1                                  # release
.\build-windows.ps1 debug                            # debug
.\build-windows.ps1 release -Clean                   # wipe build dir
.\build-windows.ps1 release -NoDeploy                # skip asset staging
.\build-windows.ps1 release -GameDir "C:\...\Amnesia The Dark Descent"
.\build-windows.ps1 release -- --with-tools=no
```

The script is a thin command-line wrapper around the Premake VS workflow: it generates a Visual Studio 2026 solution under `build-premake\` via `premake5 vs2026`, builds it with `msbuild` targeting `x64`, and optionally runs the Premake `deploy` action. Extra args after `--` are forwarded to `premake5 vs2026` (premake options, e.g. `--with-tools=no`, `--slangc=...`), not to MSBuild. Generated project files and output stay under `build-premake\`; runtime output goes to `build-premake\amnesia\<Config>\`.

## 5. Native build details (when you want to drop the wrappers)

### Linux

Dependencies: GCC 10+ or Clang 11+, CMake >= 3.18, GNU Make or Ninja, an assembler (NASM/GAS), plus X11/Wayland headers if those backends are enabled. Bundled dependency sources under `HPL2/extern/` cover the rest, so a system-wide install of SDL2/OpenAL/etc. is optional.

### Shader compilers

GLSL shaders (`.vert`, `.frag`, `.comp`, `.rgen`, etc.) compile to SPIR-V via the bundled `glslang` submodule. Slang shaders (`.slang`) compile via a prebuilt `slangc` automatically downloaded from the [Slang releases page](https://github.com/shader-slang/slang/releases) at configure time. Override with `-DSLANGC_EXECUTABLE=/path/to/slangc` (e.g. from the Vulkan SDK) to skip the download and use a local install. The pinned Slang version is set in [`cmake/slang.cmake`](cmake/slang.cmake) (`SLANG_VERSION`).

The **premake** build mirrors this in pure Lua (premake's built-in `http.download` + `zip.extract`, no extra tooling): running `premake5` auto-downloads the pinned `slangc` into `build-premake/_deps/slang-prebuilt/` at configure time. Override with `--slangc=/path/to/slangc` to skip the download. The pinned version lives in [`premake/helpers.lua`](premake/helpers.lua) (`SLANG_VERSION`), kept in sync with `cmake/slang.cmake`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DAMNESIA_GAME_DIRECTORY="$HOME/.steam/steam/steamapps/common/Amnesia The Dark Descent"
cmake --build build -j"$(nproc)"
cmake --build build --target deploy -j"$(nproc)"
```

### Windows

From a *Developer Command Prompt for VS 2026*:

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

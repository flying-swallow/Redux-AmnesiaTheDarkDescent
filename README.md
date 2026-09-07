# Amnesia 64
64-bit Windows port of Amnesia: The Dark Descent

## Key changes:
- Can be compiled in both 32-bit and 64-bit modes using VS2019 with latest build tools.
- Single solution file for all projects (main game, HPL2, dependencies and editors). No need to compile the engine separately.
- Produces self-contained .exe files without dependency on 3rd party dlls (this prevents cluttering user's game folder with 64-bit dlls).
- Some libraries were changed, most notably:
	- SDL2 was upgraded from 2.0.4 to 2.0.12
	- alut was replaced with freealut
	- Newton Dynamics was upgraded from 2.08 to 2.32 (I simply couldn't find the source code for 2.08)
	- Fbx support is temporarily removed (I'm planning to re-implement it using OpenFBX)

The project builds with **premake5**, using one project model for Linux and Windows; see [BUILD.md](BUILD.md) for the full instructions.

## Prerequisites
- A legitimate copy of **Amnesia: The Dark Descent** (e.g. via Steam). The build only produces the executable; assets, scripts, configs, and shaders come from your installed copy.
- **Git**. The project uses dependency submodules under `HPL2/extern/`; clone recursively or run `git submodule update --init --recursive`. On Windows, enable long paths first: `git config --system core.longpaths true`.
- **premake5** on `PATH` (`premake5.exe` on Windows), used to generate the build files.
- A C++20-capable toolchain — see each build path below for specifics.

```
git clone --recurse-submodules https://github.com/<your-fork>/Amnesia64.git
cd Amnesia64
```

## Building on Windows

Requires `premake5.exe` on `PATH` and Visual Studio with the *Desktop development with C++* workload. Run:

```
.\build-windows.ps1 [release|debug]
```

The wrapper generates `build-premake\Amnesia.sln` via `premake5 vs2026`, then builds it with MSBuild targeting `x64`. After generation, you can also open the generated solution directly in Visual Studio.

Its options include `-Clean` to remove `build-premake\` before generation, `-NoDeploy` to skip asset deployment, `-GameDir <path>` to provide the installed game folder, and `--` to pass extra arguments through to Premake (for example, `--with-tools=no`). `ATDD_DIR` is still honored as the game-folder environment variable and as the generated projects' debugger working directory; `-GameDir` also sets it for the wrapper build.

The [Windows workflow](.github/workflows/windows-build.yml) uses `premake5 vs2022`. `premake5.lua` does not pin `_ACTION`, so `vs2022` and `vs2026` generate the same project model.

> Note: per the most recent commits, post effects and the menu background are temporarily disabled on this branch while the renderer backend is being reworked.

## Building on Linux

Generate Makefiles and build with:

```
premake5 gmake2 [options]
make -C build-premake config=release -j"$(nproc)"
```

The built output is in `build-premake/amnesia/<Debug|Release>/`. The native wrapper performs generation, the build, Python tests, and optional deployment on the host: `./build-linux.sh [release|debug]`.

The containerized wrapper performs generation, the build, Python tests, and deployment in one step:

```
./build-linux-docker.sh [release|debug]
```

`cmake` is still required as an installed tool because `premake/external.lua` drives the bundled SDL2 and openal-soft builds with their own CMake; `--cmake=PATH` overrides which executable is used.

**Useful premake options** (see [BUILD.md](BUILD.md) for the full table):
- `--game-dir=PATH` — no default; required by the `deploy` action.
- `--with-tools=yes|no` — default `yes`.
- `--with-tests=yes|no` — default `yes`.
- `--with-python-tests=yes|no` — default `yes`.
- `--with-fsr=yes|no` — default `yes`.
- `--with-xess=yes|no` — default `yes`; Windows only and ignored on Linux.
- `--graphics-x11=on|off` — default `on`.
- `--graphics-wayland=on|off` — default `on`.
- `--slangc=PATH` — no explicit default; an extracted compiler is reused or the pinned release is downloaded when omitted.
- `--cmake=PATH` — default `cmake` on `PATH`.

## Running

First stage the game assets with:

```
premake5 deploy --game-dir="/path/to/Amnesia The Dark Descent"
```

This copies assets from the installed game into `build-premake/amnesia/Debug/` and `build-premake/amnesia/Release/`, excluding names beginning with `Amnesia` and files matching `*.rar`, `*.pdf`, `*.dll`, or `*.exe`. The result is a self-contained directory; launch the built executable from the corresponding output directory. `build-linux-docker.sh` runs this action for you unless `--no-deploy` is supplied.

If the game complains about missing files on startup, check that you built or deployed into the expected `build-premake/amnesia/<Debug|Release>/` directory, then re-run `premake5 deploy` with the correct install path.

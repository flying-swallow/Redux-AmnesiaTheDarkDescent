# Building Amnesia64

Amnesia64 uses CMake (>= 3.15) defined in [`CMakeLists.txt`](CMakeLists.txt). The wrapper scripts below cover the common paths so you don't have to remember per-platform build commands.

A Visual Studio solution ([`Amnesia.sln`](Amnesia.sln)) is also shipped as an alternative Windows path — see [README.md](README.md) for details. macOS builds are not yet supported.

## Quick start

One script per platform — pick the one matching your host:

| Host                 | Command                       |
| -------------------- | ----------------------------- |
| Linux                | `./build-linux.sh`            |
| Windows (PowerShell) | `.\build-windows.ps1`         |

Both default to a release build, init submodules on first run, configure CMake, build, and stage assets via the `deploy` target. Output ends up in `build/bin/`.

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

The script invokes plain CMake with `-DCMAKE_BUILD_TYPE=<Release|Debug>` and forwards any args after `--` straight to the configure step.

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

The script uses the `Visual Studio 17 2022` generator targeting `x64`. Output goes to `build\bin\`.

## 5. Native build details (when you want to drop the wrappers)

### Linux

Dependencies: GCC 10+ or Clang 11+, CMake >= 3.15, GNU Make or Ninja, an assembler (NASM/GAS), plus X11/Wayland headers if those backends are enabled. Bundled dependency sources under `extern/` and `HPL2/dependencies/` cover the rest, so a system-wide install of SDL2/OpenAL/etc. is optional.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
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

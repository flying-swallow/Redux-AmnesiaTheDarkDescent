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

The repository ships **two parallel build systems**: a Visual Studio solution (`Amnesia.sln`) intended for Windows development, and a CMake project (`CMakeLists.txt`) used for Linux/macOS and as an alternative on Windows. They build the same code from the same dependency sources — pick whichever fits your platform.

## Prerequisites
- A legitimate copy of **Amnesia: The Dark Descent** (e.g. via Steam). The build only produces the executable; assets, scripts, configs, and shaders come from your installed copy.
- **Git**. The tree includes bundled dependency sources under `extern/` and `HPL2/dependencies/`, so no submodule init is required. On Windows, enable long paths first: `git config --system core.longpaths true`.
- **Perl** on `PATH`, used by `HPL2/core/buildcounter.pl` to stamp build IDs during compilation. Strawberry Perl works.
- A C++20-capable toolchain — see each build path below for specifics.

```
git clone https://github.com/<your-fork>/Amnesia64.git
cd Amnesia64
```

## Building with Visual Studio (`Amnesia.sln`)

Recommended path on Windows.

**Required tools**
- **Visual Studio 2019** or newer with the *Desktop development with C++* workload (v142 or v143 toolset and Windows 10 SDK).

**Steps**
1. Open `Amnesia.sln` in Visual Studio.
2. Pick a configuration: `Debug|x64`, `Release|x64`, `Debug|Win32`, or `Release|Win32`.
3. Right-click **Lux** → *Set as Startup Project*.
4. Define an environment variable `ATDD_DIR` pointing to your installed Amnesia game folder, then restart Visual Studio:
	```
	setx ATDD_DIR "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
	```
	The Lux debugger's working directory is `$(ATDD_DIR)` (see `amnesia\src\game\Lux.vcxproj.user`), which is what lets the freshly built exe find game assets when you press F5.
5. Build the solution (Ctrl+Shift+B). All bundled dependencies (SDL2, AngelScript, Newton, DevIL, GLEW, jpeg, png, ogg, vorbis, theora, zlib, freealut) compile from source as part of the solution.
6. Run/debug **Lux**. It will launch into your existing game install.

> Note: per the most recent commits, post effects and the menu background are temporarily disabled on this branch while the renderer backend is being reworked.

## Building with CMake (`CMakeLists.txt`)

Used for Linux and macOS, and works on Windows as an alternative to the VS solution.

**Required tools**
- **CMake 3.5+**
- A C++20 compiler: GCC 10+, Clang 11+, or MSVC matching the VS prerequisites above.
- An assembler (the build calls `enable_language(ASM)`). NASM/GAS on Linux; MASM ships with MSVC.

**Steps**
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAMNESIA_GAME_DIRECTORY="/path/to/Amnesia The Dark Descent"
cmake --build build --config Release -j
cmake --install build
```

The install step copies the game's data files (excluding the original executables and DLLs) next to the freshly built binaries via `amnesia/copy_game_assets.cmake`, producing a self-contained `build/bin/` you can run directly.

**Useful CMake options** (all `OFF` by default — bundled sources are used otherwise):
- `USE_SYSTEM_ZLIB`, `USE_SYSTEM_OPENAL`, `USE_SYSTEM_TINYXML`, `USE_SYSTEM_SDL2`, `USE_SYSTEM_OGG`, `USE_SYSTEM_VORBIS`, `USE_SYSTEM_DEVIL`
- Linux display backends: `USE_GRAPHICS_X11=ON`, `USE_GRAPHICS_WAYLAND=ON` (both on by default)
- `AMNESIA_GAME_DIRECTORY` defaults to `~/.local/share/Steam/steamapps/common/Amnesia The Dark Descent` on Linux; on other platforms it must be set explicitly.

## Running

Launch the built `Lux` (or `Amnesia`) executable from a directory that contains the game's `config/`, `entities/`, `maps/`, `core/`, etc.
- **Visual Studio path**: that directory is your `ATDD_DIR`.
- **CMake path**: that directory is the `bin/` folder produced by `cmake --install`.

If the game complains about missing files on startup, the working directory is wrong — fix `ATDD_DIR` (VS) or re-run the install step (CMake).

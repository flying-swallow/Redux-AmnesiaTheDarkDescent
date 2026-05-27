# scripts/compile_slang_shaders.py
import os
import sys
import subprocess
from pathlib import Path

STAGE_SUFFIXES = {
    ".vert.slang": "vertex",
    ".frag.slang": "fragment",
    ".comp.slang": "compute",
    ".cs.slang": "compute",
    ".geom.slang": "geometry",
    ".tesc.slang": "hull",
    ".tese.slang": "domain",
    ".rgen.slang": "raygeneration",
    ".rchit.slang": "closesthit",
    ".rmiss.slang": "miss",
    ".rahit.slang": "anyhit",
    ".rint.slang": "intersection",
    ".rcall.slang": "callable",
    ".rt.slang": None,  # Usually multiple ray-tracing entry points; rely on [shader(...)]
    ".3d.slang": None,  # Project-specific naming; rely on [shader(...)]
}

def find_slangc() -> str:
    env = os.environ.get("SLANGC")
    if env:
        return env

    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        for subdir in ("Bin", "bin"):
            candidate = Path(vulkan_sdk) / subdir / ("slangc.exe" if os.name == "nt" else "slangc")
            if candidate.exists():
                return str(candidate)

    return "slangc"

def get_stage(path: Path):
    name = path.name
    for suffix, stage in STAGE_SUFFIXES.items():
        if name.endswith(suffix):
            return stage
    return None

def is_entry_shader(path: Path) -> bool:
    return any(path.name.endswith(suffix) for suffix in STAGE_SUFFIXES)

def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: compile_slang_shaders.py <shader_source_dir> <output_dir> [include_dir ...]")
        return 1

    source_dir = Path(sys.argv[1]).resolve()
    output_dir = Path(sys.argv[2]).resolve()
    include_dirs = [source_dir] + [Path(p).resolve() for p in sys.argv[3:]]

    output_dir.mkdir(parents=True, exist_ok=True)

    slangc = find_slangc()

    shader_files = [
        p for p in source_dir.rglob("*.slang")
        if p.is_file() and is_entry_shader(p)
    ]

    failed = False

    for shader in shader_files:
        name = shader.name

        if name.endswith(".slang"):
            name = name[:-len(".slang")]

        out = output_dir / (name + ".spv")

        print(f"Compiling: {shader} -> {out}")

        cmd = [
            slangc,
            str(shader),
            "-target", "spirv",
            "-profile", "sm_6_6",
            "-fvk-use-entrypoint-name",
            "-matrix-layout-column-major",
            "-fvk-use-scalar-layout",
            "-o", str(out),
        ]

        for inc in include_dirs:
            cmd += ["-I", str(inc)]

        # Usually leave this OFF if your runtime creates Vulkan stages with pName = "main".
        # Turn it ON only if you want SPIR-V entry point names preserved.
        if os.environ.get("SLANG_KEEP_ENTRYPOINT_NAMES") == "1":
            cmd.append("-fvk-use-entrypoint-name")

        # Optional: only useful if your entry point is literally `main` and lacks [shader(...)].
        # For your Falcor-style Slang ports, relying on [shader(...)] is probably better.
        stage = get_stage(shader)
        if os.environ.get("SLANG_FORCE_STAGE") == "1" and stage:
            cmd += ["-stage", stage]

        result = subprocess.run(
            cmd,
            text=True,
            capture_output=True,
        )

        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr)

        if result.returncode != 0:
            failed = True

    return 1 if failed else 0

if __name__ == "__main__":
    raise SystemExit(main())
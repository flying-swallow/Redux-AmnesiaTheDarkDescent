# scripts/compile_shaders.py
import os
import sys
import subprocess
from pathlib import Path

SHADER_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rmiss", ".rahit", ".rint", ".rcall",
}

def find_glslc() -> str:
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        candidate = Path(vulkan_sdk) / "Bin" / "glslc.exe"
        if candidate.exists():
            return str(candidate)

    return "glslc"  # fallback to PATH

def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: compile_shaders.py <shader_source_dir> <output_dir>")
        return 1

    source_dir = Path(sys.argv[1]).resolve()
    output_dir = Path(sys.argv[2]).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    glslc = find_glslc()

    shader_files = [
        p for p in source_dir.rglob("*")
        if p.is_file() and p.suffix in SHADER_EXTS
    ]

    failed = False

    for shader in shader_files:
        rel = shader.relative_to(source_dir)
        out = output_dir / (str(rel).replace("\\", "_").replace("/", "_") + ".spv")

        # Skip if output is newer than source
        if out.exists() and out.stat().st_mtime >= shader.stat().st_mtime:
            print(f"Up to date: {shader}")
            continue

        print(f"Compiling: {shader} -> {out}")

        result = subprocess.run(
            [glslc, str(shader), "-o", str(out)],
            text=True,
            capture_output=True,
        )

        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr)
            failed = True

    return 1 if failed else 0

if __name__ == "__main__":
    raise SystemExit(main())
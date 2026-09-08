#!/usr/bin/env python3
"""Compile the Vulkan FSR3 Upscaler shader permutations with FidelityFX_SC."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable, Sequence


SHADER_SUBDIRECTORY = Path("sdk/src/backends/vk/shaders/fsr3upscaler")
GPU_INCLUDE_SUBDIRECTORY = Path("sdk/include/FidelityFX/gpu")
FSR3_INCLUDE_SUBDIRECTORY = GPU_INCLUDE_SUBDIRECTORY / "fsr3upscaler"
PASS_NAMES = (
    "ffx_fsr3upscaler_accumulate_pass",
    "ffx_fsr3upscaler_autogen_reactive_pass",
    "ffx_fsr3upscaler_debug_view_pass",
    "ffx_fsr3upscaler_luma_instability_pass",
    "ffx_fsr3upscaler_luma_pyramid_pass",
    "ffx_fsr3upscaler_prepare_inputs_pass",
    "ffx_fsr3upscaler_prepare_reactivity_pass",
    "ffx_fsr3upscaler_rcas_pass",
    "ffx_fsr3upscaler_shading_change_pass",
    "ffx_fsr3upscaler_shading_change_pyramid_pass",
)
VARIANT_SUFFIXES = ("", "_wave64", "_16bit", "_wave64_16bit")
PERMUTATION_DEFINES = (
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH",
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING",
)
FIXED_ARGS = (
    "-reflection",
    "-deps=gcc",
    "-DFFX_GPU=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
)
API_ARGS = (
    "-compiler=glslang",
    "-e",
    "CS",
    "--target-env",
    "vulkan1.2",
    "-S",
    "comp",
    "-Os",
    "-DFFX_GLSL=1",
)
HEADER_DATA_RE = re.compile(
    r"static const unsigned char g_[^\s]+_data\[\] = \{(.*?)\};", re.DOTALL
)
HEX_BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")
PERMUTATION_INCLUDE_BLOCK_RE = re.compile(
    r'(?P<block>(?:#include "[^"]+"\n)+)'
)
INDIRECTION_TABLE_RE = re.compile(
    r"(?P<prefix>static const uint32_t g_[A-Za-z0-9_]+_IndirectionTable\[\] = \{\n)"
    r"(?P<body>.*?)"
    r"(?P<suffix>\};\n\n)",
    re.DOTALL,
)
PERMUTATION_INFO_TABLE_RE = re.compile(
    r"(?P<prefix>static const [A-Za-z0-9_]+_PermutationInfo "
    r"g_[A-Za-z0-9_]+_PermutationInfo\[\] = \{\n)"
    r"(?P<body>(?:    \{[^\n]*\},\n)+)"
    r"(?P<suffix>\};\n\n)",
    re.DOTALL,
)


class DriverError(RuntimeError):
    """An expected, user-facing shader-driver failure."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", required=True, type=Path)
    parser.add_argument("--ffx-sc", required=True, type=Path)
    parser.add_argument("--glslang", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--spirv-val", type=Path)
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def require_executable(path: Path, name: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise DriverError(f"{name} was not found: {path}")
    if os.name != "nt" and not os.access(path, os.X_OK):
        raise DriverError(f"{name} is not executable: {path}")
    return path


def require_file(path: Path, name: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise DriverError(f"{name} was not found: {path}")
    return path


def iter_files(paths: Iterable[Path]) -> list[Path]:
    return sorted({path.resolve() for path in paths if path.is_file()})


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relevant_inputs(
    sdk_root: Path, ffx_sc: Path, glslang: Path, spirv_val: Path | None
) -> list[Path]:
    sdk_shader_dir = sdk_root / SHADER_SUBDIRECTORY
    gpu_include_dir = sdk_root / GPU_INCLUDE_SUBDIRECTORY
    shader_files = iter_files(sdk_shader_dir.glob("*.glsl"))
    include_files = iter_files(gpu_include_dir.rglob("*"))
    authority_files = iter_files(
        (
            sdk_root / "sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt",
            sdk_root / "sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt",
            sdk_root
            / "sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt",
            Path(__file__).resolve(),
        )
    )
    tool_files = [ffx_sc, glslang]
    if spirv_val is not None:
        tool_files.append(spirv_val)
    return iter_files((*shader_files, *include_files, *authority_files, *tool_files))


def input_signature(
    sdk_root: Path,
    out_dir: Path,
    ffx_sc: Path,
    glslang: Path,
    spirv_val: Path | None,
    jobs: int | None,
    inputs: Sequence[Path],
) -> str:
    manifest = {
        "format": 1,
        "sdk_root": str(sdk_root),
        "out_dir": str(out_dir),
        "ffx_sc": str(ffx_sc),
        "glslang": str(glslang),
        "spirv_val": str(spirv_val) if spirv_val else None,
        "jobs": jobs,
        "api_args": API_ARGS,
        "fixed_args": FIXED_ARGS,
        "permutation_defines": PERMUTATION_DEFINES,
        "inputs": [(str(path), sha256_file(path)) for path in inputs],
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def expected_headers() -> set[str]:
    return {
        f"{pass_name}{suffix}_permutations.h"
        for pass_name in PASS_NAMES
        for suffix in VARIANT_SUFFIXES
    }


def read_stamp(stamp: Path, output_dir: Path, depfile: Path | None) -> bool:
    try:
        metadata = json.loads(stamp.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    outputs = metadata.get("outputs")
    if not isinstance(outputs, list) or not all(isinstance(item, str) for item in outputs):
        return False
    if not all((output_dir / item).is_file() for item in outputs):
        return False
    if depfile is not None and not depfile.is_file():
        return False
    return True


def make_compile_command(
    ffx_sc: Path,
    glslang: Path,
    output_dir: Path,
    shader: Path,
    suffix: str,
    include_args: Sequence[str],
    jobs: int | None,
) -> list[str]:
    shader_name = shader.stem + suffix
    command = [str(ffx_sc), f"-glslangexe={glslang}"]
    command.extend(FIXED_ARGS)
    command.extend(API_ARGS)
    command.extend(f"-D{name}={{0,1}}" for name in PERMUTATION_DEFINES)
    command.extend((f"-name={shader_name}", f"-DFFX_HALF={1 if suffix.endswith('16bit') else 0}"))
    command.extend(include_args)
    if jobs is not None:
        command.append(f"-num-threads={jobs}")
    command.extend((f"-output={output_dir}", str(shader)))
    return command


def run_compiler(command: Sequence[str], shader: Path, verbose: bool) -> None:
    if verbose:
        print("$ " + shlex.join([str(item) for item in command]), flush=True)
    result = subprocess.run(
        [str(item) for item in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        compiler_output = result.stdout.strip() or "(glslangValidator produced no diagnostic output)"
        raise DriverError(
            f"FidelityFX_SC failed for {shader} (exit {result.returncode}).\n"
            f"--- glslangValidator error ---\n{compiler_output}\n"
            "--- end glslangValidator error ---"
        )
    if verbose and result.stdout.strip():
        print(result.stdout.rstrip(), flush=True)


def validate_spirv(output_dir: Path, spirv_val: Path, verbose: bool) -> None:
    for header in sorted(output_dir.glob("*.h")):
        if header.name.endswith("_permutations.h"):
            continue
        contents = header.read_text(encoding="utf-8")
        match = HEADER_DATA_RE.search(contents)
        if match is None:
            raise DriverError(f"generated binary header has no SPIR-V data array: {header}")
        binary = bytes(int(value, 16) for value in HEX_BYTE_RE.findall(match.group(1)))
        if not binary:
            raise DriverError(f"generated SPIR-V data array is empty: {header}")
        with tempfile.NamedTemporaryFile(prefix="ffx-", suffix=".spv", delete=False) as stream:
            temporary_spv = Path(stream.name)
            stream.write(binary)
        try:
            command = [str(spirv_val), "--target-env", "vulkan1.2", str(temporary_spv)]
            if verbose:
                print("$ " + shlex.join(command), flush=True)
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
            )
            if result.returncode != 0:
                diagnostic = result.stdout.strip() or "(spirv-val produced no diagnostic output)"
                raise DriverError(
                    f"spirv-val failed for {header} (exit {result.returncode}):\n{diagnostic}"
                )
        finally:
            temporary_spv.unlink(missing_ok=True)


def normalize_unique_blob_order(staging_dir: Path) -> None:
    """Make FidelityFX_SC's completion-order blob indices deterministic.

    FidelityFX_SC writes unique blobs as workers finish, so its include order,
    indirection values, and permutation-info rows can vary with scheduling.
    The blob headers themselves are content-hashed and remain untouched; only
    the generated accessor table is reordered and remapped as one unit.
    """
    for permutation_header in sorted(staging_dir.glob("*_permutations.h")):
        contents = permutation_header.read_text(encoding="utf-8")
        include_match = PERMUTATION_INCLUDE_BLOCK_RE.match(contents)
        if include_match is None:
            raise DriverError(
                f"generated permutation header has no include block: {permutation_header}"
            )

        include_names = re.findall(
            r'^#include "([^"]+)"$', include_match.group("block"), re.MULTILINE
        )
        if not include_names or len(set(include_names)) != len(include_names):
            raise DriverError(
                f"generated permutation header has invalid unique-blob includes: {permutation_header}"
            )
        sorted_names = sorted(include_names)
        old_index_by_name = {name: index for index, name in enumerate(include_names)}
        new_index_by_name = {name: index for index, name in enumerate(sorted_names)}
        old_to_new = {
            old_index_by_name[name]: new_index_by_name[name] for name in include_names
        }

        contents = (
            contents[: include_match.start()]
            + "".join(f'#include "{name}"\n' for name in sorted_names)
            + contents[include_match.end() :]
        )

        table_match = INDIRECTION_TABLE_RE.search(contents)
        if table_match is None:
            raise DriverError(
                f"generated permutation header has no indirection table: {permutation_header}"
            )
        old_table = [
            int(value) for value in re.findall(r"\b\d+\b", table_match.group("body"))
        ]
        if any(index not in old_to_new for index in old_table):
            raise DriverError(
                f"generated permutation header has an invalid indirection index: {permutation_header}"
            )
        new_table_body = re.sub(
            r"\b\d+\b",
            lambda match: str(old_to_new[int(match.group())]),
            table_match.group("body"),
        )
        contents = (
            contents[: table_match.start()]
            + table_match.group("prefix")
            + new_table_body
            + table_match.group("suffix")
            + contents[table_match.end() :]
        )

        info_match = PERMUTATION_INFO_TABLE_RE.search(contents)
        if info_match is None:
            raise DriverError(
                f"generated permutation header has no permutation info table: {permutation_header}"
            )
        info_rows = info_match.group("body").splitlines(keepends=True)
        if len(info_rows) != len(include_names):
            raise DriverError(
                f"generated permutation header has mismatched blob tables: {permutation_header}"
            )
        for name, row in zip(include_names, info_rows):
            blob_stem = Path(name).stem
            expected_prefix = f"    {{ g_{blob_stem}_size, g_{blob_stem}_data, "
            if not row.startswith(expected_prefix):
                raise DriverError(
                    f"generated permutation info does not match its blob include: {permutation_header}"
                )
        new_info_body = "".join(
            info_rows[old_index_by_name[name]] for name in sorted_names
        )
        contents = (
            contents[: info_match.start()]
            + info_match.group("prefix")
            + new_info_body
            + info_match.group("suffix")
            + contents[info_match.end() :]
        )
        permutation_header.write_text(contents, encoding="utf-8")


def makefile_escape(path: Path) -> str:
    value = path.resolve().as_posix()
    return (
        value.replace("\\", "\\\\")
        .replace(" ", "\\ ")
        .replace("#", "\\#")
        .replace("$", "$$")
    )


def write_depfile(depfile: Path, stamp: Path, inputs: Sequence[Path]) -> None:
    depfile.parent.mkdir(parents=True, exist_ok=True)
    temporary = depfile.with_name(f".{depfile.name}.{os.getpid()}.tmp")
    escaped_inputs = [makefile_escape(path) for path in inputs]
    lines = [f"{makefile_escape(stamp)}: \\"]
    lines.extend(f"  {path} \\" for path in escaped_inputs[:-1])
    if escaped_inputs:
        lines.append(f"  {escaped_inputs[-1]}")
    else:
        lines[-1] = f"{makefile_escape(stamp)}:"
    temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(temporary, depfile)


def write_stamp(stamp: Path, signature: str, outputs: Sequence[str]) -> None:
    stamp.parent.mkdir(parents=True, exist_ok=True)
    temporary = stamp.with_name(f".{stamp.name}.{os.getpid()}.tmp")
    metadata = {"signature": signature, "outputs": sorted(outputs)}
    temporary.write_text(json.dumps(metadata, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, stamp)


def publish(staging_dir: Path, output_dir: Path, outputs: set[str], old_outputs: set[str]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for filename in sorted(outputs):
        os.replace(staging_dir / filename, output_dir / filename)
    for filename in sorted(old_outputs - outputs):
        stale = output_dir / filename
        if stale.is_file():
            stale.unlink()


def compile_all(args: argparse.Namespace) -> int:
    sdk_root = args.sdk_root.expanduser().resolve()
    ffx_sc = require_executable(args.ffx_sc, "FidelityFX_SC")
    glslang = require_executable(args.glslang, "glslangValidator")
    spirv_val = require_executable(args.spirv_val, "spirv-val") if args.spirv_val else None
    shader_dir = sdk_root / SHADER_SUBDIRECTORY
    gpu_include_dir = sdk_root / GPU_INCLUDE_SUBDIRECTORY
    fsr3_include_dir = sdk_root / FSR3_INCLUDE_SUBDIRECTORY
    if not sdk_root.is_dir() or not (sdk_root / "sdk").is_dir():
        raise DriverError(f"--sdk-root is not a FidelityFX SDK root containing sdk/: {sdk_root}")
    if not gpu_include_dir.is_dir() or not fsr3_include_dir.is_dir():
        raise DriverError(f"SDK include tree is missing under {gpu_include_dir}")
    if not shader_dir.is_dir():
        raise DriverError(f"FSR3 Upscaler Vulkan shader directory is missing: {shader_dir}")
    shaders = iter_files(shader_dir.glob("*.glsl"))
    if {shader.stem for shader in shaders} != set(PASS_NAMES):
        found = ", ".join(shader.name for shader in shaders)
        raise DriverError(f"expected the pinned SDK's 10 FSR3 Upscaler GLSL entry points; found: {found}")

    output_dir = args.out_dir.expanduser().resolve()
    stamp = args.stamp.expanduser().resolve()
    depfile = args.depfile.expanduser().resolve() if args.depfile else None
    inputs = relevant_inputs(sdk_root, ffx_sc, glslang, spirv_val)
    signature = input_signature(
        sdk_root, output_dir, ffx_sc, glslang, spirv_val, args.jobs, inputs
    )
    if read_stamp(stamp, output_dir, depfile):
        try:
            metadata = json.loads(stamp.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            metadata = {}
        if metadata.get("signature") == signature:
            if args.verbose:
                print(f"FSR3 Upscaler shaders are up to date ({len(metadata['outputs'])} headers).")
            return 0

    include_args = (f"-I{gpu_include_dir}", f"-I{fsr3_include_dir}")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging_dir = Path(tempfile.mkdtemp(prefix=f".{output_dir.name}.", suffix=".tmp", dir=output_dir.parent))
    old_outputs: set[str] = set()
    try:
        try:
            old_metadata = json.loads(stamp.read_text(encoding="utf-8"))
            if isinstance(old_metadata.get("outputs"), list):
                old_outputs = {item for item in old_metadata["outputs"] if isinstance(item, str)}
        except (OSError, ValueError):
            pass

        for shader in shaders:
            for suffix in VARIANT_SUFFIXES:
                command = make_compile_command(
                    ffx_sc, glslang, staging_dir, shader, suffix, include_args, args.jobs
                )
                run_compiler(command, shader, args.verbose)

        generated = {path.name for path in staging_dir.glob("*.h") if path.is_file()}
        missing = expected_headers() - generated
        if missing:
            missing_list = ", ".join(sorted(missing))
            raise DriverError(f"FidelityFX_SC did not generate required permutation headers: {missing_list}")
        normalize_unique_blob_order(staging_dir)
        if spirv_val is not None:
            validate_spirv(staging_dir, spirv_val, args.verbose)

        # Each completed staged file is published with one atomic rename. The
        # output directory therefore never exposes a partially-written header.
        publish(staging_dir, output_dir, generated, old_outputs)
        if depfile is not None:
            write_depfile(depfile, stamp, inputs)
        write_stamp(stamp, signature, sorted(generated))
    except OSError as error:
        raise DriverError(f"could not publish FSR shader output: {error}") from error
    finally:
        shutil.rmtree(staging_dir, ignore_errors=True)

    if args.verbose:
        print(f"Generated {len(generated)} headers in {output_dir}.")
    return 0


def main() -> int:
    args = parse_args()
    if args.jobs is not None and args.jobs < 1:
        print("compile_fsr_shaders.py: error: --jobs must be a positive integer", file=sys.stderr)
        return 2
    try:
        return compile_all(args)
    except DriverError as error:
        print(f"compile_fsr_shaders.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

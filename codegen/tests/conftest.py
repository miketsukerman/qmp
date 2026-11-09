# SPDX-License-Identifier: Apache-2.0
"""Shared helpers: run protoc + protoc-gen-qmp in-process on a proto file."""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest
from google.protobuf import descriptor_pb2
from google.protobuf.compiler import plugin_pb2

from protoc_gen_qmp.__main__ import run

CODEGEN = Path(__file__).resolve().parents[1]
REPO = CODEGEN.parent
IDL = REPO / "idl"
UP_SPEC = REPO / "third_party" / "up-spec" / "up-core-api"
INCLUDE = REPO / "include"
GOLDEN = Path(__file__).resolve().parent / "golden"


def descriptor_set(
    proto: Path, extra_includes: tuple[Path, ...] = ()
) -> descriptor_pb2.FileDescriptorSet:
    """Compile a proto to a FileDescriptorSet with grpcio-tools' bundled protoc."""
    from grpc_tools import protoc  # noqa: PLC0415

    out = proto.parent / f".{proto.stem}.pb"
    wkt = Path(protoc.__file__).parent / "_proto"
    args = [
        "protoc",
        f"-I{UP_SPEC}",
        f"-I{IDL}",
        f"-I{wkt}",
        *(f"-I{p}" for p in extra_includes),
        "--include_imports",
        "--include_source_info",
        f"--descriptor_set_out={out}",
        str(proto),
    ]
    rc = protoc.main(args)
    if rc != 0:
        raise RuntimeError(f"protoc failed ({rc}) for {proto}")
    try:
        fds = descriptor_pb2.FileDescriptorSet()
        fds.ParseFromString(out.read_bytes())
    finally:
        out.unlink(missing_ok=True)
    return fds


@dataclass
class GenResult:
    error: str
    files: dict[str, str]


def generate(
    proto: Path, rel_name: str, params: str = "flat", extra_includes: tuple[Path, ...] = ()
) -> GenResult:
    """Run the plugin on `proto`, addressed as `rel_name` relative to an include root."""
    fds = descriptor_set(proto, extra_includes)
    req = plugin_pb2.CodeGeneratorRequest()
    req.parameter = params
    req.file_to_generate.append(rel_name)
    req.proto_file.extend(fds.file)
    resp = run(req)
    return GenResult(resp.error, {f.name: f.content for f in resp.file})


@pytest.fixture(scope="session")
def joint() -> GenResult:
    res = generate(IDL / "examples/demo/joint/v1/joint.proto", "examples/demo/joint/v1/joint.proto")
    assert res.error == "", res.error
    return res


def compiler(name: str) -> str | None:
    return shutil.which(name)


def compile_check(src_dir: Path, c_file: Path, lang: str, cc: str) -> None:
    std = "-std=c11" if lang == "c" else "-std=c++17"
    pedantic = ["-Wpedantic"] if lang == "c" else []
    cmd = [
        cc,
        "-x",
        lang,
        std,
        "-Wall",
        "-Wextra",
        *pedantic,
        "-Werror",
        f"-I{INCLUDE}",
        f"-I{src_dir}",
        "-fsyntax-only",
        str(c_file),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    assert proc.returncode == 0, f"{' '.join(cmd)}\n{proc.stderr}"


def write_outputs(res: GenResult, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for name, content in res.files.items():
        (dest / name).write_text(content)

# SPDX-License-Identifier: Apache-2.0
"""protoc plugin entry point: CodeGeneratorRequest on stdin -> CodeGeneratorResponse on stdout.

Parameters (comma separated, via --qmp_opt or --qmp_out=<params>:<dir>):
  flat   write outputs next to each other (stem.qmp.h) instead of mirroring the proto path
"""

from __future__ import annotations

import sys

from google.protobuf.compiler import plugin_pb2

from protoc_gen_qmp.emit import generate_file
from protoc_gen_qmp.model import build_universe
from protoc_gen_qmp.validate import CodegenError, validate_file


def run(request: plugin_pb2.CodeGeneratorRequest) -> plugin_pb2.CodeGeneratorResponse:
    response = plugin_pb2.CodeGeneratorResponse()
    response.supported_features = plugin_pb2.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL
    params = {p.strip() for p in request.parameter.split(",") if p.strip()}
    unknown = params - {"flat"}
    if unknown:
        response.error = f"protoc-gen-qmp: unknown parameter(s): {', '.join(sorted(unknown))}"
        return response
    universe = build_universe(list(request.proto_file))
    errors: list[str] = []
    for name in request.file_to_generate:
        fm = universe.files[name]
        try:
            validate_file(universe, fm)
        except CodegenError as e:
            errors.extend(e.errors)
            continue
        for out in generate_file(universe, fm, flat="flat" in params):
            f = response.file.add()
            f.name = out.name
            f.content = out.content
    if errors:
        response.error = "\n".join(errors)
    return response


def main() -> int:
    data = sys.stdin.buffer.read()
    request = plugin_pb2.CodeGeneratorRequest.FromString(data)
    response = run(request)
    sys.stdout.buffer.write(response.SerializeToString())
    sys.stdout.buffer.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())

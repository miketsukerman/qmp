#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Regenerate the checked-in descriptor modules in protoc_gen_qmp/_pb/ from
# third_party/up-spec and idl/qmp. Uses grpc_tools.protoc so the generated code matches the
# protobuf runtime pinned in pyproject.toml.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
out="$here/protoc_gen_qmp/_pb"
mkdir -p "$out"
python3 -m grpc_tools.protoc \
  -I"$root/third_party/up-spec/up-core-api" -I"$root/idl" \
  --python_out="$out" --pyi_out="$out" \
  uprotocol/uoptions.proto qmp/options.proto
# Make the generated modules importable as a package relative to protoc_gen_qmp._pb.
for d in uprotocol qmp; do
  touch "$out/$d/__init__.py"
done
echo "regenerated: $(find "$out" -name '*_pb2.py' | sort | tr '\n' ' ')"

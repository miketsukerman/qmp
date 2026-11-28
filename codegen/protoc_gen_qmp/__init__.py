# SPDX-License-Identifier: Apache-2.0
"""protoc-gen-qmp: protoc plugin emitting C11 bindings for qmp."""

# Kept equal to include/qmp/qmp_version.h (one product version, ADR-011);
# scripts/check-version.sh fails CI when they differ.
__version__ = "0.3.8"

# SPDX-License-Identifier: Apache-2.0
"""Generated output must compile as C11 (-Wpedantic) and C++17 with -Werror."""

from __future__ import annotations

from pathlib import Path

import pytest

from .conftest import GenResult, compile_check, compiler, write_outputs

COMPILERS = [c for c in ("gcc", "clang", "cc") if compiler(c)]


@pytest.mark.skipif(not COMPILERS, reason="no C compiler on PATH")
@pytest.mark.parametrize("cc", COMPILERS)
@pytest.mark.parametrize("lang", ["c", "c++"])
def test_generated_code_compiles(joint: GenResult, tmp_path: Path, cc: str, lang: str) -> None:
    write_outputs(joint, tmp_path)
    compile_check(tmp_path, tmp_path / "joint.qmp.c", lang, cc)

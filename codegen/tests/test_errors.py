# SPDX-License-Identifier: Apache-2.0
"""Every validation rule has a fixture under golden/errors/<name>.proto + <name>.expected."""

from __future__ import annotations

from pathlib import Path

import pytest

from .conftest import GOLDEN, generate

ERRORS = GOLDEN / "errors"
CASES = sorted(p.stem for p in ERRORS.glob("*.proto"))


@pytest.mark.parametrize("name", CASES)
def test_validation_error(name: str) -> None:
    proto = ERRORS / f"{name}.proto"
    expected = (ERRORS / f"{name}.expected").read_text().strip()
    res = generate(proto, proto.name, extra_includes=(ERRORS,))
    assert res.error, f"{name}: expected a validation error, got none"
    assert expected in res.error, f"{name}: expected '{expected}' in:\n{res.error}"
    assert f"{proto.name}:" in res.error, "error must carry file:line location"
    assert not res.files, "nothing must be generated on error"


def test_every_fixture_has_expectation() -> None:
    missing = [n for n in CASES if not (ERRORS / f"{n}.expected").exists()]
    assert not missing


def test_unknown_parameter_rejected() -> None:
    proto = Path(__file__).parents[2] / "idl/examples/demo/joint/v1/joint.proto"
    res = generate(proto, "examples/demo/joint/v1/joint.proto", params="flat,bogus")
    assert "unknown parameter(s): bogus" in res.error

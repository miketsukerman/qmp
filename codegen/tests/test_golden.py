# SPDX-License-Identifier: Apache-2.0
"""Golden-file comparison for the Joint example. Regenerate with UPDATE_GOLDEN=1 pytest."""

from __future__ import annotations

import json
import os

import pytest

from .conftest import GOLDEN, GenResult

EXPECTED_FILES = ("joint.qmp.h", "joint.qmp.c", "joint.reflect.json")


def test_generates_exactly_three_files(joint: GenResult) -> None:
    assert sorted(joint.files) == sorted(EXPECTED_FILES)


@pytest.mark.parametrize("name", EXPECTED_FILES)
def test_matches_golden(joint: GenResult, name: str) -> None:
    golden = GOLDEN / "joint" / name
    if os.environ.get("UPDATE_GOLDEN") == "1":
        golden.write_text(joint.files[name])
    assert golden.exists(), f"missing golden {golden}; run UPDATE_GOLDEN=1 pytest"
    assert joint.files[name] == golden.read_text(), f"{name} differs from golden"


def test_reflection_describes_design_layouts(joint: GenResult) -> None:
    refl = json.loads(joint.files["joint.reflect.json"])
    assert refl["qmp_reflect_version"] == 1
    assert refl["up_spec"] == "v1.5.8"
    svc = refl["services"][0]
    assert svc["service_id"] == 0x0101
    assert svc["uuri"] == "/101/1"
    assert svc["qnx_path"] == "qmp/0101-0000/v1"
    assert svc["max_msg_size"] == 24 + 48
    by_name = {m["name"]: m for m in svc["methods"]}
    assert by_name["SetTarget"]["uuri"] == "/101/1/1"
    assert by_name["SetTarget"]["in_size"] == 8
    assert by_name["Stop"]["oneway"] is True
    assert by_name["GetState"]["out_size"] == 48
    topic = svc["topics"][0]
    assert topic["id"] == 0x8001
    assert topic["uuri"] == "/101/1/8001"
    assert topic["mode"] == "LATEST"
    state = refl["messages"]["demo.joint.v1.JointState"]
    assert state["size"] == 48
    assert {f["name"]: f["offset"] for f in state["fields"]} == {
        "seq": 0,
        "timestamp_ns": 8,
        "position_rad": 16,
        "velocity": 20,
        "mode": 24,
    }


def test_header_contains_identifiers_and_asserts(joint: GenResult) -> None:
    h = joint.files["joint.qmp.h"]
    assert "#define DEMO_JOINT_V1_JOINT_SERVICE_ID   0x0101U" in h
    assert "#define DEMO_JOINT_V1_JOINT_M_SETTARGET 0x0001U" in h
    assert "#define DEMO_JOINT_V1_JOINT_T_STATECHANGED 0x8001U" in h
    assert 'DEMO_JOINT_V1_JOINT_UURI_STATECHANGED "/101/1/8001"' in h
    assert "QMP_STATIC_ASSERT(sizeof(demo_joint_v1_JointState) == 48" in h
    assert "QMP_STATIC_ASSERT(offsetof(demo_joint_v1_JointState, mode) == 24" in h
    assert "uint8_t _pad0[6];" in h
    assert 'extern "C"' in h
    # one-way and topics are declared but have no stubs in W1
    assert "demo_joint_v1_Joint_Stop(" not in h
    assert "int (*Stop)(void *ctx, const qmp_req_info_t *info)" in h


def test_source_embeds_reflection_blob(joint: GenResult) -> None:
    c = joint.files["joint.qmp.c"]
    refl = joint.files["joint.reflect.json"].encode()
    assert f"demo_joint_v1_Joint_reflect_json_len = {len(refl)}U;" in c
    assert "demo_joint_v1_JointState_validate" in c
    assert "if (m->mode.len > 16U)" in c

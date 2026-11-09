# SPDX-License-Identifier: Apache-2.0
"""Layout engine unit tests (no protoc): alignment, padding, helper structs."""

from __future__ import annotations

from protoc_gen_qmp.layout import LayoutTable, Struct
from protoc_gen_qmp.model import build_universe

from .conftest import IDL, descriptor_set


def _joint_structs() -> dict[str, Struct]:
    fds = descriptor_set(IDL / "examples/demo/joint/v1/joint.proto")
    universe = build_universe(list(fds.file))
    table = LayoutTable(universe)
    for fq in (".demo.joint.v1.JointState", ".demo.joint.v1.SetTargetRequest"):
        table.message(fq)
    out = dict(table.structs)
    out.update({h.c_name: h for h in table.helpers})
    return {s.c_name: s for s in out.values()}


def test_joint_state_layout_matches_design() -> None:
    st = _joint_structs()["demo_joint_v1_JointState"]
    assert st.size == 48
    assert st.align == 8
    offs = {f.name: f.offset for f in st.members if not f.name.startswith("_pad")}
    assert offs == {"seq": 0, "timestamp_ns": 8, "position_rad": 16, "velocity": 20, "mode": 24}


def test_set_target_request_has_no_padding() -> None:
    st = _joint_structs()["demo_joint_v1_SetTargetRequest"]
    assert st.size == 8
    assert not any(f.name.startswith("_pad") for f in st.members)


def test_string_helper_is_len_plus_data() -> None:
    st = _joint_structs()["demo_joint_v1_JointState_mode_t"]
    assert st.size == 18
    assert st.align == 2
    assert [f.name for f in st.members] == ["len", "data"]

# SPDX-License-Identifier: Apache-2.0
"""Build the template context and render the generated files for one proto file."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import jinja2

from protoc_gen_qmp import __version__
from protoc_gen_qmp.layout import HDR_SIZE, LayoutTable, Struct, decl_of
from protoc_gen_qmp.model import FileModel, Kind, Message, Service, Universe, c_ident
from protoc_gen_qmp.validate import reachable_messages

UP_SPEC_TAG = "v1.5.8"
REFLECT_VERSION = 1

_ENV = jinja2.Environment(
    loader=jinja2.PackageLoader("protoc_gen_qmp", "templates"),
    trim_blocks=True,
    lstrip_blocks=True,
    keep_trailing_newline=True,
    undefined=jinja2.StrictUndefined,
    autoescape=False,  # noqa: S701 — generating C, not HTML
)


def _wrap(items: list[str], indent: int = 4, budget: int = 72) -> str:
    """Join C parameters/arguments on one line when short, else one per line."""
    one = ", ".join(items)
    if len(one) <= budget:
        return one
    sep = ",\n" + " " * indent
    return sep.join(items)


_ENV.filters["wrap"] = _wrap


@dataclass
class Output:
    name: str
    content: str


def uuri(service_id: int, instance_id: int, major: int, resource: int | None) -> str:
    """uProtocol URI path: /<ue_id>/<major>/<resource>, upper-case hex, no leading zeros.

    VERIFY(up): V-UP-006 — format per basics/uri.adoc at v1.5.8 (lines 154-159).
    """
    ue_id = (instance_id << 16) | service_id
    path = f"/{ue_id:X}/{major:X}"
    return path if resource is None else f"{path}/{resource:X}"


def _acc(expr: str, name: str) -> str:
    return f"m->{name}" if expr == "*m" else f"{expr}.{name}"


def _validator_lines(st: Struct, expr: str, depth: int, out: list[str]) -> None:
    """Emit C statements checking bounds of the struct lvalue `expr` with layout `st`."""
    ind = "    " * (depth + 1)
    if st.kind in ("string", "bytes"):
        out.append(f"{ind}if ({_acc(expr, 'len')} > {st.max_n}U) {{")
        out.append(f"{ind}    return ERANGE;")
        out.append(f"{ind}}}")
        return
    if st.kind == "array":
        out.append(f"{ind}if ({_acc(expr, 'count')} > {st.max_n}U) {{")
        out.append(f"{ind}    return ERANGE;")
        out.append(f"{ind}}}")
        for member, helper in st.checks:
            idx = f"i{depth}"
            out.append(f"{ind}for (uint32_t {idx} = 0; {idx} < {_acc(expr, 'count')}; {idx}++) {{")
            _validator_lines(helper, f"{_acc(expr, member.name)}[{idx}]", depth + 1, out)
            out.append(f"{ind}}}")
        return
    for member, helper in st.checks:
        _validator_lines(helper, _acc(expr, member.name), depth, out)


def _struct_ctx(st: Struct) -> dict[str, Any]:
    return {
        "c_name": st.c_name,
        "kind": st.kind,
        "size": st.size,
        "align": st.align,
        "max_n": st.max_n,
        "members": [
            {
                "decl": decl_of(m),
                "offset": m.offset,
                "comment": m.comment,
                "kind": m.kind,
                "name": m.name,
            }
            for m in st.members
        ],
        "asserts": st.asserts,
    }


def _message_ctx(
    fq: str, msg: Message, st: Struct | None, table: LayoutTable, universe: Universe
) -> dict[str, Any]:
    fields = []
    for f in sorted(msg.fields, key=lambda x: x.number):
        member = next(m for m in st.members if m.name == f.name) if st else None
        entry: dict[str, Any] = {
            "name": f.name,
            "number": f.number,
            "type": f.kind.value,
            "repeated": f.repeated,
            "c_type": member.c_type if member else None,
            "offset": member.offset if member else None,
            "size": member.size if member else None,
        }
        if f.kind in (Kind.ENUM, Kind.MESSAGE):
            entry["type_name"] = f.type_name.lstrip(".")
        if f.kind == Kind.ENUM:
            entry["values"] = universe.enums[f.type_name].values
        if f.max_len is not None:
            entry["max_len"] = f.max_len
        if f.max_count is not None:
            entry["max_count"] = f.max_count
        fields.append(entry)
    return {
        "name": fq.lstrip("."),
        "c_type": st.c_name if st else None,
        "size": st.size if st else 0,
        "align": st.align if st else 0,
        "fields": fields,
    }


def _service_ctx(
    svc: Service, universe: Universe, table: LayoutTable, emitted: set[str]
) -> dict[str, Any]:
    assert svc.service_id is not None and svc.ver_major is not None
    prefix = c_ident(svc.fqname)
    macro = prefix.upper()
    ordered = reachable_messages(universe, svc)

    # Lay out in dependency order, grouping helper typedefs with the message that owns them.
    struct_groups: list[dict[str, Any]] = []
    for m in ordered:
        before = len(table.helpers)
        st = table.message(m.fqname)
        helpers = table.helpers[before:]
        if m.fqname in emitted:
            continue  # shared with an earlier service in this file
        emitted.add(m.fqname)
        if st is None:
            struct_groups.append({"empty": True, "name": m.fqname.lstrip(".")})
            continue
        vlines: list[str] = []
        _validator_lines(st, "*m", 0, vlines)
        struct_groups.append(
            {
                "empty": False,
                "name": m.fqname.lstrip("."),
                "helpers": [_struct_ctx(h) for h in helpers],
                "struct": _struct_ctx(st),
                "validator": vlines,
            }
        )

    def size_of(fq: str) -> int:
        st = table.structs.get(fq)
        return st.size if st else 0

    methods: list[dict[str, Any]] = []
    for meth in sorted(svc.methods, key=lambda x: x.method_id or 0):
        assert meth.method_id is not None
        in_st = table.structs.get(meth.input_type)
        out_st = table.structs.get(meth.output_type)
        params = ["qmp_client_t *c"]
        args = ["c"]
        ops_params = ["void *ctx", "qmp_req_t *req"]
        if in_st:
            params.append(f"const {in_st.c_name} *req")
            args.append("req")
            ops_params.append(f"const {in_st.c_name} *in")
        if out_st:
            params.append(f"{out_st.c_name} *rep")
            args.append("rep")
            ops_params.append(f"{out_st.c_name} *out")
        params.append("int32_t *status")
        args.append("status")
        ops_params.append("int32_t *status")
        tr_args = ["ctx", "req"]
        if in_st:
            tr_args.append(f"(const {in_st.c_name} *)in")
        if out_st:
            tr_args.append(f"({out_st.c_name} *)out")
        tr_args.append("status")
        methods.append(
            {
                "name": meth.name,
                "upper": meth.name.upper(),
                "id": meth.method_id,
                "params": params,
                "args": args,
                "ops_params": ops_params,
                "tr_args": tr_args,
                "call_req": "req, sizeof(*req)" if in_st else "NULL, 0",
                "call_rep": "rep, sizeof(*rep)" if out_st else "NULL, 0",
                "desc_flags": "QMP_MF_ONEWAY" if meth.oneway else "0U",
                "desc_in": f"(uint32_t)sizeof({in_st.c_name})" if in_st else "0U",
                "desc_out": f"(uint32_t)sizeof({out_st.c_name})" if out_st else "0U",
                "desc_validate": f"{in_st.c_name}_validate" if in_st and in_st.checks else "NULL",
                "desc_handler": "NULL /* one-way: W3 */"
                if meth.oneway
                else f"{prefix}_tr_{meth.name}",
                "uuri": uuri(svc.service_id, 0, svc.ver_major, meth.method_id),
                "oneway": meth.oneway,
                "in": in_st.c_name if in_st else None,
                "out": out_st.c_name if out_st else None,
                "in_size": size_of(meth.input_type),
                "out_size": size_of(meth.output_type),
                "in_validator": bool(in_st and in_st.checks),
                "input": meth.input_type.lstrip("."),
                "output": meth.output_type.lstrip("."),
                "allow_types": meth.allow_types,
                "permission_level": meth.permission_level,
            }
        )
    topics: list[dict[str, Any]] = []
    for t in sorted(svc.topics, key=lambda x: x.topic_id):
        topics.append(
            {
                "name": t.name,
                "upper": t.name.upper(),
                "id": t.topic_id,
                "uuri": uuri(svc.service_id, 0, svc.ver_major, t.topic_id),
                "message": t.resolved_message.lstrip("."),
                "c_type": table.structs[t.resolved_message].c_name,
                "size": size_of(t.resolved_message),
                "mode": t.mode,
                "depth": t.depth,
                "notification": t.notification,
            }
        )
    sizes: list[int] = [0]
    sizes += [int(m["in_size"]) for m in methods]
    sizes += [int(m["out_size"]) for m in methods]
    sizes += [int(t["size"]) for t in topics]
    max_payload = max(sizes)
    return {
        "name": svc.name,
        "package": svc.package,
        "fqname": svc.fqname.lstrip("."),
        "prefix": prefix,
        "macro": macro,
        "service_id": svc.service_id,
        "service_name": svc.service_name,
        "ver_major": svc.ver_major,
        "ver_minor": svc.ver_minor,
        "permission_level": svc.permission_level,
        "uuri": uuri(svc.service_id, 0, svc.ver_major, None),
        "qnx_path": f"qmp/{svc.service_id:04x}-0000/v{svc.ver_major}",
        "max_payload": max_payload,
        "max_msg_size": HDR_SIZE + max_payload,
        "methods": methods,
        "sync_methods": [m for m in methods if not m["oneway"]],
        "topics": topics,
        "struct_groups": struct_groups,
        "messages": [m.fqname for m in ordered],
    }


def _reflect(
    fm: FileModel, services: list[dict[str, Any]], table: LayoutTable, universe: Universe
) -> dict[str, Any]:
    msgs: dict[str, Any] = {}
    for svc in services:
        for fq in svc["messages"]:
            msgs[fq.lstrip(".")] = _message_ctx(
                fq, universe.messages[fq], table.structs.get(fq), table, universe
            )
    return {
        "qmp_reflect_version": REFLECT_VERSION,
        "generator": f"protoc-gen-qmp {__version__}",
        "up_spec": UP_SPEC_TAG,
        "source": fm.name,
        "package": fm.package,
        "services": [
            {
                "name": s["name"],
                "full_name": s["fqname"],
                "service_id": s["service_id"],
                "service_name": s["service_name"],
                "version": {"major": s["ver_major"], "minor": s["ver_minor"]},
                "permission_level": s["permission_level"],
                "uuri": s["uuri"],
                "qnx_path": s["qnx_path"],
                "max_msg_size": s["max_msg_size"],
                "methods": [
                    {
                        k: m[k]
                        for k in (
                            "name",
                            "id",
                            "uuri",
                            "oneway",
                            "input",
                            "output",
                            "in_size",
                            "out_size",
                            "allow_types",
                            "permission_level",
                        )
                    }
                    for m in s["methods"]
                ],
                "topics": [
                    {
                        k: t[k]
                        for k in (
                            "name",
                            "id",
                            "uuri",
                            "message",
                            "size",
                            "mode",
                            "depth",
                            "notification",
                        )
                    }
                    for t in s["topics"]
                ],
            }
            for s in services
        ],
        "messages": dict(sorted(msgs.items())),
    }


def _c_byte_array(data: bytes, per_line: int = 16) -> list[str]:
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return lines


def generate_file(universe: Universe, fm: FileModel, flat: bool) -> list[Output]:
    """Render .qmp.h, .qmp.c and .reflect.json for one validated file."""
    table = LayoutTable(universe)
    emitted: set[str] = set()
    services = [_service_ctx(s, universe, table, emitted) for s in fm.services]
    reflect = _reflect(fm, services, table, universe)
    reflect_json = json.dumps(reflect, indent=2, sort_keys=False) + "\n"
    reflect_bytes = reflect_json.encode("utf-8")

    stem = Path(fm.name).stem
    out_dir = "" if flat else str(Path(fm.name).parent)
    base = f"{out_dir}/{stem}" if out_dir and out_dir != "." else stem
    header_name = f"{stem}.qmp.h"
    guard = (
        f"{c_ident('.' + fm.package).upper()}_{stem.upper()}_QMP_H"
        if fm.package
        else f"{stem.upper()}_QMP_H"
    )

    ctx = {
        "version": __version__,
        "source": fm.name,
        "package": fm.package,
        "guard": guard,
        "header_name": header_name,
        "services": services,
        "reflect_lines": _c_byte_array(reflect_bytes),
        "reflect_len": len(reflect_bytes),
        "hdr_size": HDR_SIZE,
    }
    return [
        Output(f"{base}.qmp.h", _ENV.get_template("service.qmp.h.j2").render(**ctx)),
        Output(f"{base}.qmp.c", _ENV.get_template("service.qmp.c.j2").render(**ctx)),
        Output(f"{base}.reflect.json", reflect_json),
    ]

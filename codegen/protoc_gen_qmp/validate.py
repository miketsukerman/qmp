# SPDX-License-Identifier: Apache-2.0
"""v1 subset validation. Every rule produces `file:line: error: message`.

Rules are listed in .github/instructions/codegen.instructions.md; keep both in sync.
"""

from __future__ import annotations

from protoc_gen_qmp.model import (
    MAX_COUNT_MAX,
    MAX_LEN_MAX,
    MAX_NESTING,
    METHOD_ID_MAX,
    SERVICE_ID_MAX,
    TOPIC_ID_MAX,
    TOPIC_ID_MIN,
    FileModel,
    Kind,
    Loc,
    Message,
    Service,
    Universe,
    resolve_type_name,
)


class CodegenError(Exception):
    """Raised with all collected validation errors, one per line."""

    def __init__(self, errors: list[str]) -> None:
        super().__init__("\n".join(errors))
        self.errors = errors


class _Ctx:
    def __init__(self, universe: Universe) -> None:
        self.u = universe
        self.errors: list[str] = []
        self.checked_messages: set[str] = set()

    def err(self, loc: Loc, msg: str) -> None:
        self.errors.append(f"{loc}: error: {msg}")


def _check_message(ctx: _Ctx, fq: str, chain: tuple[str, ...]) -> None:
    """Validate a message and, recursively, the messages it embeds."""
    if fq in chain:
        m = ctx.u.messages[fq]
        ctx.err(
            m.loc,
            f"message '{fq}' is recursive via {' -> '.join(chain)}; "
            "fixed-layout messages cannot embed themselves",
        )
        return
    if fq in ctx.checked_messages:
        return
    ctx.checked_messages.add(fq)
    m = ctx.u.messages[fq]
    if fq.startswith(".google.protobuf."):
        ctx.err(m.loc, f"well-known type '{fq}' is not supported (no Any/Struct/Timestamp in v1)")
        return
    fm = _file_of(ctx.u, fq)
    if fm is not None and fm.syntax != "proto3":
        ctx.err(m.loc, f"message '{fq}' is '{fm.syntax}'; only proto3 is supported")
    if m.map_entry:
        ctx.err(m.loc, "map<> fields are not supported in v1")
        return
    if m.has_oneof:
        ctx.err(m.loc, f"oneof in '{fq}' is not supported in v1")
    if m.depth > MAX_NESTING:
        ctx.err(m.loc, f"message '{fq}' nested deeper than {MAX_NESTING} levels")
    for f in m.fields:
        if f.kind == Kind.MESSAGE:
            target = ctx.u.messages.get(f.type_name)
            if target is None:
                ctx.err(f.loc, f"field '{f.name}': unresolved message type '{f.type_name}'")
                continue
            if target.map_entry:
                ctx.err(f.loc, f"field '{f.name}': map<> fields are not supported in v1")
                continue
            if f.type_name == ".google.protobuf.Any":
                ctx.err(f.loc, f"field '{f.name}': google.protobuf.Any is not supported in v1")
                continue
            if target.is_empty:
                ctx.err(
                    f.loc,
                    f"field '{f.name}': zero-field message '{f.type_name}' cannot be "
                    "a field type (no zero-sized struct is emitted)",
                )
            _check_message(ctx, f.type_name, (*chain, fq))
        elif f.kind == Kind.ENUM and f.type_name not in ctx.u.enums:
            ctx.err(f.loc, f"field '{f.name}': unresolved enum type '{f.type_name}'")

        if f.kind in (Kind.STRING, Kind.BYTES):
            if f.max_len is None:
                ctx.err(f.loc, f"field '{f.name}': {f.kind.value} requires (qmp.max_len)")
            elif not 1 <= f.max_len <= MAX_LEN_MAX:
                ctx.err(f.loc, f"field '{f.name}': (qmp.max_len) must be in 1..{MAX_LEN_MAX}")
            if (
                f.up_field_size is not None
                and f.max_len is not None
                and f.up_field_size != f.max_len
            ):
                # VERIFY(up): V-UP-010 — uprotocol.field_size counts NUL/BOM; qmp.max_len does not.
                ctx.err(
                    f.loc,
                    f"field '{f.name}': (uprotocol.field_size)={f.up_field_size} "
                    f"disagrees with (qmp.max_len)={f.max_len}; set one or make them equal",
                )
        elif f.max_len is not None:
            ctx.err(f.loc, f"field '{f.name}': (qmp.max_len) is only valid on string/bytes")

        if f.repeated:
            if f.max_count is None:
                ctx.err(f.loc, f"field '{f.name}': repeated requires (qmp.max_count)")
            elif not 1 <= f.max_count <= MAX_COUNT_MAX:
                ctx.err(f.loc, f"field '{f.name}': (qmp.max_count) must be in 1..{MAX_COUNT_MAX}")
            if (
                f.up_field_count is not None
                and f.max_count is not None
                and f.up_field_count != f.max_count
            ):
                ctx.err(
                    f.loc,
                    f"field '{f.name}': (uprotocol.field_count)={f.up_field_count} "
                    f"disagrees with (qmp.max_count)={f.max_count}",
                )
        elif f.max_count is not None:
            ctx.err(f.loc, f"field '{f.name}': (qmp.max_count) is only valid on repeated fields")


def _file_of(u: Universe, fq: str) -> FileModel | None:
    for fm in u.files.values():
        if fq in fm.messages:
            return fm
    return None


def _check_service(ctx: _Ctx, svc: Service) -> None:
    if svc.service_id is None:
        ctx.err(svc.loc, f"service '{svc.name}': missing (uprotocol.service_id)")
    elif not 0 <= svc.service_id <= SERVICE_ID_MAX:
        ctx.err(svc.loc, f"service '{svc.name}': (uprotocol.service_id) must be 16-bit")
    if svc.ver_major is None:
        ctx.err(svc.loc, f"service '{svc.name}': missing (uprotocol.service_version_major)")
    elif not 1 <= svc.ver_major <= 0xFE:
        # 0xFF is the uProtocol wildcard major version (basics/uri.adoc).
        ctx.err(svc.loc, f"service '{svc.name}': (uprotocol.service_version_major) must be 1..254")
    if not 0 <= svc.ver_minor <= 0xFF:
        ctx.err(svc.loc, f"service '{svc.name}': (uprotocol.service_version_minor) must be 0..255")

    seen_ids: dict[int, str] = {}
    for m in svc.methods:
        if m.client_streaming or m.server_streaming:
            ctx.err(m.loc, f"method '{m.name}': streaming rpcs are not supported")
        if m.method_id is None:
            ctx.err(m.loc, f"method '{m.name}': missing (uprotocol.method_id)")
        elif not 0 <= m.method_id <= METHOD_ID_MAX:
            ctx.err(m.loc, f"method '{m.name}': (uprotocol.method_id) must be 0x0000..0x7FFF")
        elif m.method_id in seen_ids:
            ctx.err(
                m.loc,
                f"method '{m.name}': duplicate id 0x{m.method_id:04X} "
                f"(also used by '{seen_ids[m.method_id]}')",
            )
        else:
            seen_ids[m.method_id] = m.name
        for role, tn in (("input", m.input_type), ("output", m.output_type)):
            if tn not in ctx.u.messages:
                ctx.err(m.loc, f"method '{m.name}': unresolved {role} type '{tn}'")
            else:
                _check_message(ctx, tn, ())
        out = ctx.u.messages.get(m.output_type)
        if m.oneway and out is not None and not out.is_empty:
            ctx.err(
                m.loc,
                f"method '{m.name}': (qmp.oneway) requires a zero-field reply "
                f"message, got '{m.output_type}'",
            )

    seen_topics: dict[int, str] = {}
    for t in svc.topics:
        if not TOPIC_ID_MIN <= t.topic_id <= TOPIC_ID_MAX:
            ctx.err(t.loc, f"topic '{t.name}': id must be 0x8000..0xFFFF")
        elif t.topic_id in seen_topics:
            ctx.err(
                t.loc,
                f"topic '{t.name}': duplicate id 0x{t.topic_id:04X} "
                f"(also used by '{seen_topics[t.topic_id]}')",
            )
        else:
            seen_topics[t.topic_id] = t.name
        if not t.name:
            ctx.err(t.loc, f"topic 0x{t.topic_id:04X}: name is required")
        if t.resources:
            # VERIFY(up): V-UP-008 — resource-enumerated topics are rejected in v1.
            ctx.err(t.loc, f"topic '{t.name}': 'resources' are not supported in v1")
        resolved = resolve_type_name(t.message, svc.fqname, ctx.u) if t.message else None
        if resolved is None:
            ctx.err(t.loc, f"topic '{t.name}': cannot resolve message '{t.message}'")
        else:
            t.resolved_message = resolved
            if ctx.u.messages[resolved].is_empty:
                ctx.err(t.loc, f"topic '{t.name}': message '{resolved}' has no fields")
            _check_message(ctx, resolved, ())

    for tm in svc.topic_modes:
        topic = next((t for t in svc.topics if t.topic_id == tm.topic_id), None)
        if topic is None:
            ctx.err(svc.loc, f"(qmp.topic_mode) references undeclared topic 0x{tm.topic_id:04X}")
            continue
        if tm.mode == "QUEUE" and not 1 <= tm.depth <= 0xFFFF:
            ctx.err(svc.loc, f"(qmp.topic_mode) for '{topic.name}': QUEUE depth must be 1..65535")
        topic.mode = tm.mode
        topic.depth = tm.depth if tm.mode == "QUEUE" else 0

    if not svc.service_name:
        svc.service_name = svc.fqname.lstrip(".")


def validate_file(universe: Universe, fm: FileModel) -> None:
    """Validate one file to generate; raises CodegenError with all problems found."""
    ctx = _Ctx(universe)
    if fm.syntax != "proto3":
        ctx.err(Loc(fm.name, 1), f"file is '{fm.syntax}'; only proto3 is supported")
    if not fm.services:
        ctx.err(Loc(fm.name, 1), "file declares no service; nothing to generate")
    for svc in fm.services:
        _check_service(ctx, svc)
    # Also validate top-level messages of the file not reached from a service so that a
    # malformed message is reported even when unused.
    for fq in fm.messages:
        _check_message(ctx, fq, ())
    if ctx.errors:
        raise CodegenError(sorted(set(ctx.errors), key=_sort_key))


def _sort_key(e: str) -> tuple[str, int, str]:
    file, _, rest = e.partition(":")
    line, _, msg = rest.partition(":")
    try:
        return (file, int(line), msg)
    except ValueError:
        return (file, 0, rest)


def reachable_messages(universe: Universe, svc: Service) -> list[Message]:
    """Messages used by a service (methods and topics), dependencies first, deterministic."""
    order: list[Message] = []
    seen: set[str] = set()

    def visit(fq: str) -> None:
        if fq in seen:
            return
        seen.add(fq)
        m = universe.messages[fq]
        for f in m.fields:
            if f.kind == Kind.MESSAGE:
                visit(f.type_name)
        order.append(m)

    for m in svc.methods:
        visit(m.input_type)
        visit(m.output_type)
    for t in svc.topics:
        if t.resolved_message:
            visit(t.resolved_message)
    return order

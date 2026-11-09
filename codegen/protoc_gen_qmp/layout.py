# SPDX-License-Identifier: Apache-2.0
"""Pure C layout computation (ADR-002).

Rules: fields in field-number order; natural alignment of the C scalar types; explicit
`_padN` members wherever the compiler would insert padding; message structs are padded to a
multiple of 8 bytes; helper structs (string/bytes/repeated) are padded to their own alignment.
The emitted `_Static_assert`s make the C compiler the oracle for everything computed here.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from protoc_gen_qmp.model import Field, Kind, Universe, c_ident

HDR_SIZE = 24
MSG_ALIGN = 8

_SCALAR_C = {
    Kind.BOOL: ("uint8_t", 1),
    Kind.INT32: ("int32_t", 4),
    Kind.UINT32: ("uint32_t", 4),
    Kind.INT64: ("int64_t", 8),
    Kind.UINT64: ("uint64_t", 8),
    Kind.FLOAT: ("float", 4),
    Kind.DOUBLE: ("double", 8),
    Kind.ENUM: ("int32_t", 4),
}


@dataclass
class Member:
    name: str
    c_type: str  # full C declarator type (e.g. "uint32_t", "demo_Foo", "demo_Foo_bar_t")
    offset: int
    size: int
    align: int
    kind: str  # scalar | pad | string | bytes | array | message
    comment: str = ""
    array_len: int = 0  # for pads: byte count

    @property
    def decl(self) -> str:
        if self.kind == "pad":
            return f"uint8_t {self.name}[{self.array_len}]"
        return f"{self.c_type} {self.name}"


@dataclass
class Struct:
    c_name: str
    members: list[Member]
    size: int
    align: int
    kind: str  # message | string | bytes | array
    # bounds for helpers
    max_n: int = 0
    elem_c_type: str = ""
    elem_kind: str = ""  # scalar | string | bytes | message
    elem_fq: str = ""  # fq message name of the element when elem_kind == message
    # validation hooks for messages: (member, check kind, helper struct)
    checks: list[tuple[Member, Struct]] = field(default_factory=list)

    @property
    def asserts(self) -> list[tuple[str, int]]:
        """(member name, expected offset) for every non-pad member."""
        return [(m.name, m.offset) for m in self.members if m.kind != "pad"]


def _align_up(x: int, a: int) -> int:
    return (x + a - 1) // a * a


def _pack(members: list[tuple[str, str, int, int, str, str]], tail_align: int) -> Struct:
    """Place (name, c_type, size, align, kind, comment) tuples, inserting explicit pads."""
    out: list[Member] = []
    off = 0
    npad = 0
    max_align = 1
    for name, c_type, size, align, kind, comment in members:
        max_align = max(max_align, align)
        aligned = _align_up(off, align)
        if aligned != off:
            out.append(
                Member(
                    f"_pad{npad}", "uint8_t", off, aligned - off, 1, "pad", array_len=aligned - off
                )
            )
            npad += 1
            off = aligned
        out.append(Member(name, c_type, off, size, align, kind, comment))
        off += size
    total = _align_up(off, max(tail_align, max_align))
    if total == 0:
        raise ValueError("zero-sized struct")
    if total != off:
        out.append(
            Member(f"_pad{npad}", "uint8_t", off, total - off, 1, "pad", array_len=total - off)
        )
    return Struct(c_name="", members=out, size=total, align=max_align, kind="")


class LayoutTable:
    """Computes and caches layouts for all messages of a Universe."""

    def __init__(self, universe: Universe) -> None:
        self.u = universe
        self.structs: dict[str, Struct] = {}  # fq message name -> Struct
        self.helpers: list[Struct] = []  # in definition order (dependencies first)

    def message(self, fq: str) -> Struct | None:
        """Layout of a message; None for zero-field messages (no struct emitted)."""
        if fq in self.structs:
            return self.structs[fq]
        m = self.u.messages[fq]
        if m.is_empty:
            return None
        cname = c_ident(fq)
        rows: list[tuple[str, str, int, int, str, str]] = []
        checks: list[tuple[str, Struct]] = []
        for f in sorted(m.fields, key=lambda x: x.number):
            c_type, size, align, kind, helper = self._field_type(cname, f)
            rows.append((f.name, c_type, size, align, kind, f"field {f.number}"))
            if helper is not None:
                checks.append((f.name, helper))
            elif kind == "message":
                sub = self.structs[f.type_name]
                if sub.checks:
                    checks.append((f.name, sub))
        st = _pack(rows, MSG_ALIGN)
        st.c_name = cname
        st.kind = "message"
        by_name = {mm.name: mm for mm in st.members}
        st.checks = [(by_name[n], h) for n, h in checks]
        self.structs[fq] = st
        return st

    def _scalar(self, f: Field) -> tuple[str, int, int]:
        c_type, size = _SCALAR_C[f.kind]
        return c_type, size, size

    def _elem(self, owner: str, f: Field) -> tuple[str, int, int, str, Struct | None, str]:
        """Element type of a (possibly repeated) field: (c_type, size, align, kind, helper, fq)."""
        if f.kind in (Kind.STRING, Kind.BYTES):
            assert f.max_len is not None
            suffix = "_item_t" if f.repeated else "_t"
            h = self._string_helper(f"{owner}_{f.name}{suffix}", f.kind, f.max_len)
            return h.c_name, h.size, h.align, h.kind, h, ""
        if f.kind == Kind.MESSAGE:
            sub = self.message(f.type_name)
            assert sub is not None  # validated: no empty message as field type
            return sub.c_name, sub.size, sub.align, "message", None, f.type_name
        c_type, size, align = self._scalar(f)
        return c_type, size, align, "scalar", None, ""

    def _field_type(self, owner: str, f: Field) -> tuple[str, int, int, str, Struct | None]:
        e_type, e_size, e_align, e_kind, e_helper, e_fq = self._elem(owner, f)
        if not f.repeated:
            return e_type, e_size, e_align, e_kind, e_helper
        assert f.max_count is not None
        arr = self._array_helper(
            f"{owner}_{f.name}_t", e_type, e_size, e_align, e_kind, f.max_count, e_fq
        )
        if e_helper is not None:
            arr.checks = [(arr.members[2], e_helper)]
        elif e_kind == "message" and self.structs[e_fq].checks:
            arr.checks = [(arr.members[2], self.structs[e_fq])]
        return arr.c_name, arr.size, arr.align, "array", arr

    def _string_helper(self, c_name: str, kind: Kind, n: int) -> Struct:
        data_t = "char" if kind == Kind.STRING else "uint8_t"
        st = _pack(
            [
                ("len", "uint16_t", 2, 2, "scalar", "bytes used"),
                ("data", f"{data_t}", n, 1, "scalar", f"max_len = {n}"),
            ],
            2,
        )
        st.members[1].c_type = f"{data_t}"
        st.members[1].kind = "data"
        st.members[1].array_len = n
        st.c_name = c_name
        st.kind = kind.value
        st.max_n = n
        st.elem_c_type = data_t
        self.helpers.append(st)
        return st

    def _array_helper(  # noqa: PLR0917
        self, c_name: str, e_type: str, e_size: int, e_align: int, e_kind: str, n: int, e_fq: str
    ) -> Struct:
        st = _pack(
            [
                ("count", "uint32_t", 4, 4, "scalar", "items used"),
                ("_pad", "uint32_t", 4, 4, "scalar", "sender zeroes"),
                ("items", e_type, e_size * n, e_align, "items", f"max_count = {n}"),
            ],
            max(4, e_align),
        )
        st.members[2].array_len = n
        st.c_name = c_name
        st.kind = "array"
        st.max_n = n
        st.elem_c_type = e_type
        st.elem_kind = e_kind
        st.elem_fq = e_fq
        self.helpers.append(st)
        return st


def decl_of(m: Member) -> str:
    """C declaration of a member, including array suffixes for data/items/pads."""
    if m.kind == "pad":
        return f"uint8_t {m.name}[{m.array_len}];"
    if m.kind in ("data", "items"):
        return f"{m.c_type} {m.name}[{m.array_len}];"
    return f"{m.c_type} {m.name};"

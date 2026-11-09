# SPDX-License-Identifier: Apache-2.0
"""Intermediate model built from FileDescriptorProtos.

The model is deliberately independent of the descriptor pool: types are resolved by
fully-qualified name over all files in the CodeGeneratorRequest so no pool conflicts can
arise with the generated option modules.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import Any

from google.protobuf import descriptor_pb2 as dpb

from protoc_gen_qmp._pb.qmp import options_pb2 as qmp_opts
from protoc_gen_qmp._pb.uprotocol import uoptions_pb2 as up_opts

FDP = dpb.FieldDescriptorProto

# Method ids are < 0x8000, topic ids >= 0x8000 (up-spec basics/uri.adoc, ADR-005).
METHOD_ID_MAX = 0x7FFF
TOPIC_ID_MIN = 0x8000
TOPIC_ID_MAX = 0xFFFF
SERVICE_ID_MAX = 0xFFFF
MAX_LEN_MAX = 0xFFFF
MAX_COUNT_MAX = 0x7FFFFFFF
MAX_NESTING = 4


class Kind(enum.Enum):
    """Scalar kinds supported in the v1 subset plus nested messages."""

    BOOL = "bool"
    INT32 = "int32"
    UINT32 = "uint32"
    INT64 = "int64"
    UINT64 = "uint64"
    FLOAT = "float"
    DOUBLE = "double"
    ENUM = "enum"
    STRING = "string"
    BYTES = "bytes"
    MESSAGE = "message"


_SCALAR_KIND = {
    FDP.TYPE_BOOL: Kind.BOOL,
    FDP.TYPE_INT32: Kind.INT32,
    FDP.TYPE_SINT32: Kind.INT32,
    FDP.TYPE_SFIXED32: Kind.INT32,
    FDP.TYPE_UINT32: Kind.UINT32,
    FDP.TYPE_FIXED32: Kind.UINT32,
    FDP.TYPE_INT64: Kind.INT64,
    FDP.TYPE_SINT64: Kind.INT64,
    FDP.TYPE_SFIXED64: Kind.INT64,
    FDP.TYPE_UINT64: Kind.UINT64,
    FDP.TYPE_FIXED64: Kind.UINT64,
    FDP.TYPE_FLOAT: Kind.FLOAT,
    FDP.TYPE_DOUBLE: Kind.DOUBLE,
    FDP.TYPE_ENUM: Kind.ENUM,
    FDP.TYPE_STRING: Kind.STRING,
    FDP.TYPE_BYTES: Kind.BYTES,
    FDP.TYPE_MESSAGE: Kind.MESSAGE,
}

# Tags in FileDescriptorProto / DescriptorProto used for SourceCodeInfo paths.
_P_MESSAGE_TYPE = 4
_P_ENUM_TYPE = 5
_P_SERVICE = 6
_P_MSG_FIELD = 2
_P_MSG_NESTED = 3
_P_MSG_ENUM = 4
_P_MSG_ONEOF = 8
_P_SVC_METHOD = 2
_P_SVC_OPTIONS = 3


@dataclass
class Loc:
    file: str
    line: int  # 1-based; 0 when unknown

    def __str__(self) -> str:
        return f"{self.file}:{self.line}" if self.line else self.file


@dataclass
class EnumDef:
    fqname: str  # ".pkg.Msg.Enum"
    values: dict[str, int]
    loc: Loc


@dataclass
class Field:
    name: str
    number: int
    kind: Kind
    repeated: bool
    type_name: str  # fq name for ENUM/MESSAGE, "" otherwise
    max_len: int | None
    max_count: int | None
    up_field_size: int | None
    up_field_count: int | None
    in_oneof: bool
    loc: Loc


@dataclass
class Message:
    fqname: str  # ".pkg.Outer.Inner"
    fields: list[Field]
    map_entry: bool
    has_oneof: bool
    depth: int
    loc: Loc

    @property
    def is_empty(self) -> bool:
        return not self.fields


@dataclass
class Method:
    name: str
    method_id: int | None
    input_type: str
    output_type: str
    oneway: bool
    allow_types: list[str]
    permission_level: int | None
    client_streaming: bool
    server_streaming: bool
    loc: Loc


@dataclass
class Topic:
    topic_id: int
    name: str
    message: str  # as written in the option
    resources: list[str]
    permission_level: int | None
    notification: bool
    loc: Loc
    resolved_message: str = ""  # fq name after validation
    mode: str = "LATEST"
    depth: int = 0


@dataclass
class TopicModeOpt:
    topic_id: int
    mode: str
    depth: int


@dataclass
class Service:
    name: str
    package: str
    service_id: int | None
    service_name: str
    ver_major: int | None
    ver_minor: int
    permission_level: int | None
    methods: list[Method]
    topics: list[Topic]
    topic_modes: list[TopicModeOpt]
    loc: Loc

    @property
    def fqname(self) -> str:
        return f".{self.package}.{self.name}" if self.package else f".{self.name}"


@dataclass
class FileModel:
    name: str
    package: str
    syntax: str
    dependencies: list[str]
    messages: dict[str, Message] = field(default_factory=dict)
    enums: dict[str, EnumDef] = field(default_factory=dict)
    services: list[Service] = field(default_factory=list)


@dataclass
class Universe:
    """All files from the request, indexed for type resolution."""

    files: dict[str, FileModel] = field(default_factory=dict)
    messages: dict[str, Message] = field(default_factory=dict)
    enums: dict[str, EnumDef] = field(default_factory=dict)

    def add(self, fm: FileModel) -> None:
        self.files[fm.name] = fm
        self.messages.update(fm.messages)
        self.enums.update(fm.enums)


class _Lines:
    """Maps SourceCodeInfo paths to 1-based lines."""

    def __init__(self, fdp: dpb.FileDescriptorProto) -> None:
        self._m: dict[tuple[int, ...], int] = {}
        for loc in fdp.source_code_info.location:
            if loc.span:
                self._m.setdefault(tuple(loc.path), loc.span[0] + 1)

    def get(self, *path: int) -> int:
        return self._m.get(tuple(path), 0)


def _opt_int(options: Any, ext: Any) -> int | None:
    if not options.HasExtension(ext):
        return None
    return int(options.Extensions[ext])


def _opt_bool(options: Any, ext: Any) -> bool:
    return bool(options.HasExtension(ext) and options.Extensions[ext])


def _opt_rep(options: Any, ext: Any) -> list[Any]:
    """Repeated extension value as a plain list (typed loosely: upb stubs lack extension types)."""
    return list(options.Extensions[ext])


def _opt_str(options: Any, ext: Any) -> str:
    return str(options.Extensions[ext])


def build_file(fdp: dpb.FileDescriptorProto) -> FileModel:
    """Build the model for one file (messages, enums, services)."""
    lines = _Lines(fdp)
    pkg_prefix = f".{fdp.package}" if fdp.package else ""
    fm = FileModel(
        name=fdp.name,
        package=fdp.package,
        syntax=fdp.syntax or "proto2",
        dependencies=list(fdp.dependency),
    )

    def add_enum(edp: dpb.EnumDescriptorProto, scope: str, path: tuple[int, ...]) -> None:
        fq = f"{scope}.{edp.name}"
        fm.enums[fq] = EnumDef(
            fqname=fq,
            values={v.name: v.number for v in edp.value},
            loc=Loc(fdp.name, lines.get(*path)),
        )

    def add_message(
        mdp: dpb.DescriptorProto, scope: str, path: tuple[int, ...], depth: int
    ) -> None:
        fq = f"{scope}.{mdp.name}"
        fields: list[Field] = []
        for i, f in enumerate(mdp.field):
            fields.append(
                Field(
                    name=f.name,
                    number=f.number,
                    kind=_SCALAR_KIND.get(f.type, Kind.MESSAGE),
                    repeated=f.label == FDP.LABEL_REPEATED,
                    type_name=f.type_name,
                    max_len=_opt_int(f.options, qmp_opts.max_len),
                    max_count=_opt_int(f.options, qmp_opts.max_count),
                    up_field_size=_opt_int(f.options, up_opts.field_size),
                    up_field_count=_opt_int(f.options, up_opts.field_count),
                    in_oneof=f.HasField("oneof_index") and not f.proto3_optional,
                    loc=Loc(fdp.name, lines.get(*path, _P_MSG_FIELD, i)),
                )
            )
        real_oneofs = [o for o in mdp.oneof_decl if not o.name.startswith("_")]
        fm.messages[fq] = Message(
            fqname=fq,
            fields=fields,
            map_entry=mdp.options.map_entry,
            has_oneof=bool(real_oneofs),
            depth=depth,
            loc=Loc(fdp.name, lines.get(*path)),
        )
        for i, nested in enumerate(mdp.nested_type):
            add_message(nested, fq, (*path, _P_MSG_NESTED, i), depth + 1)
        for i, e in enumerate(mdp.enum_type):
            add_enum(e, fq, (*path, _P_MSG_ENUM, i))

    for i, m in enumerate(fdp.message_type):
        add_message(m, pkg_prefix, (_P_MESSAGE_TYPE, i), 1)
    for i, e in enumerate(fdp.enum_type):
        add_enum(e, pkg_prefix, (_P_ENUM_TYPE, i))

    for si, sdp in enumerate(fdp.service):
        svc_opts = sdp.options
        methods: list[Method] = []
        for mi, mdp in enumerate(sdp.method):
            mo = mdp.options
            allow = [str(a) for a in _opt_rep(mo, qmp_opts.allow_types)]
            methods.append(
                Method(
                    name=mdp.name,
                    method_id=_opt_int(mo, up_opts.method_id),
                    input_type=mdp.input_type,
                    output_type=mdp.output_type,
                    oneway=_opt_bool(mo, qmp_opts.oneway),
                    allow_types=allow,
                    permission_level=_opt_int(mo, up_opts.method_permission_level),
                    client_streaming=mdp.client_streaming,
                    server_streaming=mdp.server_streaming,
                    loc=Loc(fdp.name, lines.get(_P_SERVICE, si, _P_SVC_METHOD, mi)),
                )
            )
        topics: list[Topic] = []
        svc_loc = Loc(fdp.name, lines.get(_P_SERVICE, si))
        opt_loc = Loc(fdp.name, lines.get(_P_SERVICE, si, _P_SVC_OPTIONS) or svc_loc.line)
        for notif, ext in ((False, up_opts.publish_topic), (True, up_opts.notification_topic)):
            for t in _opt_rep(svc_opts, ext):
                topics.append(
                    Topic(
                        topic_id=t.id,
                        name=t.name,
                        message=t.message,
                        resources=list(t.resources),
                        permission_level=t.permission_level
                        if t.HasField("permission_level")
                        else None,
                        notification=notif,
                        loc=opt_loc,
                    )
                )
        modes = [
            TopicModeOpt(
                topic_id=tm.topic_id,
                mode=qmp_opts.Mode.Name(tm.mode).removeprefix("MODE_"),
                depth=tm.depth,
            )
            for tm in _opt_rep(svc_opts, qmp_opts.topic_mode)
        ]
        fm.services.append(
            Service(
                name=sdp.name,
                package=fdp.package,
                service_id=_opt_int(svc_opts, up_opts.service_id),
                service_name=_opt_str(svc_opts, up_opts.service_name),
                ver_major=_opt_int(svc_opts, up_opts.service_version_major),
                ver_minor=_opt_int(svc_opts, up_opts.service_version_minor) or 0,
                permission_level=_opt_int(svc_opts, up_opts.service_permission_level),
                methods=methods,
                topics=topics,
                topic_modes=modes,
                loc=svc_loc,
            )
        )
    return fm


def build_universe(files: list[dpb.FileDescriptorProto]) -> Universe:
    u = Universe()
    for fdp in files:
        u.add(build_file(fdp))
    return u


def c_ident(fqname: str) -> str:
    """'.demo.joint.v1.JointState' -> 'demo_joint_v1_JointState'."""
    return fqname.lstrip(".").replace(".", "_")


def resolve_type_name(name: str, scope: str, universe: Universe) -> str | None:
    """Protobuf name resolution: absolute with leading '.', else innermost scope outward."""
    if name.startswith("."):
        return name if name in universe.messages else None
    parts = scope.lstrip(".").split(".") if scope.lstrip(".") else []
    while True:
        cand = "." + ".".join([*parts, name]) if parts else f".{name}"
        if cand in universe.messages:
            return cand
        if not parts:
            return None
        parts.pop()

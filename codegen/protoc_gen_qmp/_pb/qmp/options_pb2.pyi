from google.protobuf import descriptor_pb2 as _descriptor_pb2
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class Mode(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    MODE_LATEST: _ClassVar[Mode]
    MODE_QUEUE: _ClassVar[Mode]
MODE_LATEST: Mode
MODE_QUEUE: Mode
MAX_LEN_FIELD_NUMBER: _ClassVar[int]
max_len: _descriptor.FieldDescriptor
MAX_COUNT_FIELD_NUMBER: _ClassVar[int]
max_count: _descriptor.FieldDescriptor
ONEWAY_FIELD_NUMBER: _ClassVar[int]
oneway: _descriptor.FieldDescriptor
ALLOW_TYPES_FIELD_NUMBER: _ClassVar[int]
allow_types: _descriptor.FieldDescriptor
TOPIC_MODE_FIELD_NUMBER: _ClassVar[int]
topic_mode: _descriptor.FieldDescriptor

class TopicMode(_message.Message):
    __slots__ = ("topic_id", "mode", "depth")
    TOPIC_ID_FIELD_NUMBER: _ClassVar[int]
    MODE_FIELD_NUMBER: _ClassVar[int]
    DEPTH_FIELD_NUMBER: _ClassVar[int]
    topic_id: int
    mode: Mode
    depth: int
    def __init__(self, topic_id: _Optional[int] = ..., mode: _Optional[_Union[Mode, str]] = ..., depth: _Optional[int] = ...) -> None: ...

from google.protobuf import descriptor_pb2 as _descriptor_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Iterable as _Iterable, Optional as _Optional

DESCRIPTOR: _descriptor.FileDescriptor
FILE_SOMEIP_FIELD_NUMBER: _ClassVar[int]
file_someip: _descriptor.FieldDescriptor
SERVICE_ID_FIELD_NUMBER: _ClassVar[int]
service_id: _descriptor.FieldDescriptor
SERVICE_NAME_FIELD_NUMBER: _ClassVar[int]
service_name: _descriptor.FieldDescriptor
SERVICE_VERSION_MAJOR_FIELD_NUMBER: _ClassVar[int]
service_version_major: _descriptor.FieldDescriptor
SERVICE_VERSION_MINOR_FIELD_NUMBER: _ClassVar[int]
service_version_minor: _descriptor.FieldDescriptor
SERVICE_PERMISSION_LEVEL_FIELD_NUMBER: _ClassVar[int]
service_permission_level: _descriptor.FieldDescriptor
PUBLISH_TOPIC_FIELD_NUMBER: _ClassVar[int]
publish_topic: _descriptor.FieldDescriptor
NOTIFICATION_TOPIC_FIELD_NUMBER: _ClassVar[int]
notification_topic: _descriptor.FieldDescriptor
SERVICE_SOMEIP_FIELD_NUMBER: _ClassVar[int]
service_someip: _descriptor.FieldDescriptor
METHOD_ID_FIELD_NUMBER: _ClassVar[int]
method_id: _descriptor.FieldDescriptor
METHOD_PERMISSION_LEVEL_FIELD_NUMBER: _ClassVar[int]
method_permission_level: _descriptor.FieldDescriptor
MESSAGE_SOMEIP_FIELD_NUMBER: _ClassVar[int]
message_someip: _descriptor.FieldDescriptor
MESSAGE_SIZE_FIELD_NUMBER: _ClassVar[int]
message_size: _descriptor.FieldDescriptor
FIELD_SIZE_FIELD_NUMBER: _ClassVar[int]
field_size: _descriptor.FieldDescriptor
FIELD_KEY_SIZE_FIELD_NUMBER: _ClassVar[int]
field_key_size: _descriptor.FieldDescriptor
FIELD_COUNT_FIELD_NUMBER: _ClassVar[int]
field_count: _descriptor.FieldDescriptor
ENUM_SIZE_FIELD_NUMBER: _ClassVar[int]
enum_size: _descriptor.FieldDescriptor
ENUM_SOMEIP_FIELD_NUMBER: _ClassVar[int]
enum_someip: _descriptor.FieldDescriptor
MIME_TYPE_FIELD_NUMBER: _ClassVar[int]
mime_type: _descriptor.FieldDescriptor
CE_NAME_FIELD_NUMBER: _ClassVar[int]
ce_name: _descriptor.FieldDescriptor

class UServiceTopic(_message.Message):
    __slots__ = ("id", "name", "message", "resources", "permission_level")
    ID_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    PERMISSION_LEVEL_FIELD_NUMBER: _ClassVar[int]
    id: int
    name: str
    message: str
    resources: _containers.RepeatedScalarFieldContainer[str]
    permission_level: int
    def __init__(self, id: _Optional[int] = ..., name: _Optional[str] = ..., message: _Optional[str] = ..., resources: _Optional[_Iterable[str]] = ..., permission_level: _Optional[int] = ...) -> None: ...

"""PHYLAX protobuf messages for trezorlib.

Mirrors common/protob/messages-phylax.proto. trezorlib's generated
messages.py does not include them, so they are defined and registered here.
Import this module once, then use e.g.:

    blocks = session.call(PhylaxGetBlocks(), expect=PhylaxBlocks).blocks
"""

from __future__ import annotations

from enum import IntEnum
from typing import Optional, Sequence

from trezorlib import mapping, protobuf


class PhylaxBlockKind(IntEnum):
    PHYLAX_BLOCK_MAIN = 0
    PHYLAX_BLOCK_KEY = 1
    PHYLAX_BLOCK_STORAGE = 2


class PhylaxBlockInfo(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = None
    FIELDS = {
        1: protobuf.Field("address", "uint32", repeated=False, required=True),
        2: protobuf.Field("kind", "PhylaxBlockKind", repeated=False, required=True),
        3: protobuf.Field("unique_id", "bytes", repeated=False, required=True),
        4: protobuf.Field("fingerprint", "uint32", repeated=False, required=False, default=None),
        5: protobuf.Field("initialized", "bool", repeated=False, required=False, default=None),
        6: protobuf.Field("locked", "bool", repeated=False, required=False, default=None),
        7: protobuf.Field("major_version", "uint32", repeated=False, required=False, default=None),
        8: protobuf.Field("minor_version", "uint32", repeated=False, required=False, default=None),
        9: protobuf.Field("patch_version", "uint32", repeated=False, required=False, default=None),
    }

    def __init__(
        self,
        *,
        address: int,
        kind: PhylaxBlockKind,
        unique_id: bytes,
        fingerprint: Optional[int] = None,
        initialized: Optional[bool] = None,
        locked: Optional[bool] = None,
        major_version: Optional[int] = None,
        minor_version: Optional[int] = None,
        patch_version: Optional[int] = None,
    ) -> None:
        self.address = address
        self.kind = kind
        self.unique_id = unique_id
        self.fingerprint = fingerprint
        self.initialized = initialized
        self.locked = locked
        self.major_version = major_version
        self.minor_version = minor_version
        self.patch_version = patch_version

    def describe(self) -> str:
        kind = {0: "main", 1: "key", 2: "storage"}.get(int(self.kind), str(self.kind))
        fp = f"{self.fingerprint:08x}" if self.fingerprint is not None else (
            "locked" if self.locked else "no seed" if not self.initialized else "?")
        return f"0x{self.address:02x} {kind:<7} fingerprint {fp}  id {self.unique_id.hex()}"


class PhylaxGetBlocks(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7700


class PhylaxBlocks(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7701
    FIELDS = {
        1: protobuf.Field("blocks", "PhylaxBlockInfo", repeated=True, required=False, default=None),
    }

    def __init__(self, *, blocks: Optional[Sequence[PhylaxBlockInfo]] = None) -> None:
        self.blocks: Sequence[PhylaxBlockInfo] = blocks if blocks is not None else []


for _cls in (PhylaxGetBlocks, PhylaxBlocks):
    mapping.DEFAULT_MAPPING.register(_cls)

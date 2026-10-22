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


# --- saved wallets (storage block) -------------------------------------

from trezorlib.messages import HDNodeType  # noqa: E402  (used by field types)


class PhylaxWalletSummary(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = None
    FIELDS = {
        1: protobuf.Field("slot", "uint32", repeated=False, required=True),
        2: protobuf.Field("name", "string", repeated=False, required=True),
        3: protobuf.Field("threshold", "uint32", repeated=False, required=True),
        4: protobuf.Field("fingerprints", "uint32", repeated=True, required=False, default=None),
        5: protobuf.Field("complete", "bool", repeated=False, required=False, default=None),
    }

    def __init__(self, *, slot: int, name: str, threshold: int,
                 fingerprints: Optional[Sequence[int]] = None,
                 complete: Optional[bool] = None) -> None:
        self.slot = slot
        self.name = name
        self.threshold = threshold
        self.fingerprints: Sequence[int] = fingerprints if fingerprints is not None else []
        self.complete = complete

    def describe(self) -> str:
        fps = " ".join(f"{f:08x}" for f in self.fingerprints)
        state = "ready" if self.complete else "block missing"
        return f"slot {self.slot}: {self.name:<10} {self.threshold}-of-{len(self.fingerprints)}  {fps}  ({state})"


class PhylaxGetWallets(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7702


class PhylaxWallets(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7703
    FIELDS = {
        1: protobuf.Field("wallets", "PhylaxWalletSummary", repeated=True, required=False, default=None),
        2: protobuf.Field("storage_present", "bool", repeated=False, required=True),
    }

    def __init__(self, *, storage_present: bool,
                 wallets: Optional[Sequence[PhylaxWalletSummary]] = None) -> None:
        self.wallets: Sequence[PhylaxWalletSummary] = wallets if wallets is not None else []
        self.storage_present = storage_present


class PhylaxWalletKey(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = None
    FIELDS = {
        1: protobuf.Field("fingerprint", "uint32", repeated=False, required=True),
        2: protobuf.Field("mainnet", "HDNodeType", repeated=False, required=True),
        3: protobuf.Field("testnet", "HDNodeType", repeated=False, required=True),
    }

    def __init__(self, *, fingerprint: int, mainnet: HDNodeType, testnet: HDNodeType) -> None:
        self.fingerprint = fingerprint
        self.mainnet = mainnet
        self.testnet = testnet


class PhylaxGetWallet(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7704
    FIELDS = {1: protobuf.Field("slot", "uint32", repeated=False, required=True)}

    def __init__(self, *, slot: int) -> None:
        self.slot = slot


class PhylaxWallet(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7705
    FIELDS = {
        1: protobuf.Field("slot", "uint32", repeated=False, required=True),
        2: protobuf.Field("name", "string", repeated=False, required=True),
        3: protobuf.Field("threshold", "uint32", repeated=False, required=True),
        4: protobuf.Field("keys", "PhylaxWalletKey", repeated=True, required=False, default=None),
        5: protobuf.Field("complete", "bool", repeated=False, required=False, default=None),
    }

    def __init__(self, *, slot: int, name: str, threshold: int,
                 keys: Optional[Sequence[PhylaxWalletKey]] = None,
                 complete: Optional[bool] = None) -> None:
        self.slot = slot
        self.name = name
        self.threshold = threshold
        self.keys: Sequence[PhylaxWalletKey] = keys if keys is not None else []
        self.complete = complete


for _cls in (PhylaxGetWallets, PhylaxWallets, PhylaxGetWallet, PhylaxWallet):
    mapping.DEFAULT_MAPPING.register(_cls)


# --- select a block (pass-through to a key block) ----------------------


class PhylaxSelectBlock(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7706
    FIELDS = {1: protobuf.Field("address", "uint32", repeated=False, required=True)}

    def __init__(self, *, address: int) -> None:
        self.address = address


mapping.DEFAULT_MAPPING.register(PhylaxSelectBlock)


# --- rename / delete a saved wallet (confirmed on the main block) -------


class PhylaxSetWalletName(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7707
    FIELDS = {
        1: protobuf.Field("slot", "uint32", repeated=False, required=True),
        2: protobuf.Field("name", "string", repeated=False, required=True),
    }

    def __init__(self, *, slot: int, name: str) -> None:
        self.slot = slot
        self.name = name


class PhylaxDeleteWallet(protobuf.MessageType):
    MESSAGE_WIRE_TYPE = 7708
    FIELDS = {1: protobuf.Field("slot", "uint32", repeated=False, required=True)}

    def __init__(self, *, slot: int) -> None:
        self.slot = slot


for _cls in (PhylaxSetWalletName, PhylaxDeleteWallet):
    mapping.DEFAULT_MAPPING.register(_cls)

"""Read-only ABI fixtures for the frozen BG3 4.1.1.7398727 arm64 image.

These checks encode only the PASS findings from ABI_REVIEW_7398727.md.  They
resolve the local symbol at every reviewed entry before checking instruction
words or vtable data.  CI installations without the migration binary skip the
module without consulting the installed game.
"""

from __future__ import annotations

import re
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
FROZEN_BINARY = (
    REPO_ROOT
    / "build/migration-binaries/4.1.1.7398727/Baldur's Gate 3"
)

CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
EXPECTED_ARM64_SLICE_OFFSET = 0x0F5C0000


@dataclass(frozen=True)
class Segment:
    vmaddr: int
    filesize: int
    fileoff: int


class FrozenArm64Image:
    def __init__(self, path: Path):
        self.path = path
        self.slice_offset, self.segments = self._parse()

    def _parse(self) -> tuple[int, tuple[Segment, ...]]:
        with self.path.open("rb") as stream:
            magic = stream.read(4)
            fat_formats = {
                b"\xca\xfe\xba\xbe": (">", False),
                b"\xbe\xba\xfe\xca": ("<", False),
                b"\xca\xfe\xba\xbf": (">", True),
                b"\xbf\xba\xfe\xca": ("<", True),
            }
            assert magic in fat_formats, "frozen BG3 image is not universal Mach-O"
            endian, is_64 = fat_formats[magic]
            count = struct.unpack(f"{endian}I", stream.read(4))[0]
            arch_format = f"{endian}iiQQII" if is_64 else f"{endian}iiIII"
            arch_size = struct.calcsize(arch_format)
            slice_offset = None
            for _ in range(count):
                fields = struct.unpack(arch_format, stream.read(arch_size))
                if fields[0] & 0xFFFFFFFF == CPU_TYPE_ARM64:
                    slice_offset = fields[2]
            assert slice_offset is not None, "frozen BG3 image has no arm64 slice"
            assert slice_offset == EXPECTED_ARM64_SLICE_OFFSET

            stream.seek(slice_offset)
            header = stream.read(32)
            fields = struct.unpack("<IiiIIIII", header)
            assert fields[0] == 0xFEEDFACF
            assert fields[1] & 0xFFFFFFFF == CPU_TYPE_ARM64
            command_count = fields[4]

            segments = []
            for _ in range(command_count):
                command_start = stream.tell()
                command, command_size = struct.unpack("<II", stream.read(8))
                assert command_size >= 8
                stream.seek(command_start)
                command_data = stream.read(command_size)
                assert len(command_data) == command_size
                if command == LC_SEGMENT_64:
                    values = struct.unpack("<II16sQQQQiiII", command_data[:72])
                    segments.append(Segment(values[3], values[6], values[5]))

        return slice_offset, tuple(segments)

    def read(self, va: int, size: int) -> bytes:
        for segment in self.segments:
            if segment.vmaddr <= va and va + size <= segment.vmaddr + segment.filesize:
                offset = self.slice_offset + segment.fileoff + va - segment.vmaddr
                with self.path.open("rb") as stream:
                    stream.seek(offset)
                    data = stream.read(size)
                assert len(data) == size, f"short read at {va:#x}"
                return data
        raise AssertionError(f"VA {va:#x} is not file-backed")

    def words(self, va: int, count: int) -> tuple[int, ...]:
        return struct.unpack(f"<{count}I", self.read(va, count * 4))

    def pointers(self, va: int, count: int) -> tuple[int, ...]:
        return struct.unpack(f"<{count}Q", self.read(va, count * 8))


@pytest.fixture(scope="module")
def image() -> FrozenArm64Image:
    if not FROZEN_BINARY.exists():
        pytest.skip(f"frozen 7398727 binary absent: {FROZEN_BINARY}")
    return FrozenArm64Image(FROZEN_BINARY)


REVIEWED_SYMBOL_VAS = {
    # Functor execution.
    0x10577E650,
    0x105782530,
    0x105782884,
    0x105784374,
    0x105785530,
    0x1057890F0,
    0x10578A7C0,
    0x10578B640,
    0x10578D6A8,
    0x1057911FC,
    0x105389568,
    # ComponentOps.
    0x100CFFAC4,
    0x105E80F34,
    0x10639350C,
    0x101E862C4,
    0x105E9A59C,
    0x1086E2558,
    0x1088625C8,
    # ECS update.
    0x100C81890,
    0x100F76F38,
    0x101050CF4,
    0x101EF5CC8,
    0x1063A057C,
    # RaycastAny and savegame.
    0x105C598B4,
    0x105C63AC8,
    0x104B5C750,
    # Replication.
    0x10290CEB4,
    0x106367268,
    0x106369F98,
    0x10639D944,
    0x10639DC1C,
}


@pytest.fixture(scope="module")
def symbols(image: FrozenArm64Image) -> dict[int, str]:
    del image  # Make binary absence skip before tool discovery.
    if not shutil.which("nm"):
        pytest.skip("nm unavailable for local-symbol verification")

    process = subprocess.Popen(
        ["nm", "-arch", "arm64", "-n", str(FROZEN_BINARY)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdout is not None
    found = {}
    for line in process.stdout:
        match = re.match(r"^([0-9a-fA-F]{16})\s+\S\s+(\S+)$", line.rstrip())
        if match:
            va = int(match.group(1), 16)
            if va in REVIEWED_SYMBOL_VAS:
                found[va] = match.group(2)
    stderr = process.stderr.read() if process.stderr else ""
    assert process.wait(timeout=300) == 0, stderr[:300]
    return found


def assert_symbol(symbols: dict[int, str], va: int, expected: str) -> None:
    assert symbols.get(va) == expected, f"symbol mismatch at {va:#x}"


def test_functor_execution_abi_is_unchanged(
        image: FrozenArm64Image, symbols: dict[int, str]):
    entries = {
        0x10577E650: (
            "__Z19ExecuteStatsFunctorPKN3eoc16StatsFunctorBaseEmRN3esv7functor23AttackTargetContextDataE",
            "e923b96dfc6f01a9fa6702a9f85f03a9",
        ),
        0x105782530: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_23AttackTargetContextDataE",
            "fc6fbaa9fa6701a9f85f02a9f65703a9",
        ),
        0x105782884: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_25AttackPositionContextDataE",
            "ed33b76deb2b016de923026dfc6f03a9",
        ),
        0x105784374: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_15MoveContextDataE",
            "eb2bb86de923016dfc6f02a9fa6703a9",
        ),
        0x105785530: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_17TargetContextDataE",
            "eb2bb86de923016dfc6f02a9fa6703a9",
        ),
        0x1057890F0: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_25NearbyAttackedContextDataE",
            "eb2bb86de923016dfc6f02a9fa6703a9",
        ),
        0x10578A7C0: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_26NearbyAttackingContextDataE",
            "eb2bb86de923016dfc6f02a9fa6703a9",
        ),
        0x10578B640: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_16EquipContextDataE",
            "fc6fbaa9fa6701a9f85f02a9f65703a9",
        ),
        0x10578D6A8: (
            "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_17SourceContextDataE",
            "eb2bb86de923016dfc6f02a9fa6703a9",
        ),
        0x1057911FC: (
            "__ZN3esv7functor20ExecuteStatsFunctorsERN3ecs11EntityWorldEPKN3eoc16StatsFunctorListERNS0_20InterruptContextDataE",
            "ef3bb66ded33016deb2b026de923036d",
        ),
    }
    for va, (symbol, entry_hex) in entries.items():
        assert_symbol(symbols, va, symbol)
        assert image.read(va, 16) == bytes.fromhex(entry_hex)

    damage_va = 0x105389568
    assert symbols[damage_va].startswith(
        "__ZN12_GLOBAL__N_125ProcessDealDamageFunctorsE"
    )
    assert image.read(damage_va, 16) == bytes.fromhex(
        "fc6fbaa9fa6701a9f85f02a9f65703a9"
    )

    # Target caller: x0=Result storage, x1=list, x2=context; no x8 sret.
    assert image.words(0x104DF1814, 4) == (
        0x910423E0, 0x911083E2, 0xAA1503E1, 0x94264F44,
    )
    assert image.words(0x105785558, 3) == (
        0xAA0203F4, 0xAA0103F5, 0xAA0003F3,
    )

    # Interrupt caller and callee retain x0=result, x1=world, x2=list, x3=context.
    assert image.words(0x105380770, 5) == (
        0xF9400101, 0x910903E0, 0x911F03E3, 0xAA1303E2, 0x9410429F,
    )
    assert image.words(0x10579122C, 4) == (
        0xAA0303FB, 0xAA0203FC, 0xF900F7E1, 0xAA0003F9,
    )

    # The 12-argument damage worker saves x0..x7 and reads stack args 8..10.
    assert image.words(0x1053895A4, 8) == (
        0xAA0703F8, 0xAA0603F9, 0xAA0503FA, 0xAA0403F4,
        0xA9008FE0, 0xF9000FE2, 0xB94023B3, 0xA94157BC,
    )


def test_component_ops_registry_abi_is_unchanged(
        image: FrozenArm64Image, symbols: dict[int, str]):
    assert_symbol(
        symbols, 0x100CFFAC4,
        "__ZN3ecl18RegisterComponentsERN3ecs11EntityWorldE",
    )
    assert_symbol(
        symbols, 0x105E80F34,
        "__ZN2ls24RegisterSharedComponentsERN3ecs11EntityWorldE",
    )
    assert_symbol(
        symbols, 0x10639350C,
        "__ZN3ecs11EntityWorld36AttachImmediateComponentDependenciesERKNS_11ComponentIdERKN2ls2IDINS_18EntityHandleTraitsEEEi",
    )
    assert image.words(0x100CFFD28, 1) == (0x910E4276,)  # world + 0x390
    assert image.words(0x105E81184, 1) == (0x910E4276,)
    assert image.words(0x1063935EC, 8) == (
        0x92403AE8, 0xF941CA89, 0xF8687920, 0xF9400008,
        0xF9401508, 0xAA1603E1, 0xAA1503E2, 0xD63F0100,
    )

    health_vtable = 0x1086E2558
    transform_vtable = 0x1088625C8
    assert_symbol(
        symbols, health_vtable,
        "__ZTVN3ecs12ComponentOpsIN3eoc15HealthComponentEEE",
    )
    assert_symbol(
        symbols, transform_vtable,
        "__ZTVN3ecs12ComponentOpsIN2ls18TransformComponentEEE",
    )
    assert image.pointers(health_vtable, 8)[7] == 0x101E862C4
    assert image.pointers(transform_vtable, 8)[7] == 0x105E9A59C
    assert_symbol(
        symbols, 0x101E862C4,
        "__ZN3ecs12ComponentOpsIN3eoc15HealthComponentEE28AddImmediateDefaultComponentEN2ls2IDINS_18EntityHandleTraitsEEEi",
    )
    assert_symbol(
        symbols, 0x105E9A59C,
        "__ZN3ecs12ComponentOpsIN2ls18TransformComponentEE28AddImmediateDefaultComponentENS1_2IDINS_18EntityHandleTraitsEEEi",
    )
    assert image.words(0x101E862D4, 3) == (
        0xAA0203F3, 0xF90007E1, 0xF9401014,
    )


def test_ecs_system_update_abi_is_unchanged(
        image: FrozenArm64Image, symbols: dict[int, str]):
    executor = 0x1063A057C
    assert_symbol(
        symbols, executor,
        "__ZN3ecs4core24SystemDependencyExecutor15ExecuteWTKernelEv",
    )
    assert image.words(executor + 0x2C, 10) == (
        0xF9401009, 0xB4000129, 0xF9400D28, 0xB40000E8, 0x3940492A,
        0x340000AA, 0xF9400120, 0xF9401661, 0x91080022, 0xD63F0100,
    )

    set_at = 0x100C81890
    assert_symbol(
        symbols, set_at,
        "__ZN2ls16DEPRECATED_ArrayIN3ecs4core15SystemTypeEntryENS_12NewInterfaceIS3_EENS_22GetDefaultValueFunctorIS3_EEE5SetAtEmOS3_",
    )
    assert image.words(set_at + 0x64, 4) == (
        0xF940004A, 0xF9400408, 0x52801F09, 0x9B092028,
    )

    register = 0x100F76F38
    assert_symbol(
        symbols, register,
        "__ZN3ecs8_private24SystemRegistrationHelper14RegisterSystemIN2ls30LevelInstanceLoadRequestSystemEJEEEvRNS_11EntityWorldEDpOT0_",
    )
    assert image.words(register + 0x390, 13) == (
        0x9100A297, 0xB9403A88, 0x6B13011F, 0x540000A8,
        0xB9404688, 0x8B130101, 0xAA1703E0, 0x97F42846,
        0x9100C3F4, 0x9100C3E2, 0xAA1703E0, 0xAA1303E1,
        0x97F42966,
    )
    assert image.words(register + 0x130, 5)[0:2] == (0x0E040E60, 0x9003A508)
    assert image.words(register + 0x13C, 2) == (0xFD001FE0, 0xF90027E8)

    assert_symbol(
        symbols, 0x101050CF4,
        "__ZN3ecs8_private24SystemRegistrationHelper12SystemUpdateIN3ecl20PickingHelperManagerEEEvRNS_6SystemERNS_11EntityWorldERKN2ls8GameTimeE",
    )
    assert_symbol(
        symbols, 0x101EF5CC8,
        "__ZN3ecs8_private24SystemRegistrationHelper12SystemUpdateIN2ls24AnimationBlueprintSystemEEEvRNS_6SystemERNS_11EntityWorldERKNS3_8GameTimeE",
    )
    assert image.words(0x101050D0C, 2) == (0xAA0203F3, 0xAA0003F4)
    assert image.words(0x101EF5CF0, 2) == (0xAA0203FB, 0xAA0103F6)


def test_raycast_any_worker_abi_is_unchanged(
        image: FrozenArm64Image, symbols: dict[int, str]):
    wrapper = 0x105C598B4
    worker = 0x105C63AC8
    assert_symbol(
        symbols, wrapper,
        "__ZNK3phx10PhysXScene10RaycastAnyERK8Vector3fS3_N2ls12EPhysicsTypeEjjNS4_15EPhysicsContextEjjNS4_8OptionalIRNS4_26PhysicsSceneScopedReadLockEEE",
    )
    assert_symbol(
        symbols, worker,
        "__ZN3phx17PhysXSceneHelpers10RaycastAnyEPN5physx7PxSceneERK8Vector3fS6_N2ls12EPhysicsTypeEjjNS7_15EPhysicsContextEjjNS7_8OptionalIRNS7_26PhysicsSceneScopedReadLockEEE",
    )
    assert image.words(wrapper, 6) == (
        0xB94003E8, 0xA940A7EA, 0xF9404C00,
        0xA900A7EA, 0xB90003E8, 0x14002880,
    )
    assert image.words(worker + 0x44, 1) == (0xF94013A8,)
    assert image.words(worker + 0x120, 2) == (0xF2401D1F, 0x54000200)

    # Disengaged optional: lockRead(+0x310), raycast(+0x2b8), unlockRead(+0x318).
    assert image.words(worker + 0x164, 6) == (
        0xF9400268, 0xF9418908, 0xAA1303E0,
        0xD2800001, 0x52800002, 0xD63F0100,
    )
    assert image.words(worker + 0x180, 3) == (
        0xF9400268, 0xF9415D08, 0xB0016846,
    )
    assert image.words(worker + 0x1B4, 4) == (
        0xF9400268, 0xF9418D08, 0xAA1303E0, 0xD63F0100,
    )


def test_savegame_visit_entry_abi_is_unchanged(
        image: FrozenArm64Image, symbols: dict[int, str]):
    visit = 0x104B5C750
    assert_symbol(
        symbols, visit,
        "__ZN3esv20OsirisVariableHelper13SavegameVisitEPN3eoc15SavegameVisitorE",
    )
    assert image.read(visit, 16) == bytes.fromhex(
        "ff0301d1f65701a9f44f02a9fd7b03a9"
    )
    assert image.words(visit + 0x14, 7) == (
        0xAA0103F3, 0xAA0003F4, 0xF9405820, 0xF9400008,
        0xF9403908, 0xD001F8C1, 0x913C0021,
    )


def test_replication_syncbuffers_layout_is_unchanged(
        image: FrozenArm64Image, symbols: dict[int, str]):
    assert_symbol(
        symbols, 0x10290CEB4,
        "__ZN3ecs11EntityWorld12GetComponentIN3eoc14ArmorComponentEEENSt3__111conditionalIX17v_ShouldBeWrappedIT_EENS_4sync10SyncedDataIS6_EEPS6_E4typeERKN2ls2IDINS_18EntityHandleTraitsEEE",
    )
    assert image.words(0x10290CEF0, 2) == (0xF94007E2, 0xF9400263)

    mark_dirty = 0x106367268
    assert_symbol(
        symbols, mark_dirty,
        "__ZN3ecs4sync25EntitiesDirtyFieldsBuffer9MarkDirtyEN2ls2IDINS_18EntityHandleTraitsEEEiONS2_13DynamicBitSetINS2_15TaggedAllocatorIiEEEE",
    )
    assert image.words(mark_dirty + 0x2C, 6) == (
        0x52800028, 0x39004008, 0xF9400008,
        0x93407C49, 0x8B091914, 0xB9802E93,
    )

    flatten = 0x10639DC1C
    assert_symbol(
        symbols, flatten,
        "__ZN3ecs4sync8protocol12_GLOBAL__N_123FlattenComponentsLayoutERKN2ls4SpanIKNS3_2IDINS_18EntityHandleTraitsEEEEERKNS3_7HashMapIS7_NS3_13DynamicBitSetINS3_15TaggedAllocatorIiEEEENS0_15RPLHashTableOpsEEEi",
    )
    assert image.words(flatten + 0x3B4, 4) == (
        0xB98009A9, 0x34002209, 0xF9400FE8, 0xF9400108,
    )
    assert image.words(flatten + 0x3D0, 3) == (
        0xF94001AA, 0x937E7D29, 0xB8696949,
    )
    assert image.words(flatten + 0x3E0, 3) == (
        0xF94011AA, 0xF94009AB, 0xF869794C,
    )
    assert image.words(flatten + 0x400, 2) == (0xF94019A8, 0x8B091116)

    ensure = 0x10639D944
    assert_symbol(
        symbols, ensure,
        "__ZN2ls13DynamicBitSetINS_15TaggedAllocatorIiEEE6EnsureEib",
    )
    assert image.words(ensure + 0x1C, 3) == (
        0xB9400808, 0x6B01011F, 0x540000AC,
    )
    assert image.words(ensure + 0x38, 4) == (
        0xB9400E88, 0x7101051F, 0x5400004B, 0xF9400294,
    )

    sync = 0x106369F98
    assert_symbol(
        symbols, sync,
        "__ZN3ecs4sync26EntityReplicationAuthority4SyncERKN2ls8GameTimeE",
    )
    assert image.words(sync + 0x1178, 2) == (0x39408369, 0x34000149)
    assert image.words(sync + 0x383C, 3) == (
        0x3900831F, 0xF9400B14, 0xB9801F08,
    )

#!/usr/bin/python3
"""Resolve an NVIDIA scanout pool from hardware state, without guest scans.

The write-capable family profiles cover Turing/TU10x and Ada/AD10x. VFIO supplies
PCI/BAR geometry, PMC_BOOT_0 selects the family, and display/BAR1 locations are
accepted only after structural validation.  It does not depend on nvlddmkm
virtual addresses, private structure offsets, or a guest-physical-memory scan.

If the current PRAMIN selector does not cover the display instance, the tool
fails unless --allow-pramin-switch is supplied.  Switching is only safe while
the VM is suspended.
"""

from __future__ import annotations

import argparse
import ctypes
import fcntl
import json
import mmap
import os
import struct
from pathlib import Path


WINDOW_COUNT = 8
METHOD_STRIDE = 0x1000

VFIO_DEVICE_GET_REGION_INFO = 0x3B6C
VFIO_REGION_INFO_FLAG_READ = 1 << 0
VFIO_REGION_INFO_FLAG_WRITE = 1 << 1
VFIO_REGION_INFO_FLAG_MMAP = 1 << 2
VFIO_PCI_BAR0_REGION_INDEX = 0
VFIO_PCI_BAR1_REGION_INDEX = 1
VFIO_PCI_CONFIG_REGION_INDEX = 7


# Values that cannot be inferred safely without issuing GPU commands remain in
# a chipset-family profile.  Everything else is queried from VFIO or located by
# structural validation below.  Adding another family is deliberately data-only
# until its MMU write registers and page-table format have been independently
# validated.
GPU_PROFILES = (
    {
        "name": "turing-tu10x-v1",
        "chipsets": {0x162, 0x164, 0x166, 0x167, 0x168},
        "pbus_bar0_window": 0x1700,
        "pramin_offset": 0x700000,
        "pramin_size": 0x100000,
        "preferred_display_instance_register": 0x610014,
        "display_instance_register_range": (0x610000, 0x610100),
        "display_instance_size": 0x10000,
        "preferred_method_base": 0x690000,
        "method_base_range": (0x600000, 0x700000),
        "bar1_block": 0xB80F40,
        "bar1_block_range": (0xB80000, 0xB90000),
        "bar1_bind_status": 0xB80F50,
        "mmu_invalidate_pdb": 0xB830A0,
        "mmu_invalidate_upper_pdb": 0xB830A4,
        "mmu_invalidate": 0xB830B0,
        "instance_pdb_offset": 0x200,
        "instance_limit_offset": 0x208,
        "zero_instance_limit_uses_bar1_size": False,
        "page_table_levels": 3,
        "page_2m": 0x200000,
        "write_capable": True,
    },
    {
        # AD10x keeps the TU102 virtual-register MMU invalidation path and the
        # GP10x PDE encoding used here.  GA10x/AD10x additionally allow a
        # 512 MiB PD1 leaf, but the sparse mapper installs only 2 MiB PDE0
        # leaves. The scanner still requires unique live display and BAR1
        # instance validation before it emits a write-capable descriptor.
        "name": "ada-ad10x-v1",
        "chipsets": {0x192, 0x193, 0x194, 0x196, 0x197},
        "pbus_bar0_window": 0x1700,
        "pramin_offset": 0x700000,
        "pramin_size": 0x100000,
        "preferred_display_instance_register": 0x610014,
        "display_instance_register_range": (0x610000, 0x610100),
        "display_instance_size": 0x10000,
        "preferred_method_base": 0x690000,
        "method_base_range": (0x600000, 0x700000),
        "bar1_block": 0xB80F40,
        "bar1_block_range": (0xB80000, 0xB90000),
        "bar1_bind_status": 0xB80F50,
        "mmu_invalidate_pdb": 0xB830A0,
        "mmu_invalidate_upper_pdb": 0xB830A4,
        "mmu_invalidate": 0xB830B0,
        "instance_pdb_offset": 0x200,
        "instance_limit_offset": 0x208,
        # The AD104 Windows driver leaves the legacy NV_RAMIN_ADR_LIMIT pair
        # clear for the 16 GiB resizable aperture.  VFIO remains the authority
        # for the live PCI BAR size, and the renderer independently rechecks
        # that size before mapping.
        "zero_instance_limit_uses_bar1_size": True,
        "page_table_levels": 3,
        "page_2m": 0x200000,
        "write_capable": True,
    },
)

FORMAT_BPP = {
    0xCF: 4,  # A8R8G8B8
    0xE6: 4,  # X8R8G8B8
    0xD5: 4,  # A8B8G8R8
    0xF9: 4,  # X8B8G8R8
    0xDF: 4,  # A2R10G10B10
    0xD1: 4,  # A2B10G10R10
}


def vfio_region_info(fd: int, index: int) -> dict[str, int]:
    encoded = bytearray(struct.pack("<IIIIQQ", 32, 0, index, 0, 0, 0))
    fcntl.ioctl(fd, VFIO_DEVICE_GET_REGION_INFO, encoded, True)
    argsz, flags, observed_index, cap_offset, size, offset = struct.unpack(
        "<IIIIQQ", encoded
    )
    if argsz < 32 or observed_index != index:
        raise RuntimeError(f"invalid VFIO region metadata for index {index}")
    return {
        "index": index,
        "flags": flags,
        "cap_offset": cap_offset,
        "size": size,
        "offset": offset,
    }


def gpu_profile(chipset: int) -> dict[str, object]:
    matches = [profile for profile in GPU_PROFILES if chipset in profile["chipsets"]]
    if len(matches) != 1:
        raise RuntimeError(
            f"unsupported NVIDIA chipset 0x{chipset:03x}; discovery is read-only "
            "until a validated family profile is added"
        )
    return matches[0]


def auto_vfio_fd(pid: int) -> int:
    candidates: list[int] = []
    for fdinfo in Path(f"/proc/{pid}/fdinfo").iterdir():
        try:
            lines = fdinfo.read_text().splitlines()
        except OSError:
            continue
        syspath_text = next(
            (line.partition(":")[2].strip() for line in lines
             if line.startswith("vfio-device-syspath:")),
            "",
        )
        if not syspath_text:
            continue
        syspath = Path(syspath_text)
        try:
            vendor = int((syspath / "vendor").read_text(), 0)
            class_code = int((syspath / "class").read_text(), 0)
        except (OSError, ValueError):
            continue
        if vendor == 0x10DE and class_code >> 16 == 0x03:
            candidates.append(int(fdinfo.name))
    if len(candidates) != 1:
        raise RuntimeError(
            f"expected one NVIDIA display VFIO descriptor, got {candidates}"
        )
    return candidates[0]


class Hardware:
    def __init__(self, pid: int, fd_number: int | None, allow_switch: bool):
        if fd_number is None:
            fd_number = auto_vfio_fd(pid)
        self.fd_number = fd_number
        libc = ctypes.CDLL(None, use_errno=True)
        pidfd = libc.syscall(434, pid, 0)
        if pidfd < 0:
            raise OSError(ctypes.get_errno(), "pidfd_open")
        self.fd = libc.syscall(438, pidfd, fd_number, 0)
        os.close(pidfd)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), "pidfd_getfd")
        self.bar0_region = vfio_region_info(self.fd, VFIO_PCI_BAR0_REGION_INDEX)
        self.bar1_region = vfio_region_info(self.fd, VFIO_PCI_BAR1_REGION_INDEX)
        self.config_region = vfio_region_info(self.fd, VFIO_PCI_CONFIG_REGION_INDEX)
        required = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE
        if self.bar0_region["size"] < 0x1000 or \
                self.bar0_region["flags"] & required != required or \
                not self.bar0_region["flags"] & VFIO_REGION_INFO_FLAG_MMAP:
            raise RuntimeError("VFIO BAR0 is not a readable/writable mmap region")
        if self.bar1_region["size"] < 0x200000 or \
                self.bar1_region["flags"] & required != required or \
                not self.bar1_region["flags"] & VFIO_REGION_INFO_FLAG_MMAP:
            raise RuntimeError("VFIO BAR1 is not a readable/writable mmap region")
        config = os.pread(self.fd, 12, self.config_region["offset"])
        if len(config) != 12:
            raise RuntimeError("short VFIO PCI configuration read")
        self.vendor_id, self.device_id = struct.unpack_from("<HH", config)
        self.class_code = int.from_bytes(config[9:12], "little")
        if self.vendor_id != 0x10DE or self.class_code >> 16 != 0x03:
            raise RuntimeError(
                f"VFIO device {self.vendor_id:04x}:{self.device_id:04x} is not "
                "an NVIDIA display function"
            )
        self.bar0 = mmap.mmap(
            self.fd,
            self.bar0_region["size"],
            flags=mmap.MAP_SHARED,
            prot=mmap.PROT_READ | mmap.PROT_WRITE,
            offset=self.bar0_region["offset"],
        )
        self.boot0 = self.read32(0)
        self.chipset = (self.boot0 >> 20) & 0x1FF
        self.profile = gpu_profile(self.chipset)
        self.pbus_bar0_window = int(self.profile["pbus_bar0_window"])
        self.pramin_offset = int(self.profile["pramin_offset"])
        self.pramin_size = int(self.profile["pramin_size"])
        for offset, size, name in (
            (self.pbus_bar0_window, 4, "PBUS BAR0 window"),
            (self.pramin_offset, self.pramin_size, "PRAMIN aperture"),
        ):
            if offset < 0 or offset + size > self.bar0_region["size"]:
                raise RuntimeError(f"{name} lies outside VFIO BAR0")
        self.original_selector = self.read32(self.pbus_bar0_window)
        self.current_selector = self.original_selector
        self.allow_switch = allow_switch

    def close(self) -> None:
        try:
            if self.current_selector != self.original_selector:
                self.write32(self.pbus_bar0_window, self.original_selector)
                self.current_selector = self.read32(self.pbus_bar0_window)
                if self.current_selector != self.original_selector:
                    raise RuntimeError("failed to restore PRAMIN selector")
        finally:
            self.bar0.close()
            os.close(self.fd)

    def read32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.bar0, offset)[0]

    def write32(self, offset: int, value: int) -> None:
        struct.pack_into("<I", self.bar0, offset, value)

    def read_vram(self, address: int, length: int) -> bytes:
        output = bytearray()
        while length:
            window_base = address & ~(self.pramin_size - 1)
            current_base = (self.current_selector & 0x00FFFFFF) << 16
            if current_base != window_base:
                if not self.allow_switch:
                    raise RuntimeError(
                        f"display state requires PRAMIN switch from 0x{current_base:x} "
                        f"to 0x{window_base:x}; suspend the VM and use "
                        f"--allow-pramin-switch"
                    )
                target = self.original_selector & 0x03000000
                selector = target | ((window_base >> 16) & 0x00FFFFFF)
                self.write32(self.pbus_bar0_window, selector)
                self.current_selector = self.read32(self.pbus_bar0_window)
                if self.current_selector != selector:
                    raise RuntimeError("PRAMIN selector readback mismatch")
            offset = address - window_base
            count = min(length, self.pramin_size - offset)
            start = self.pramin_offset + offset
            output += self.bar0[start : start + count]
            address += count
            length -= count
        return bytes(output)


def assembled_windows(hw: Hardware, method_base: int) -> list[dict[str, int | str]]:
    windows: list[dict[str, int | str]] = []
    for index in range(WINDOW_COUNT):
        base = method_base + index * METHOD_STRIDE
        size = hw.read32(base + 0x224)
        storage = hw.read32(base + 0x228)
        params = hw.read32(base + 0x22C)
        pitch_blocks = hw.read32(base + 0x230) & 0x1FFF
        offset_units = hw.read32(base + 0x260)
        width, height = size & 0xFFFF, size >> 16
        bpp = FORMAT_BPP.get(params & 0xFF)
        if not width or not height or not pitch_blocks or bpp is None:
            continue
        if width > 16384 or height > 16384 or pitch_blocks * 64 < width * bpp:
            raise RuntimeError(f"window {index}: invalid assembled dimensions/pitch")
        if storage & 0x10:
            continue  # pitch-linear objects are not the DWM scanout pool
        log2_gobs_y = storage & 0xF
        if log2_gobs_y > 5:
            raise RuntimeError(f"window {index}: invalid block height")
        block_height = 8 << log2_gobs_y
        padded_height = (height + block_height - 1) // block_height * block_height
        required = pitch_blocks * 64 * padded_height
        windows.append(
            {
                "window": index,
                "display_channel": index + 1,
                "width": width,
                "height": height,
                "format": f"0x{params & 0xFF:02x}",
                "bytes_per_pixel": bpp,
                "pitch_blocks": pitch_blocks,
                "pitch_bytes": pitch_blocks * 64,
                "log2_gobs_per_block_y": log2_gobs_y,
                "block_height_rows": block_height,
                "padded_height": padded_height,
                "required_bytes": required,
                "surface_offset_bytes": offset_units << 8,
            }
        )
    return windows


def ordered_offsets(preferred: int, start: int, end: int, step: int) -> list[int]:
    result = [preferred]
    result.extend(offset for offset in range(start, end, step) if offset != preferred)
    return result


def in_original_pramin_window(hw: Hardware, address: int, size: int) -> bool:
    base = (hw.original_selector & 0x00FFFFFF) << 16
    return address >= base and size <= hw.pramin_size and address - base <= hw.pramin_size - size


def method_base_candidates(hw: Hardware) -> list[tuple[int, list[dict[str, int | str]]]]:
    profile = hw.profile
    start, end = (int(value) for value in profile["method_base_range"])
    preferred = int(profile["preferred_method_base"])
    candidates: list[tuple[int, list[dict[str, int | str]]]] = []
    # Display method banks are 64 KiB aligned; individual windows are 4 KiB
    # apart inside the bank.  Searching at the bank granularity avoids the same
    # live windows appearing as seven shifted aliases.
    for base in ordered_offsets(preferred, start, end, 0x10000):
        if base < 0 or base + WINDOW_COUNT * METHOD_STRIDE > hw.bar0_region["size"]:
            continue
        windows = assembled_windows(hw, base)
        if windows:
            candidates.append((base, windows))
    return candidates


def display_instance_candidates(hw: Hardware) -> list[tuple[int, int, bytes]]:
    profile = hw.profile
    start, end = (int(value) for value in profile["display_instance_register_range"])
    preferred = int(profile["preferred_display_instance_register"])
    size = int(profile["display_instance_size"])
    candidates: list[tuple[int, int, bytes]] = []
    seen_addresses: set[int] = set()
    for register in ordered_offsets(preferred, start, end, 4):
        if register < 0 or register + 4 > hw.bar0_region["size"]:
            continue
        address = hw.read32(register) << 16
        if not address or address in seen_addresses:
            continue
        # The family-profile hint may require one controlled aperture switch
        # while suspended.  Fallback discovery never chases arbitrary MMIO
        # values outside the already selected PRAMIN window.
        if register != preferred and not in_original_pramin_window(hw, address, size):
            continue
        seen_addresses.add(address)
        try:
            instance = hw.read_vram(address, size)
        except RuntimeError:
            continue
        candidates.append((register, address, instance))
    return candidates


def resolve_hardware_layout(
    hw: Hardware, owned_vram_bases: set[int] | None,
    display_channel: int | None,
) -> tuple[int, int, int, list[dict[str, int | str]], list[dict[str, int | str]]]:
    successes: list[
        tuple[int, int, int, list[dict[str, int | str]], list[dict[str, int | str]]]
    ] = []
    errors: list[str] = []
    for method_base, windows in method_base_candidates(hw):
        for register, address, instance in display_instance_candidates(hw):
            try:
                selected_windows, pool = resolve_pool(
                    windows, ramht_objects(instance), owned_vram_bases,
                    display_channel
                )
            except RuntimeError as error:
                errors.append(
                    f"methods=0x{method_base:x} instance-reg=0x{register:x}: {error}"
                )
                continue
            successes.append((method_base, register, address, selected_windows, pool))
    identities = {
        (
            success[0],
            success[1],
            success[2],
            tuple(int(str(obj["allocation_base"]), 0) for obj in success[4]),
        )
        for success in successes
    }
    if len(identities) != 1:
        detail = errors[:8]
        raise RuntimeError(
            f"expected one structurally validated display layout, got "
            f"{len(identities)}; details={detail}"
        )
    return successes[0]


def resolve_bar1_block(hw: Hardware) -> tuple[int, int, int, int]:
    profile = hw.profile
    pdb_offset = int(profile["instance_pdb_offset"])
    limit_offset = int(profile["instance_limit_offset"])
    read_size = max(pdb_offset, limit_offset) + 8
    start, end = (int(value) for value in profile["bar1_block_range"])
    preferred = int(profile["bar1_block"])
    matches: list[tuple[int, int, int, int]] = []
    seen_instances: set[int] = set()
    for register in ordered_offsets(preferred, start, end, 4):
        if register < 0 or register + 4 > hw.bar0_region["size"]:
            continue
        block = hw.read32(register)
        if not block & 0x80000000:
            continue
        instance_address = (block & 0x0FFFFFFF) << 12
        if instance_address in seen_instances:
            continue
        if register != preferred and not in_original_pramin_window(
            hw, instance_address, read_size
        ):
            continue
        seen_instances.add(instance_address)
        try:
            instance = hw.read_vram(instance_address, read_size)
        except RuntimeError:
            continue
        pdb = struct.unpack_from("<Q", instance, pdb_offset)[0]
        raw_limit = struct.unpack_from("<Q", instance, limit_offset)[0]
        limit = (
            hw.bar1_region["size"]
            if raw_limit == 0
            and register == preferred
            and bool(profile.get("zero_instance_limit_uses_bar1_size", False))
            else raw_limit + 1
        )
        # The low PDB bits contain instance-block flags; the renderer masks
        # them before walking.  Requiring raw 4 KiB alignment would reject the
        # valid TU104 encoding.
        if not pdb or not pdb & ~0xFFF or limit != hw.bar1_region["size"]:
            continue
        matches.append((register, instance_address, pdb, limit))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one structurally valid BAR1 instance register, got {matches}"
        )
    return matches[0]


def ramht_objects(instance: bytes) -> list[dict[str, int | str]]:
    result: list[dict[str, int | str]] = []
    seen: set[tuple[int, int, int]] = set()
    for entry_offset in range(0, 0x2000, 8):
        handle, context = struct.unpack_from("<II", instance, entry_offset)
        if not handle:
            continue
        channel = (context >> 25) & 0x7F
        object_offset = ((context >> 14) & 0x7FF) << 5
        if not 0x2000 <= object_offset <= len(instance) - 20:
            continue
        flags, base_lo, base_hi, limit_lo, limit_hi = struct.unpack_from(
            "<IIIII", instance, object_offset
        )
        target = flags & 3
        kind = (flags >> 20) & 1
        base = (((base_hi & 0x7F) << 32) | base_lo) << 8
        limit = ((((limit_hi & 0x7F) << 32) | limit_lo) << 8) | 0xFF
        if limit < base:
            continue
        key = (handle, channel, object_offset)
        if key in seen:
            continue
        seen.add(key)
        result.append(
            {
                "context_handle": f"0x{handle:08x}",
                "display_channel": channel,
                "ramht_entry": entry_offset // 8,
                "context_object_offset": f"0x{object_offset:x}",
                "flags": f"0x{flags:08x}",
                "target": target,
                "kind": kind,
                "allocation_base": f"0x{base:x}",
                "allocation_limit": f"0x{limit:x}",
                "allocation_size": limit - base + 1,
            }
        )
    return result


def resolve_pool(
    windows: list[dict[str, int | str]],
    objects: list[dict[str, int | str]],
    owned_vram_bases: set[int] | None,
    display_channel: int | None,
) -> tuple[list[dict[str, int | str]], list[dict[str, int | str]]]:
    pools: list[tuple[dict[str, int | str], list[dict[str, int | str]]]] = []
    for window in windows:
        matches = [
            obj
            for obj in objects
            if obj["display_channel"] == window["display_channel"]
            and obj["target"] == 1
            and obj["kind"] == 1
            and obj["allocation_size"] == window["required_bytes"]
        ]
        unique_bases = {obj["allocation_base"] for obj in matches}
        if len(matches) >= 2 and len(matches) == len(unique_bases):
            pools.append((window, matches))
    if display_channel is not None:
        pools = [
            pool for pool in pools
            if int(pool[0]["display_channel"]) == display_channel
        ]
    summary = [
        {
            "window": pool[0]["window"],
            "channel": pool[0]["display_channel"],
            "format": pool[0]["format"],
            "dimensions": f"{pool[0]['width']}x{pool[0]['height']}",
            "allocation_size": pool[0]["required_bytes"],
            "pool_count": len(pool[1]),
            "bases": [obj["allocation_base"] for obj in pool[1]],
        }
        for pool in pools
    ]
    if owned_vram_bases is not None:
        owned_pools = [
            pool
            for pool in pools
            if {
                int(str(obj["allocation_base"]), 0)
                for obj in pool[1]
            }.issubset(owned_vram_bases)
        ]
        if len(owned_pools) != 1:
            raise RuntimeError(
                "WDDM ownership did not select exactly one hardware plane: "
                f"matches={len(owned_pools)} pools={summary}"
            )
        pools = owned_pools
    if not pools:
        raise RuntimeError(f"no structurally valid scanout pools found: {summary}")
    selected_windows = [pool[0] for pool in pools]
    selected_objects = [obj for pool in pools for obj in pool[1]]
    expected_count = sum(len(pool[1]) for pool in pools)
    if len(selected_objects) != expected_count:
        raise RuntimeError("scanout pool flattening lost objects")
    if owned_vram_bases is not None and len(selected_objects) < 2:
        raise RuntimeError("owned plane has fewer than two buffers")
    ranges = sorted(
        (int(str(obj["allocation_base"]), 0), int(str(obj["allocation_limit"]), 0))
        for obj in selected_objects
    )
    for (_, previous_limit), (base, _) in zip(ranges, ranges[1:]):
        if base <= previous_limit:
            raise RuntimeError("scanout allocations overlap")
    return selected_windows, sorted(
        selected_objects,
        key=lambda obj: (int(obj["display_channel"]), int(str(obj["allocation_base"]), 0)),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--fd", type=int, help="QEMU VFIO fd; auto-detected by default")
    parser.add_argument("--allow-pramin-switch", action="store_true")
    parser.add_argument(
        "--owned-vram-base",
        action="append",
        type=lambda value: int(value, 0),
        help="WDDM-owned resident allocation base; repeat for all process allocations",
    )
    parser.add_argument(
        "--display-channel",
        type=int,
        help="select one structurally validated display channel",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("run through /bin/csu")

    hw = Hardware(args.pid, args.fd, args.allow_pramin_switch)
    try:
        owned_vram_bases = (
            set(args.owned_vram_base) if args.owned_vram_base is not None else None
        )
        method_base, display_instance_register, display_instance, selected_windows, pool = (
            resolve_hardware_layout(hw, owned_vram_bases, args.display_channel)
        )
        bar1_block, bar1_instance, bar1_pdb, bar1_limit = resolve_bar1_block(hw)
        bases = [int(str(obj["allocation_base"]), 0) for obj in pool]
        limits = [int(str(obj["allocation_limit"]), 0) for obj in pool]
        vram_base = min(bases) & ~0x1FFFFF
        vram_limit = (max(limits) + 0x200000) & ~0x1FFFFF
        result = {
            "mode": (
                "hardware-structural-signatures+wddm-process-ownership"
                if owned_vram_bases is not None
                else "hardware-structural-signatures"
            ),
            "hardware": {
                "schema_version": 1,
                "profile": str(hw.profile["name"]),
                "write_capable": bool(hw.profile["write_capable"]),
                "qemu_vfio_fd": hw.fd_number,
                "pci": {
                    "vendor_id": hw.vendor_id,
                    "device_id": hw.device_id,
                    "class_code": hw.class_code,
                },
                "boot0": hw.boot0,
                "chipset": hw.chipset,
                "regions": {
                    "bar0_size": hw.bar0_region["size"],
                    "bar0_offset": hw.bar0_region["offset"],
                    "bar1_size": hw.bar1_region["size"],
                    "bar1_offset": hw.bar1_region["offset"],
                },
                "registers": {
                    "pbus_bar0_window": int(hw.profile["pbus_bar0_window"]),
                    "pramin_offset": int(hw.profile["pramin_offset"]),
                    "pramin_size": int(hw.profile["pramin_size"]),
                    "display_instance": display_instance_register,
                    "method_base": method_base,
                    "bar1_block": bar1_block,
                    "bar1_bind_status": int(hw.profile["bar1_bind_status"]),
                    "mmu_invalidate_pdb": int(hw.profile["mmu_invalidate_pdb"]),
                    "mmu_invalidate_upper_pdb": int(
                        hw.profile["mmu_invalidate_upper_pdb"]
                    ),
                    "mmu_invalidate": int(hw.profile["mmu_invalidate"]),
                },
                "page_table": {
                    "instance_pdb_offset": int(hw.profile["instance_pdb_offset"]),
                    "instance_limit_offset": int(hw.profile["instance_limit_offset"]),
                    "levels": int(hw.profile["page_table_levels"]),
                    "page_2m": int(hw.profile["page_2m"]),
                },
                "validation": {
                    "display_layout": "structural-unique",
                    "bar1_instance": "structural-unique",
                    "bar1_instance_address": f"0x{bar1_instance:x}",
                    "bar1_pdb": f"0x{bar1_pdb:x}",
                    "bar1_limit": f"0x{bar1_limit:x}",
                },
            },
            "display_instance_address": f"0x{display_instance:x}",
            "pramin_selector": f"0x{hw.original_selector:08x}",
            "windows": selected_windows,
            "surface_pool": pool,
            "mapping_requirement": {
                "vram_base": f"0x{vram_base:x}",
                "vram_limit": f"0x{vram_limit:x}",
                "span": f"0x{vram_limit - vram_base:x}",
                "alignment": "0x200000",
            },
            "invariants": {
                "pool_count": len(pool),
                "display_channels": sorted({int(obj["display_channel"]) for obj in pool}),
                "target": "video-memory",
                "kind": "block-linear",
                "non_overlapping": True,
            },
        }
        encoded = json.dumps(result, indent=2) + "\n"
        if args.output:
            args.output.write_text(encoded)
        print(encoded, end="")
    finally:
        hw.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# Profiled NVIDIA BAR1 scanout overlay

The directory and binary retain their historical `tu104` names so existing
launchers and state-management commands continue to work. The normal launcher
now supplies a detected descriptor; compiled TU104 defaults remain only for
backward-compatible direct invocations.

This tool establishes a persistent CPU mapping of the Windows guest's
block-linear scanout allocations and writes a solid test rectangle without a
guest process, per-frame PRAMIN switching, detiling, allocation, or semantic
driver scan.

The mapping operation must run while `win11` is suspended. It replaces
only invalid/sparse 2 MiB BAR1 PDEs, saves their exact original 128-bit values,
invalidates the BAR1 TLB, and can restore those values exactly. Rendering runs
while the VM is live.

Current 610.88 pool:

```text
channel 1: 0x08e00000 0x09800000 0x0a200000
channel 2: 0x05c00000 0x07000000 0x07a00000
mapping: BAR1 VA 0x08000000 -> VRAM 0x05c00000, span 0x05000000
```

Build flags use `-O3 -march=native -flto` plus native interposition/alignment
optimizations. Box frames enumerate only their two-pixel perimeters, batch and
coalesce every rectangle in one pass, then reuse the translated runs. A
startup-built screen-coordinate lookup table memoizes NVIDIA block-linear
offsets, packed color/offset keys use a fixed-width radix sort, and identical
integer geometry frames reuse the previous translated batch. The hot
path uses AVX-512/AVX2 and 64-bit non-temporal stores through the root-only
`tu104_bar1_wc` mmap bridge and emits one `SFENCE` per complete repaint. VFIO's
uncached BAR1 mmap remains a functional fallback.
It supports CPU affinity, `SCHED_FIFO`, and absolute monotonic-clock pacing.

Do not run `install` or `restore` against a live guest. The NVIDIA driver owns
the BAR1 page tables and must not race those one-time modifications.

`render-current-pool.sh` now structurally scans the C57E assembled state and
display RAMHT during a brief suspended interval, then verifies every installed
BAR1 PDE before resuming and starting the hot path. It does not invoke memflow,
scan guest physical memory, or trust cached surface addresses.

The scanner queries the live VFIO PCI configuration and BAR region metadata,
reads `PMC_BOOT_0` to select a chipset-family profile, and structurally resolves
the display method bank, display instance, and BAR1 instance register. It emits
a versioned hardware descriptor which the C renderer consumes. The renderer
queries BAR sizes again and refuses a descriptor whose registers lie outside
BAR0 or whose instance-reported BAR1 limit differs from VFIO.

MMU invalidation registers, instance-field offsets, page-table depth, and the
block-linear swizzle cannot be safely inferred by passive MMIO scanning. Those
remain explicit family-profile data. `turing-tu10x-v1` covers known
TU102/TU104/TU106/TU116/TU117 chipset IDs, while `ada-ad10x-v1` covers
AD102/AD103/AD104/AD106/AD107. The Ada profile handles the clear legacy
`NV_RAMIN_ADR_LIMIT` field used with a large resizable BAR by validating the
live VFIO BAR1 size independently. Unsupported families still fail closed.
The WC helper now accepts any NVIDIA display
function bound to `vfio-pci` with a prefetchable BAR1 of at least 256 MiB; its
device-node name remains `/dev/tu104-bar1-wc` for compatibility.
Environment overrides are `BAR1_RECT`, `BAR1_COLOR`, `BAR1_HZ`, and
`BAR1_DURATION_MS`. Box geometry arrives at 360 Hz, while the cached outlines
default to a 2 kHz BAR1 repaint to reduce races with 300+ FPS backbuffer writes.
The renderer verifies the live BAR1 instance against the saved mapping state
and fails if the GPU was reset. Shutdown telemetry reports received updates,
actual builds, memoization hits, geometry build time, BAR1 draw time, sequence
drops, and deadline misses.

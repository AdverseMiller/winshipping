#!/bin/zsh
set -euo pipefail

directory=${0:A:h}
binary=$directory/tu104-bar1-overlay
scanner=${BAR1_SCANNER:-$directory/../tu104-scanout-pool.py}
probe=${BAR1_WDDM_PROBE:-$directory/../memflow-live-probe}
state=/run/tu104-bar1-game.state
domain=${BAR1_DOMAIN:-win-gaming}
pid_file=${BAR1_QEMU_PID_FILE:-/run/libvirt/qemu/$domain.pid}
virsh=${BAR1_VIRSH:-$(command -v virsh || true)}
target_process=${BAR1_TARGET_PROCESS:-FortniteClient}

if (( EUID != 0 )); then
  if [[ -x /bin/csu ]]; then
    exec /bin/csu "$0" "$@"
  elif (( $+commands[pkexec] )); then
    exec pkexec "$0" "$@"
  elif (( $+commands[sudo] )); then
    exec sudo "$0" "$@"
  else
    print -u2 "root access is required and csu, pkexec, and sudo are unavailable"
    exit 1
  fi
fi

[[ -x $binary ]] || { print -u2 "renderer is not built: $binary"; exit 1; }
[[ -x $scanner ]] || { print -u2 "hardware scanner is unavailable: $scanner"; exit 1; }
[[ -x $probe ]] || { print -u2 "WDDM ownership probe is unavailable: $probe"; exit 1; }
[[ -x $virsh ]] || { print -u2 "virsh is unavailable; set BAR1_VIRSH"; exit 1; }
[[ -r $pid_file ]] || { print -u2 "$domain PID file is unavailable"; exit 1; }

qemu_pid=$(<$pid_file)
[[ $qemu_pid == <1-> ]] || { print -u2 "invalid QEMU PID"; exit 1; }

# Resolve the live NVIDIA display-function VFIO cdev from QEMU fdinfo. This is
# stable across QEMU fd-number churn and rejects audio/USB functions.
vfio_fd=${BAR1_VFIO_FD:-}
if [[ -z $vfio_fd ]]; then
  vfio_candidates=()
  for info in /proc/$qemu_pid/fdinfo/*; do
    syspath=$(sed -n 's/^vfio-device-syspath: //p' "$info")
    [[ -n $syspath && -r $syspath/vendor && -r $syspath/class ]] || continue
    vendor=$(<$syspath/vendor)
    class=$(<$syspath/class)
    [[ $vendor == 0x10de && $class == 0x03* ]] || continue
    vfio_candidates+=("${info:t}")
  done
  (( ${#vfio_candidates} == 1 )) || {
    print -u2 "expected one NVIDIA display VFIO fd, got: ${vfio_candidates[*]:-(none)}"
    exit 1
  }
  vfio_fd=$vfio_candidates[1]
fi

rect=${BAR1_RECT:-900,450,96,96}
color=${BAR1_COLOR:-0xffff00ff}
hz=${BAR1_HZ:-2000}
duration=${BAR1_DURATION_MS:-0}
render_command=render-sparse
stream_args=()
if [[ ${BAR1_BOX_STREAM:-0} == 1 ]]; then
  render_command=render-boxes
  stream_args=(--rect-stream-fd 0)
fi

initial_state=$("$virsh" --connect qemu:///system domstate "$domain")
[[ $initial_state == running || $initial_state == paused ]] || {
  print -u2 "$domain must be running or paused (state: $initial_state)"
  exit 1
}
suspended_by_us=0
resume_guest() {
  if (( suspended_by_us )); then
    "$virsh" --connect qemu:///system resume "$domain" >/dev/null
    suspended_by_us=0
  fi
}
trap resume_guest EXIT INT TERM HUP
if [[ $initial_state == running ]]; then
  "$virsh" --connect qemu:///system suspend "$domain" >/dev/null
  suspended_by_us=1
fi

# Correlate exact process-owned VidMm allocations to the hardware RAMHT plane.
# Both snapshots are taken while paused, so allocation and scanout state cannot
# race each other.
owner_json=$("$probe" wddm-allocations "$target_process" --json)
owned_values=("${(@f)$(jq -er '.resident_allocations[].resident_address' <<<"$owner_json")}")
(( ${#owned_values} )) || { print -u2 "no resident WDDM allocations for $target_process"; exit 1; }
owner_args=()
for base in "${owned_values[@]}"; do
  owner_args+=(--owned-vram-base "$base")
done

pool_json=$("$scanner" --pid "$qemu_pid" --fd "$vfio_fd" \
  --allow-pramin-switch "${owner_args[@]}")
jq -e '
  . as $root |
  .hardware.schema_version == 1 and
  .hardware.write_capable == true and
  .hardware.validation.display_layout == "structural-unique" and
  .hardware.validation.bar1_instance == "structural-unique" and
  .hardware.page_table.page_2m == 2097152 and
  .mode == "hardware-structural-signatures+wddm-process-ownership" and
  (.windows | length) == 1 and
  (.surface_pool | length) >= 2 and
  .windows[0].bytes_per_pixel == 4 and
  .windows[0].format == "0xd1" and
  .windows[0].surface_offset_bytes == 0 and
  ([.surface_pool[].display_channel] | unique | length) == 1 and
  ([.surface_pool[].allocation_base] | unique | length) == (.surface_pool | length) and
  all(.surface_pool[]; .target == 1 and .kind == 1 and
      .allocation_size >= $root.windows[0].required_bytes)
' <<<"$pool_json" >/dev/null || {
  print -u2 "owned hardware plane failed renderer invariants"
  exit 1
}

surface_size=$(jq -er '.windows[0].required_bytes' <<<"$pool_json")
frame_width=$(jq -er '.windows[0].width' <<<"$pool_json")
frame_height=$(jq -er '.windows[0].height' <<<"$pool_json")
pitch_blocks=$(jq -er '.windows[0].pitch_blocks' <<<"$pool_json")
log2_gobs_y=$(jq -er '.windows[0].log2_gobs_per_block_y' <<<"$pool_json")
surface_values=("${(@f)$(jq -er '.surface_pool[].allocation_base' <<<"$pool_json")}")
surface_args=()
for surface in "${surface_values[@]}"; do
  surface_args+=(--surface "$surface")
done

# Pass the structurally validated transport descriptor to the renderer.  The
# C binary independently queries VFIO BAR sizes and verifies the live BAR1
# hierarchy before it can touch a page-table entry.
hardware_args=(
  --pci-device-id $(jq -er '.hardware.pci.device_id' <<<"$pool_json")
  --chipset $(jq -er '.hardware.chipset' <<<"$pool_json")
  --pbus-bar0-window $(jq -er '.hardware.registers.pbus_bar0_window' <<<"$pool_json")
  --pramin-offset $(jq -er '.hardware.registers.pramin_offset' <<<"$pool_json")
  --pramin-size $(jq -er '.hardware.registers.pramin_size' <<<"$pool_json")
  --bar1-block $(jq -er '.hardware.registers.bar1_block' <<<"$pool_json")
  --bar1-bind-status $(jq -er '.hardware.registers.bar1_bind_status' <<<"$pool_json")
  --mmu-invalidate-pdb $(jq -er '.hardware.registers.mmu_invalidate_pdb' <<<"$pool_json")
  --mmu-invalidate-upper-pdb $(jq -er '.hardware.registers.mmu_invalidate_upper_pdb' <<<"$pool_json")
  --mmu-invalidate $(jq -er '.hardware.registers.mmu_invalidate' <<<"$pool_json")
  --instance-pdb-offset $(jq -er '.hardware.page_table.instance_pdb_offset' <<<"$pool_json")
  --instance-limit-offset $(jq -er '.hardware.page_table.instance_limit_offset' <<<"$pool_json")
  --page-table-levels $(jq -er '.hardware.page_table.levels' <<<"$pool_json")
)

mapping_args=(--pid "$qemu_pid" --vfio-fd "$vfio_fd" --state "$state"
  "${hardware_args[@]}" "${surface_args[@]}" --surface-size "$surface_size")

if [[ -e $state ]]; then
  if ! "$binary" verify-sparse "${mapping_args[@]}"; then
    print -u2 "surface ownership changed; replacing the stale sparse mapping"
    "$binary" restore-sparse "${mapping_args[@]}"
    "$binary" install-sparse "${mapping_args[@]}"
  fi
else
  "$binary" install-sparse "${mapping_args[@]}"
fi

resume_guest
trap - EXIT INT TERM HUP

exec "$binary" "$render_command" "${mapping_args[@]}" \
  --frame-width "$frame_width" --frame-height "$frame_height" \
  --pitch-blocks "$pitch_blocks" --log2-gobs-y "$log2_gobs_y" \
  --rect "$rect" --color "$color" --hz "$hz" --duration-ms "$duration" \
  "${stream_args[@]}" "$@"

#!/usr/bin/env bash
set -euo pipefail

directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
binary="$directory/tu104-bar1-overlay"
scanner=${BAR1_SCANNER:-"$directory/../tu104-scanout-pool.py"}
probe=${BAR1_WDDM_PROBE:-"$directory/../memflow-live-probe"}
domain=${BAR1_DOMAIN:-win11}
state=${BAR1_STATE:-"/run/tu104-bar1-${domain}.state"}
pid_file=${BAR1_QEMU_PID_FILE:-"/run/libvirt/qemu/${domain}.pid"}
virsh=${BAR1_VIRSH:-$(command -v virsh || true)}
target_process=${BAR1_TARGET_PROCESS:-FortniteClient}
display_channel=${BAR1_DISPLAY_CHANNEL:-}

if (( EUID != 0 )); then
  if [[ -x /bin/csu ]]; then
    exec /bin/csu "$0" "$@"
  elif command -v pkexec >/dev/null; then
    exec pkexec "$0" "$@"
  elif command -v sudo >/dev/null; then
    exec sudo "$0" "$@"
  else
    echo "root access is required and csu, pkexec, and sudo are unavailable" >&2
    exit 1
  fi
fi

for dependency in "$binary" "$scanner" "$probe"; do
  [[ -x $dependency ]] || { echo "required overlay tool is unavailable: $dependency" >&2; exit 1; }
done
[[ -x $virsh ]] || { echo "virsh is unavailable; set BAR1_VIRSH" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }
[[ -r $pid_file ]] || { echo "$domain PID file is unavailable" >&2; exit 1; }

qemu_pid=$(<"$pid_file")
[[ $qemu_pid =~ ^[1-9][0-9]*$ ]] || { echo "invalid QEMU PID" >&2; exit 1; }

vfio_fd=${BAR1_VFIO_FD:-}
if [[ -z $vfio_fd ]]; then
  vfio_candidates=()
  for info in /proc/"$qemu_pid"/fdinfo/*; do
    syspath=$(sed -n 's/^vfio-device-syspath: //p' "$info")
    [[ -n $syspath && -r $syspath/vendor && -r $syspath/class ]] || continue
    vendor=$(<"$syspath/vendor")
    class=$(<"$syspath/class")
    [[ $vendor == 0x10de && $class == 0x03* ]] || continue
    vfio_candidates+=("${info##*/}")
  done
  (( ${#vfio_candidates[@]} == 1 )) || {
    echo "expected one NVIDIA display VFIO fd, got: ${vfio_candidates[*]:-(none)}" >&2
    exit 1
  }
  vfio_fd=${vfio_candidates[0]}
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

initial_state=$($virsh --connect qemu:///system domstate "$domain")
[[ $initial_state == running || $initial_state == paused ]] || {
  echo "$domain must be running or paused (state: $initial_state)" >&2
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

owner_json=$("$probe" --target "$domain" wddm-allocations "$target_process" --json)
mapfile -t owned_values < <(python3 -c '
import json, sys
for item in json.load(sys.stdin)["resident_allocations"]:
    print(item["resident_address"])
' <<<"$owner_json")
(( ${#owned_values[@]} )) || { echo "no resident WDDM allocations for $target_process" >&2; exit 1; }
owner_args=()
for base in "${owned_values[@]}"; do
  owner_args+=(--owned-vram-base "$base")
done
channel_args=()
if [[ -n $display_channel ]]; then
  channel_args=(--display-channel "$display_channel")
fi

pool_json=$("$scanner" --pid "$qemu_pid" --fd "$vfio_fd" \
  --allow-pramin-switch "${channel_args[@]}" "${owner_args[@]}")
mapfile -t descriptor < <(python3 -c '
import json, sys
d = json.load(sys.stdin)
h = d["hardware"]
wins = d["windows"]
pool = d["surface_pool"]
assert h["schema_version"] == 1 and h["write_capable"] is True
assert h["validation"]["display_layout"] == "structural-unique"
assert h["validation"]["bar1_instance"] == "structural-unique"
assert h["page_table"]["page_2m"] == 2097152
assert d["mode"] == "hardware-structural-signatures+wddm-process-ownership"
assert len(wins) == 1 and len(pool) >= 2
w = wins[0]
assert w["bytes_per_pixel"] == 4
assert w["format"] in {"0xcf", "0xe6", "0xd5", "0xf9", "0xdf", "0xd1"}
assert w["surface_offset_bytes"] == 0
assert len({item["display_channel"] for item in pool}) == 1
assert len({item["allocation_base"] for item in pool}) == len(pool)
assert all(item["target"] == 1 and item["kind"] == 1 and
           item["allocation_size"] >= w["required_bytes"] for item in pool)
values = [
    w["required_bytes"], w["width"], w["height"], w["pitch_blocks"],
    w["log2_gobs_per_block_y"], h["pci"]["device_id"], h["chipset"],
    h["registers"]["pbus_bar0_window"], h["registers"]["pramin_offset"],
    h["registers"]["pramin_size"], h["registers"]["bar1_block"],
    h["registers"]["bar1_bind_status"], h["registers"]["mmu_invalidate_pdb"],
    h["registers"]["mmu_invalidate_upper_pdb"], h["registers"]["mmu_invalidate"],
    h["page_table"]["instance_pdb_offset"], h["page_table"]["instance_limit_offset"],
    h["page_table"]["levels"],
]
for value in values:
    print(value)
for item in pool:
    print(item["allocation_base"])
' <<<"$pool_json") || { echo "owned hardware plane failed renderer invariants" >&2; exit 1; }
(( ${#descriptor[@]} >= 20 )) || { echo "incomplete hardware descriptor" >&2; exit 1; }

surface_size=${descriptor[0]}
frame_width=${descriptor[1]}
frame_height=${descriptor[2]}
pitch_blocks=${descriptor[3]}
log2_gobs_y=${descriptor[4]}
hardware_args=(
  --pci-device-id "${descriptor[5]}" --chipset "${descriptor[6]}"
  --pbus-bar0-window "${descriptor[7]}" --pramin-offset "${descriptor[8]}"
  --pramin-size "${descriptor[9]}" --bar1-block "${descriptor[10]}"
  --bar1-bind-status "${descriptor[11]}" --mmu-invalidate-pdb "${descriptor[12]}"
  --mmu-invalidate-upper-pdb "${descriptor[13]}" --mmu-invalidate "${descriptor[14]}"
  --instance-pdb-offset "${descriptor[15]}" --instance-limit-offset "${descriptor[16]}"
  --page-table-levels "${descriptor[17]}"
)
surface_args=()
for (( index=18; index<${#descriptor[@]}; ++index )); do
  surface_args+=(--surface "${descriptor[index]}")
done

mapping_args=(--pid "$qemu_pid" --vfio-fd "$vfio_fd" --state "$state"
  "${hardware_args[@]}" "${surface_args[@]}" --surface-size "$surface_size")
if [[ -e $state ]]; then
  if ! "$binary" verify-sparse "${mapping_args[@]}"; then
    echo "surface ownership changed; replacing the stale sparse mapping" >&2
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

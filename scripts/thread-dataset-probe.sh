#!/usr/bin/env bash
# Probe: does the Thread dataset frame actually reach the brain?
#
# The story so far (docs/matter-backlog.md §3): the co-processor (thread-c6) is a
# pure Thread host and lends its network to the brain — the brain asks for the
# active operational dataset (hubproto ThreadDatasetReq 0x62) and the chip answers
# with ThreadDataset 0x63. The chip logs "sent the Thread dataset", the brain
# logs "no dataset in 15000 ms". The frame is written but never shows up, and the
# frame layer drops broken blocks silently, so "lost on the wire" and "never left
# the chip" look the same from outside.
#
# This script answers it in one run. It flashes the two rebuilt images (the chip
# says whether the write went out, the brain counts the blocks it dropped) and
# then asks the brain for the dataset while listening to both boards.
#
# What to look for in the output:
#   "sent the Thread dataset ... written"     the chip put the frame on the wire
#   "rx type=0x63"                            the brain decoded the frame
#   "link dropped frames: ... thread N"       the frame arrived broken (N > 0)
#   "matter: got the Thread dataset ..."      the brain accepted it
#
# On the chip's own boot log (printed right after the flash):
#   "Border Router is up"                     the BR is running
#   "mDNS hostname is 'keystone-br'"          the mDNS name is set (the BR
#                                             publishes its services under it)
#   "Wi-Fi / 802.15.4 coexistence is on"      the two radios share one PHY, and
#                                             the arbiter has to know about both
#   "Guru Meditation" / "WDT"                 the chip died — the log says where
#
# Usage:
#   scripts/thread-dataset-probe.sh [node_id] [pincode] [discriminator]
#
# Defaults are the IKEA TIMMERFLOTTE sensor that was used for the commissioning
# attempts. Flashing keeps NVS (no erase_flash): the Thread network and the
# chip's dataset survive.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NODE_ID="${1:-4660}"
PINCODE="${2:-39441506}"
DISCRIMINATOR="${3:-569}"

# The boards are told apart by their USB MAC, not by the port: the ports swap
# when a board is re-plugged (docs, CLAUDE.md §Status).
BRAIN_MAC="10:51:DB:5E:F8:94"    # ESP32-S3-Zero, the brain
THREAD_MAC="AC:EB:E6:DA:FC:08"   # ESP32-C6, the Thread co-processor
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"

# The tty a USB device ended up on, found by its MAC. The Espressif USB
# JTAG/serial unit puts the chip's MAC into the USB serial-number descriptor,
# and sysfs has it right there — the kernel log (dmesg) is the older way and
# its buffer rotates, so it cannot be relied on.
port_for_mac() {
  local mac="$1" t serial
  for t in /sys/class/tty/ttyACM*; do
    [ -e "$t" ] || continue
    serial="$(cat "$t/device/../serial" 2>/dev/null || true)"
    if [ "${serial^^}" = "${mac^^}" ]; then
      echo "$t" | sed 's#.*/##'
      return 0
    fi
  done
  # Fallback: the kernel log (may have rotated, but better than nothing).
  local busdev
  busdev="$(dmesg 2>/dev/null | grep -B6 "SerialNumber: $mac" \
            | grep -oE 'usb [0-9]+-[0-9]+' | tail -1 | cut -d' ' -f2)"
  [ -n "$busdev" ] || return 1
  dmesg 2>/dev/null | grep -oE "cdc_acm ${busdev}:1\.0: ttyACM[0-9]+" \
    | grep -oE 'ttyACM[0-9]+' | tail -1
}

BRAIN_PORT="${BRAIN_PORT:-$(port_for_mac "$BRAIN_MAC" || true)}"
THREAD_PORT="${THREAD_PORT:-$(port_for_mac "$THREAD_MAC" || true)}"

# port_for_mac returns a bare "ttyACM0"; pyserial expects a path.
[ -n "$BRAIN_PORT" ] && [ "${BRAIN_PORT#/}" = "$BRAIN_PORT" ] && BRAIN_PORT="/dev/$BRAIN_PORT"
[ -n "$THREAD_PORT" ] && [ "${THREAD_PORT#/}" = "$THREAD_PORT" ] && THREAD_PORT="/dev/$THREAD_PORT"

if [ -z "$BRAIN_PORT" ] || [ -z "$THREAD_PORT" ]; then
  echo "could not find the boards by MAC (brain $BRAIN_MAC, thread $THREAD_MAC)." >&2
  echo "check 'dmesg | grep ttyACM' and give the ports explicitly:" >&2
  echo "  BRAIN_PORT=/dev/ttyACM0 THREAD_PORT=/dev/ttyACM1 $0" >&2
  exit 1
fi
echo "brain  = $BRAIN_PORT ($BRAIN_MAC)"
echo "thread = $THREAD_PORT ($THREAD_MAC)"

# The MAC says which board it is; the console prompt says it again, on the board
# itself. Two boards were flashed with each other's firmware once because the
# ports had swapped (CLAUDE.md §Status) — so check the prompt before writing
# any flash.
identify() {  # identify <port> <expected prompt>
  # Opening the S3's USB-Serial-JTAG resets the chip (rst:0x15), and the brain
  # then floods its boot log for several seconds before the prompt appears —
  # short --wait windows miss it. We give the first attempt a long window and
  # fall back to shorter retries.
  local port="$1" want="$2" out
  for wait_s in 6 4 4 4; do
    out="$(python3 "$ROOT/scripts/kcon.py" -p "$port" --wait "$wait_s" -- "" 2>/dev/null || true)"
    case "$out" in
      *"$want"*) return 0 ;;
    esac
    sleep 2
  done
  return 1
}

if ! identify "$BRAIN_PORT" "home>"; then
  echo "the board on $BRAIN_PORT does not answer as the brain (no 'home>' prompt)." >&2
  echo "refusing to flash it." >&2
  exit 1
fi
if ! identify "$THREAD_PORT" "thread>"; then
  echo "the board on $THREAD_PORT does not answer as the thread chip (no 'thread>' prompt)." >&2
  echo "refusing to flash it." >&2
  exit 1
fi

if [ ! -f "$ESPTOOL" ]; then
  echo "esptool not found at $ESPTOOL (set ESPTOOL=...)" >&2
  exit 1
fi

flash() {  # flash <chip> <port> <offsets...>
  local chip="$1" port="$2"; shift 2
  echo "--- flashing $chip on $port"
  python3 "$ESPTOOL" --chip "$chip" -p "$port" -b 460800 write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 80m "$@"
}

# The co-processor: bootloader, partition table, otadata (which slot to boot),
# app (ota_0). NVS (0x10000) and the rest of the flash are left alone — the
# Thread network and its dataset stay.
flash esp32c6 "$THREAD_PORT" \
  0x0     "$ROOT/firmware/thread-c6/build/bootloader/bootloader.bin" \
  0xc000  "$ROOT/firmware/thread-c6/build/partition_table/partition-table.bin" \
  0x1d000 "$ROOT/firmware/thread-c6/build/ota_data_initial.bin" \
  0x20000 "$ROOT/firmware/thread-c6/build/keystone_thread_c6.bin"

# The brain: bootloader, partition table, otadata, app. NVS (0x9000) keeps the
# Wi-Fi credentials, names, rules and schedules.
flash esp32s3 "$BRAIN_PORT" \
  0x0     "$ROOT/firmware/brain-s3/build/bootloader/bootloader.bin" \
  0x8000  "$ROOT/firmware/brain-s3/build/partition_table/partition-table.bin" \
  0x10000 "$ROOT/firmware/brain-s3/build/ota_data_initial.bin" \
  0x20000 "$ROOT/firmware/brain-s3/build/keystone_brain_s3.bin"

OUT="$(mktemp)"
CHIP_OUT="$(mktemp)"
EXTRA="$(mktemp)"
BOOT_OUT="$(mktemp)"
trap 'rm -f "$OUT" "$CHIP_OUT" "$EXTRA" "$BOOT_OUT"' EXIT

# --- the chip's own boot log -------------------------------------------------
# Both boards were just flashed, so they are booting right now. The chip's log
# is what tells us whether the Border Router came up — without it the
# commissioning cannot finish (the device's _matter._tcp never reaches the
# backbone).
echo "--- listening to the thread chip's boot log (25 s)"
timeout 25 python3 "$ROOT/scripts/kcon.py" -p "$THREAD_PORT" --watch >"$BOOT_OUT" 2>&1 || true
grep -E -i 'Border Router|mDNS hostname|coexistence|link-local|Wi-Fi got IP|network is|dataset|Guru|panic|WDT|rst:' "$BOOT_OUT" \
  || tail -25 "$BOOT_OUT"

echo
echo "=================== asking the brain for the dataset ==================="
# The chip's console in the background — we only want its "sent the dataset" line.
# The window has to cover the brain's slow console sequence (each command waits
# 25 s), so it is generous.
( timeout 240 python3 "$ROOT/scripts/kcon.py" -p "$THREAD_PORT" --wait 235 -- "" \
    >"$CHIP_OUT" 2>&1 || true ) &

python3 "$ROOT/scripts/kcon.py" -p "$BRAIN_PORT" --wait 25 -- \
  "log on" \
  "chips" \
  "matter_pair_thread $NODE_ID $PINCODE $DISCRIMINATOR" \
  "chips" >"$OUT" 2>&1 || true

# The commissioning itself runs on the brain's own task and takes minutes; keep
# listening to the brain's console for what it has to say about it.
timeout 120 python3 "$ROOT/scripts/kcon.py" -p "$BRAIN_PORT" --watch >"$EXTRA" 2>&1 || true

wait || true

echo
echo "=================== brain ($BRAIN_PORT) ==================="
grep -E -i 'thread dataset|rx type=0x6[23]|dropped|crc_errors|matter|chip 0x02|Thread' "$OUT" \
  || cat "$OUT"
echo
echo "=================== brain, afterwards (commissioning) ==================="
grep -E -i 'PASE|CASE|attestation|NOC|Thread|commission|pairing|error' "$EXTRA" | head -60 \
  || true
echo
echo "=================== thread chip ($THREAD_PORT) ==================="
grep -E -i 'dataset|thread' "$CHIP_OUT" || cat "$CHIP_OUT"
echo
echo "=================== thread chip: the state of the network ==================="
# The one number that says whether the CASE packet ever entered the mesh:
# "br counters: in_unicast=… out_unicast=…". If in_unicast stays 0, the
# packet never got into the mesh (the brain had no route, or the co-processor
# had no interface into it). If it moves and CASE still fails, the brain's
# source address is the next thing to look at.
timeout 60 python3 "$ROOT/scripts/kcon.py" -p "$THREAD_PORT" --wait 25 -- "thread" 2>&1 \
  | grep -E -i 'role=|ext pan|omr prefix|br counters|br ra|children|ot netif|backbone|dataset ml' || true
echo
echo "full logs: $OUT $EXTRA $CHIP_OUT $BOOT_OUT (kept until this script exits)"

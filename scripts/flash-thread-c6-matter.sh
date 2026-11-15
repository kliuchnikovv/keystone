#!/usr/bin/env bash
# Flash the thread-c6 Matter image (F2 Stage A) and read what the chip says.
#
# The chip is the Thread host of the hub (chip 0x02) and — since the F2 fork —
# it also carries the Matter server stack: the hub itself is an accessory with
# an aggregator endpoint, so Apple Home can add the hub as one device
# (docs/matter-backlog.md § 10).
#
# What this script does:
#   1. finds the board by its USB MAC (the ports swap when a board is re-plugged)
#   2. refuses to write anything until the board answers with its own prompt
#      ("thread>") — two boards were flashed with each other's firmware once
#   3. writes bootloader + partition table + otadata + app, WITHOUT erasing
#      flash: NVS keeps the Thread network and its dataset
#   4. follows the boot log and prints the Matter lines
#   5. asks the chip for its heap numbers (the RAM budget of § 10)
#
# Usage:
#   scripts/flash-thread-c6-matter.sh
#   scripts/flash-thread-c6-matter.sh --size-only     # just report the flash size
#   scripts/flash-thread-c6-matter.sh --port /dev/ttyACM1
#   THREAD_PORT=/dev/ttyACM1 scripts/flash-thread-c6-matter.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREAD_MAC="AC:EB:E6:DA:FC:08"   # ESP32-C6, the Thread/Matter co-processor
BRAIN_MAC="10:51:DB:5E:F8:94"   # ESP32-S3, the brain
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"
BUILD="$ROOT/firmware/thread-c6/build"
SIZE_ONLY=0
CONSOLE_ONLY=0
COREDUMP=0
RESET_ONLY=0
FORCE=0
BEFORE=""
PORT_OVERRIDE=""
WATCH_SECONDS=40
REPEAT=1
RAW=0
BRAIN=0
BRAIN_FLASH=0
CHIP_CMD=0
BRAIN_CMD=()
while [ $# -gt 0 ]; do
  case "$1" in
    --size-only) SIZE_ONLY=1 ;;
    --console-only) CONSOLE_ONLY=1 ;;
    --coredump) COREDUMP=1 ;;
    --reset) RESET_ONLY=1 ;;
    --force) FORCE=1 ;;
    --no-reset) BEFORE="no_reset" ;;
    --raw) RAW=1 ;;
    --brain) BRAIN=1 ;;
    --brain-flash) BRAIN_FLASH=1 ;;
    --chip-cmd) CHIP_CMD=1 ;;
    --seconds) WATCH_SECONDS="$2"; shift ;;
    --repeat) REPEAT="$2"; shift ;;
    --port) PORT_OVERRIDE="$2"; shift ;;
    --) shift; BRAIN_CMD=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

# The Espressif USB JTAG/serial unit puts the chip's MAC into the USB
# serial-number descriptor, and sysfs has it right there.
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
  return 1
}

PORT="${PORT_OVERRIDE:-${THREAD_PORT:-$(port_for_mac "$THREAD_MAC" || true)}}"
[ -n "$PORT" ] && [ "${PORT#/}" = "$PORT" ] && PORT="/dev/$PORT"
if [ -z "$PORT" ]; then
  echo "could not find the thread chip by MAC ($THREAD_MAC); give the port:" >&2
  echo "  THREAD_PORT=/dev/ttyACM1 $0" >&2
  exit 1
fi
echo "thread chip = $PORT ($THREAD_MAC)"

# --brain-flash: write the brain's Matter image (the commissioner side).
# The NVS partition (Wi-Fi, tokens, names) is left alone on purpose — only
# the bootloader, the partition table, the OTA header and the app are written.
if [ "$BRAIN_FLASH" = "1" ]; then
  BPORT="$(port_for_mac "$BRAIN_MAC" || true)"
  if [ -z "$BPORT" ]; then
    echo "could not find the brain by MAC ($BRAIN_MAC)" >&2
    exit 1
  fi
  BPORT="/dev/$BPORT"
  BBUILD="$ROOT/firmware/brain-s3/build"
  echo "brain = $BPORT ($BRAIN_MAC)"
  for f in "$BBUILD/keystone_brain_s3.bin" "$BBUILD/bootloader/bootloader.bin" \
           "$BBUILD/partition_table/partition-table.bin" "$BBUILD/ota_data_initial.bin"; do
    [ -f "$f" ] || { echo "missing $f — build the Matter image first" >&2; exit 1; }
  done
  echo "--- writing the brain's Matter image"
  python3 "$ESPTOOL" --chip esp32s3 -p "$BPORT" -b 460800 write_flash \
    --flash_mode dio --flash_size 4MB --flash_freq 80m \
    0x0     "$BBUILD/bootloader/bootloader.bin" \
    0x8000  "$BBUILD/partition_table/partition-table.bin" \
    0x10000 "$BBUILD/ota_data_initial.bin" \
    0x20000 "$BBUILD/keystone_brain_s3.bin"
  echo
  echo "--- the brain's boot log (30 s)"
  timeout 30 python3 "$ROOT/scripts/kcon.py" -p "$BPORT" --watch 2>&1 | tr -d '\r' | tail -40
  exit 0
fi

# --brain: talk to the brain's console instead (the USB ports swap when a
# board is re-plugged, so the MAC decides). Everything after "--" is the
# command to run, e.g.
#   scripts/flash-thread-c6-matter.sh --brain -- "chips" "wifi_status"
if [ "$CHIP_CMD" = "1" ]; then
  echo "thread chip = $PORT ($THREAD_MAC)"
  timeout "$((WATCH_SECONDS + 20))" python3 "$ROOT/scripts/kcon.py" -p "$PORT" \
    --wait "$WATCH_SECONDS" -- "${BRAIN_CMD[@]}" 2>&1 | tr -d '\r' | tail -40
  exit 0
fi

if [ "$BRAIN" = "1" ]; then
  BPORT="$(port_for_mac "$BRAIN_MAC" || true)"
  if [ -z "$BPORT" ]; then
    echo "could not find the brain by MAC ($BRAIN_MAC)" >&2
    exit 1
  fi
  BPORT="/dev/$BPORT"
  echo "brain = $BPORT ($BRAIN_MAC)"
  if [ "${#BRAIN_CMD[@]}" -eq 0 ]; then
    timeout 30 python3 "$ROOT/scripts/kcon.py" -p "$BPORT" --watch 2>&1 | tr -d '\r' | tail -60
  else
    timeout "$((WATCH_SECONDS + 20))" python3 "$ROOT/scripts/kcon.py" -p "$BPORT" \
      --wait "$WATCH_SECONDS" -- "${BRAIN_CMD[@]}" 2>&1 | tr -d '\r' | tail -80
  fi
  exit 0
fi

if [ ! -f "$ESPTOOL" ]; then
  echo "esptool not found at $ESPTOOL (set ESPTOOL=...)" >&2
  exit 1
fi

# What flash does the module actually have? The Matter image is ~2.1 MB: the
# 4 MB layout gives it two 1.92 MB OTA slots, an 8 MB module gives two 3.44 MB
# ones (the layout of esp-matter's own Border Router example). esptool prints
# the detected size, so ask it before writing anything.
if [ "$CONSOLE_ONLY" != "1" ] && [ "$RESET_ONLY" != "1" ]; then
  echo "--- flash id"
  # --connect-attempts: opening the port on this board resets the chip, the
  # USB device re-enumerates and the first sync attempt dies with "Serial
  # data stream stopped". A retry finds the chip in whatever state it landed
  # in (bootloader with BOOT held, application otherwise).
  python3 "$ESPTOOL" --chip esp32c6 -p "$PORT" ${BEFORE:+--before "$BEFORE"} \
    --after no_reset --connect-attempts 6 \
    flash_id 2>&1 || true
fi

if [ "$SIZE_ONLY" = "1" ]; then
  exit 0
fi

# --coredump: read the crash dump the chip left in flash and decode it. The
# panic that kills the chip cannot print its own backtrace (the handler
# faults while dumping), so the dump in flash is the only trace.
if [ "$COREDUMP" = "1" ]; then
  # The dump is the only trace of that crash, so it is kept next to the
  # firmware (gitignored). It cannot go into the build dir: that one belongs
  # to the container's root and is not writable from here.
  DUMP="$ROOT/firmware/thread-c6/coredump.bin"
  DUMPDIR="$(dirname "$DUMP")"
  echo "--- reading the coredump partition (0x3F0000, 64 KB) into $DUMP"
  # --before no_reset: this board does not react to the DTR/RTS pulses, so
  # esptool's own reset sends the chip into the application — and the chip
  # crashes there, taking the USB link with it mid-read. With the chip held
  # in the ROM bootloader (BOOT+RESET) and no_reset, the read is done over
  # the bootloader's own interface and cannot be interrupted.
  python3 "$ESPTOOL" --chip esp32c6 -p "$PORT" -b 460800 ${BEFORE:+--before "$BEFORE"} \
    --after no_reset --connect-attempts 6 \
    read_flash \
    0x3F0000 0x10000 "$DUMP" 2>&1 | tail -3
  ls -l "$DUMP" || exit 1
  echo "--- first bytes:"
  od -A d -t x1z -N 48 "$DUMP"
  echo "--- decoding"
  # The positional argument of esp-coredump is the *program* ELF; the core
  # file itself goes through --core. Handing the dump over as the positional
  # argument makes the tool parse it as an ELF and fail ("expected \x7fELF").
  docker run --rm -v "$DUMPDIR:/dump" -v "$BUILD:/build" \
    espressif/esp-matter:release-v1.6_idf_v5.5.5 \
    bash -lc 'esp-coredump info_corefile --core /dump/coredump.bin --core-format raw /build/keystone_thread_c6.elf 2>&1 | tail -150'
  exit 0
fi

# --console-only: skip the flash and just follow the chip's log (the board
# crash-loops, and the panic backtrace is only in the raw log).
if [ "$CONSOLE_ONLY" = "1" ]; then
  # The chip reboots on its own now and then; when it does, the USB device
  # re-enumerates and the open file descriptor goes stale — the log after the
  # reboot is invisible to a watcher that holds the old fd. Reopening the port
  # for every round catches the beginning of the next boot, which is where
  # the chip prints why it restarted.
  for round in $(seq 1 "$REPEAT"); do
    echo "--- the chip's log, round $round (${WATCH_SECONDS} s)"
    # The `timeout` kills the watcher on purpose, and its exit code (124)
    # must not end the script: `set -e` plus `pipefail` would take the whole
    # loop down after the first round.
    if [ "$RAW" = "1" ]; then
      timeout "$WATCH_SECONDS" python3 "$ROOT/scripts/kcon.py" -p "$PORT" --watch 2>&1 \
        | tr -d '\r' | tail -70 || true
    else
      timeout "$WATCH_SECONDS" python3 "$ROOT/scripts/kcon.py" -p "$PORT" --watch 2>&1 \
        | tr -d '\r' \
        | grep -E "reset reason|WDT|Guru|panic|rst:|boot:|OpenThread is up|Matter accessory up|Border Router is up|every 30 s" \
        | tail -30 || true
    fi
  done
  exit 0
fi

# The console prompt says again which board this is, on the board itself.
# A chip that crash-loops never answers (and its USB port flaps, so the open
# can block) — hence the timeout, and --force for that case: the MAC check
# above is the one that must never be skipped.
out="$(timeout 15 python3 "$ROOT/scripts/kcon.py" -p "$PORT" --wait 6 -- "" 2>/dev/null || true)"
case "$out" in
  *"thread>"*) : ;;
  *)
    if [ "$FORCE" != "1" ]; then
      echo "the board on $PORT does not answer as the thread chip (no 'thread>' prompt)." >&2
      echo "refusing to flash it (use --force if the board is crash-looping)." >&2
      exit 1
    fi
    echo "note: $PORT did not answer; continuing anyway (--force)"
    ;;
esac

if [ ! -f "$ESPTOOL" ]; then
  echo "esptool not found at $ESPTOOL (set ESPTOOL=...)" >&2
  exit 1
fi

echo "--- flashing the Matter image"
# Refuse to write an image that does not fit its slot: the bootloader would
# not find a valid image and the chip would come up blank.
IMAGE="$BUILD/keystone_thread_c6.bin"
if [ -f "$IMAGE" ]; then
  IMAGE_BYTES=$(wc -c <"$IMAGE")
  # The slot size comes from the partition table the image was built with.
  SLOT_BYTES=$(awk -F, '/^ota_0/ { gsub(/ /, "", $5); print $5 }' \
    "$ROOT/firmware/thread-c6/partitions.csv" 2>/dev/null)
  SLOT_BYTES=$(( SLOT_BYTES ? SLOT_BYTES : 16#1e0000 ))
  if [ "$IMAGE_BYTES" -gt "$SLOT_BYTES" ]; then
    echo "the image is $IMAGE_BYTES bytes, the ota_0 slot holds $SLOT_BYTES — it would not boot." >&2
    echo "refusing to flash it." >&2
    exit 1
  fi
  echo "image $IMAGE_BYTES B fits the $SLOT_BYTES B slot"
fi

# --after hard_reset: on the C6's USB-Serial-JTAG the default "usb_jtag"
# reset leaves the chip sitting in the ROM bootloader (IO0 low) — it never
# runs the application, and the console and the UART stay silent. A plain RTS
# pulse (hard_reset) resets it into the app.
python3 "$ESPTOOL" --chip esp32c6 -p "$PORT" -b 460800 ${BEFORE:+--before "$BEFORE"} \
  --after no_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0     "$BUILD/bootloader/bootloader.bin" \
  0xc000  "$BUILD/partition_table/partition-table.bin" \
  0x1d000 "$BUILD/ota_data_initial.bin" \
  0x20000 "$BUILD/keystone_thread_c6.bin" || {
  # A chip that crash-loops reboots in the middle of the write and takes the
  # USB connection with it ("No serial data received"). The window between two
  # crashes is about as long as the write, so a couple of attempts land.
  for attempt in 2 3; do
    echo "--- write attempt $attempt"
    sleep 3
    if python3 "$ESPTOOL" --chip esp32c6 -p "$PORT" -b 460800 ${BEFORE:+--before "$BEFORE"} \
        --after no_reset write_flash \
        --flash_mode dio --flash_size 4MB --flash_freq 80m \
        0x0     "$BUILD/bootloader/bootloader.bin" \
        0xc000  "$BUILD/partition_table/partition-table.bin" \
        0x1d000 "$BUILD/ota_data_initial.bin" \
        0x20000 "$BUILD/keystone_thread_c6.bin"; then
      break
    fi
  done
}

# --reset: get the chip out of the ROM bootloader and into the application.
# Which line is which differs between boards (on the S3 the DTR *is* EN;
# esptool's USB-JTAG sequence treats DTR as IO0), and getting it wrong
# leaves the chip sitting in the bootloader: no console, no UART traffic,
# boot_id unchanged. So try the combinations and listen — the chip that
# answers is the one that booted.
if [ "$RESET_ONLY" = "1" ]; then
  python3 - "$PORT" <<'PY'
import sys
import time
import serial

port = sys.argv[1]


def pulse(boot_line, reset_line, polarity, settle=1.5):
    """Pulse the reset line, with the boot line held high.

    The C6's USB-Serial-JTAG maps the two modem lines onto IO0 and EN — which
    is which, and which level means "high", differs between boards, so all
    four combinations are tried and the console decides.
    """
    s = serial.Serial(port, 115200, timeout=0.5)
    try:
        if boot_line == "dtr":
            s.dtr = True
        else:
            s.rts = True
        time.sleep(0.05)
        if reset_line == "dtr":
            s.dtr = False if polarity == "normal" else True
        else:
            s.rts = False if polarity == "normal" else True
        time.sleep(0.1)
        if reset_line == "dtr":
            s.dtr = True if polarity == "normal" else False
        else:
            s.rts = True if polarity == "normal" else False
        time.sleep(settle)
    finally:
        s.close()


def listen(seconds=4.0):
    s = serial.Serial(port, 115200, timeout=0.5)
    out = b""
    end = time.time() + seconds
    try:
        while time.time() < end:
            chunk = s.read(4096)
            if chunk:
                out += chunk
    finally:
        s.close()
    return out.decode("utf-8", "replace")


for boot_line in ("dtr", "rts"):
    for reset_line in ("dtr", "rts"):
        if boot_line == reset_line:
            continue
        for polarity in ("normal", "inverted"):
            pulse(boot_line, reset_line, polarity)
            text = listen()
            head = " | ".join(line for line in text.splitlines() if line.strip())[:160]
            print(f"--- boot={boot_line} reset={reset_line} {polarity}: {head or '(silent)'}", flush=True)
            if text.strip():
                print("the chip talks — this is the one", flush=True)
                sys.exit(0)
print("no combination woke the chip; use the BOOT button", flush=True)
PY
  exit 0
fi

echo
echo "--- the chip's boot log (30 s)"
BOOT_OUT="$(mktemp)"
trap 'rm -f "$BOOT_OUT"' EXIT
timeout 30 python3 "$ROOT/scripts/kcon.py" -p "$PORT" --watch >"$BOOT_OUT" 2>&1 || true
# The raw tail, not a filtered view: a panic prints its register dump and
# backtrace right before the reset, and that dump is the whole point.
tr -d '\r' <"$BOOT_OUT" | tail -80

echo
echo "--- heap (the RAM number for docs/matter-backlog.md § 10)"
timeout 30 python3 "$ROOT/scripts/kcon.py" -p "$PORT" --wait 6 -- "heap" 2>&1 | tail -12

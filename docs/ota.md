# OTA: updating the C6 co-processors from the brain

The brain pushes a firmware image to a co-processor over the existing UART link
(hubproto OTA messages 0x50–0x58). No extra wiring. Chosen over an EN/BOOT
flasher in [backlog.md § OTA](backlog.md#ota-updates-for-the-co-processor-c6s);
that path is still the recovery route for a chip that no longer boots.

## Transfer

Stop-and-wait, one request in flight, resent on timeout with the same `seq`.

```
brain                                   chip
OTA_BEGIN(size, sha256)  ───────────▶   erase inactive slot (can take seconds)
                         ◀───────────   OTA_BEGIN_ACK(status, chunk_size)
OTA_CHUNK(offset, data)  ───────────▶   write, update running SHA-256
                         ◀───────────   OTA_CHUNK_ACK(status, next_offset)
        … repeated …
OTA_END                  ───────────▶   check size + SHA-256, validate image,
                                        select it for the next boot
                         ◀───────────   OTA_END_ACK(status)
                                        reboot (after the UART drains)
```

- A chunk the chip already has (lost ack) is confirmed, not written twice. A
  resent END after success is confirmed too. A chunk that skips ahead gets
  `BadOffset` + the offset the chip wants.
- Replies carry the chip's real `chip_id`/`boot_id`: the brain's
  `RadioCoordinator` resyncs (wipes the chip's entities) on any boot_id change.
- Timeouts (sender): BEGIN 30 s (slot erase), chunk 1 s × 5 retries, END 20 s.
  The chip drops a transfer that is silent for 15 s.
- Speed: the link runs at 921600 baud (`EspUartLink::Config::baud`, both ends
  must match, so changing it needs a USB flash of every chip). Measured ~39 KB/s,
  667 KB in 17 s (was ~8.5 KB/s / 78 s at 115200); thread-c6's 1.68 MB took 194 s
  at 115200. Now stop-and-wait latency (flash write + task wakeups) dominates; a
  multi-chunk window would be the next step. The UART RX loop must not ask `uart_read_bytes` for a full buffer: it
  only returns early on timeout, which cost ~20 ms per short frame (acks) and
  made the same transfer take 111 s.

Code: `libhubproto` (codecs), `core/ota_sender.hpp` (brain),
`core/ota_receiver.hpp` + `OtaSink` (chip), `firmware/components/esp_ota_sink`
(ESP-IDF sink).

## Integrity and rollback

- The SHA-256 comes from whoever starts the update, out of band (not from the
  same server as the image). The chip checks size, SHA-256 and the ESP image
  header/checksum before it selects the new slot.
- The bootloader runs with app rollback enabled: a new image boots
  "pending verify" and is reverted at the next reset **unless** it confirms
  itself. `fw::OtaConfirmGate` confirms only after the brain's QUERY arrived
  (link works) **and** the app has been up 30 s. Confirming earlier (we first
  did it inside the QUERY handler) makes any crash right after it unrecoverable:
  the image is already valid, so the chip crash-loops instead of rolling back.
- Hardware-verified on zigbee-c6: a good image is confirmed at ~30 s; an image
  that aborts 3 s after boot is rolled back by the bootloader on its own and the
  chip comes back on the old slot; a wrong SHA-256 is refused with the running
  image untouched; non-image data is refused at the first chunk (header check).
- A successful *transfer* does not mean the new image stayed: if it rolls back,
  the brain sees the chip's HELLO twice (two boot_id changes). `core::OtaVerifier`
  asks the chip afterwards and reports VERIFIED / ROLLED BACK (see the last
  section).
- Not covered yet: image *signing*. Anyone who can start an OTA with a matching
  SHA-256 can flash the chip. Secure boot / signed images are a later step.

## One-time USB flash (partition table change)

OTA cannot change the partition table. `zigbee-c6` used a single `factory` slot;
it now has `ota_0`/`ota_1`, so it must be flashed once over USB with the new
table. After that, updates can go over the link.

`zb_storage`/`zb_fct` stay at their old offsets (0x140000/0x144000) so the
joined Zigbee network survives. Flashing does not erase them as long as you do
not run `erase_flash`. Back them up first anyway:

```
esptool.py -p /dev/ttyACM0 read_flash 0x140000 0x5000 zb_backup.bin
esptool.py -p /dev/ttyACM0 read_flash 0x8000 0x1000 old_ptable.bin   # confirm layout
```

PlatformIO only honours `partitions.csv` when `board_build.partitions` is set
(it is, in `platformio.ini`).

## Usage

```
ota <zigbee|thread> <http(s)://url> <sha256-hex>   start (HTTPS uses the cert bundle)
ota status                                         progress / last result
ota abort
```

The server must send `Content-Length` (no chunked transfer). Serve an image from
the Pi with `python3 -m http.server`. The console line limit is 384 chars.

## Did the new image stay? (OTA_INFO, `core::OtaVerifier`)

"Image accepted" only means the chip wrote and checked it. A bad image is reverted
by the bootloader, and the chip then comes back on the old firmware — which looks
like a successful update. So after every transfer the brain asks the chip which
image it runs (hubproto 0x57/0x58): running slot, whether it is confirmed, and the
slot + sha256 of the last image the chip accepted over OTA (kept in NVS namespace
`ks_ota`). After the chip has rebooted (boot_id changed) the verdict is:

| verdict | meaning |
|---|---|
| Verified | runs the slot + sha we sent and it is confirmed |
| RolledBack | runs something else: the bootloader reverted the new image |
| RunningUnconfirmed | new image runs but is still pending verify at the 2 min deadline |
| NoRecord | the chip has no OTA record (the update was done by firmware older than OTA_INFO) — cannot tell |
| NoAnswer | no usable reply in time |

It shows in the log and in `ota status`. Hardware-verified on zigbee-c6 with a test
image (`-DKS_TEST_CRASH`, aborts 8 s after boot): `FLAPPING` + `ROLLED BACK`.

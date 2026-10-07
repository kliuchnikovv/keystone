# OTA: updating the C6 co-processors from the brain

The brain pushes a firmware image to a co-processor over the existing UART link
(hubproto OTA messages 0x50–0x56). No extra wiring. Chosen over an EN/BOOT
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
- Speed: ~11 KB/s at 115200 baud, so a 650 KB image takes roughly 1–1.5 min.

Code: `libhubproto` (codecs), `core/ota_sender.hpp` (brain),
`core/ota_receiver.hpp` + `OtaSink` (chip), `firmware/components/esp_ota_sink`
(ESP-IDF sink).

## Integrity and rollback

- The SHA-256 comes from whoever starts the update, out of band (not from the
  same server as the image). The chip checks size, SHA-256 and the ESP image
  header/checksum before it selects the new slot.
- The bootloader runs with app rollback enabled: a new image boots
  "pending verify" and is reverted at the next reset **unless** it confirms
  itself. The chip confirms when the brain sends its first QUERY, which proves
  the UART link and the main loop work on the new image
  (`fw::ota_confirm_running_image()`).
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

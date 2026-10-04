# Zigbee backlog — what a full Zigbee coordinator needs

State today (`main`, commit `6ad8bd2`): the C6 coordinator forms a network on
the on-chip 802.15.4 radio, persists joined devices to NVS (boot-time ZDO
re-discovery for short-address changes), bridges On/Off switch, power metering
(ActivePower W, CumulativeEnergy Wh), and energy (Metering cluster) as
`switch` entities with attrs `on_off`, `power_w`, `energy_wh` to the brain,
and answers COMMANDs with honest CmdAcks. Verified hardware: IKEA INSPELNING
plug, all three attribute families flowing end-to-end.

What's missing to call this a real coordinator, ordered by how much it enables
in the home:

## 1. More device types (ZCL clusters → SPEC §6.1 device_types)

Each is a small mapping addition on the C6, plus (for genuinely new value
kinds) a new `attr_id` + `value_type` in SPEC §6.1. Rough priority for a
typical apartment:

- **Occupancy** (cluster `0x0406`) → `device_type=occupancy`, attr
  `occupied(0x50) bool`. Already in the SPEC; unlocks the classic
  motion→light rules.
- **IAS Zone** (cluster `0x0500`) → contact sensors (door/window),
  motion (an alternative to 0x0406), smoke, water leak. IAS has its own
  little enrollment dance (Zone Enroll Response) — bit fiddlier than
  regular clusters.
- **Level Control** (cluster `0x0008`) → `device_type=light` attr
  `brightness(0x02) u8`. Pairs with On/Off for dimmable bulbs. Already the
  same plug's cluster we ignore today.
- **Color Control** (cluster `0x0300`) → light attrs `color_xy(0x03) xy_color`
  and `color_temp(0x04) u16`. Full color bulbs.
- **Battery** (cluster `0x0001`, PowerConfiguration) → attr `battery(0x1F) u8`
  on any battery-powered device. Low-battery alerts.
- **Temperature/Humidity/Illuminance** (clusters `0x0402/0x0405/0x0400`) →
  `device_type=sensor` attrs `temperature(0x10) i16`, `humidity(0x11) u16`,
  `illuminance(0x12) u16`. All scaled ints per SPEC §6.2.
- **Window Covering** (cluster `0x0102`) → `device_type=cover` attrs
  `position(0x20) u8`, `state(0x21) enum8`. Shades/blinds.
- **Door Lock** (cluster `0x0101`) → `device_type=lock` attr
  `locked(0x30) bool`.
- **Thermostat** (cluster `0x0201`) → `device_type=climate` attrs
  `setpoint(0x40) i16`, `mode(0x41) enum8`, `current_temp(0x10) i16`.
- **Generic Switch / Multistate Input** (`0x0007`, `0x0012`) →
  `device_type=button` attr `event(0x60) enum8` (single/double/long press).
  Physical scene switches.

## 2. Metering (the plug's other clusters) ✅ done (Stage 2c + 2d)

- ✅ **Electrical Measurement** cluster `0x0b04` — `ActivePower`, scaled by
  `ACPowerMultiplier/Divisor` → `power_w(0x13) U16` in W (Stage 2c).
- ✅ **Metering** cluster `0x0702` — `CurrentSummationDelivered`, scaled by
  `Multiplier/Divisor` (UINT24, LE) → `energy_wh(0x14) U32` in Wh (Stage 2d).
- Divisors read once at join time, cached per device. Brain sees clean units.
- See also **stateful helpers** in [brain-backlog](brain-backlog.md).
- Missing: `RMSVoltage`, `RMSCurrent` — add as `voltage_v` / `current_ma` if
  a device surfaces them.

## 3. Multi-endpoint devices

Today we only map endpoint 1. A power strip with two outlets, a bulb with a
separate color-temperature channel, a switch with three buttons — all
multi-endpoint. Needs the switch table keyed on `(short_addr, endpoint)` (it
already is) and DECLARE-per-endpoint from discovery. Small change, big
compatibility win.

## 4. Groups & Scenes

Clusters `0x0004` (Groups) and `0x0005` (Scenes) let a rule flip a dozen
bulbs synchronously without a dozen individual COMMANDs. The coordinator
can also snapshot/restore scenes on the devices themselves. Probably a
second-wave feature once basic device coverage is there.

## 5. OTA (Over-The-Air firmware for devices)

Cluster `0x0019` (client on devices, server on coordinator). The C6 hosts
firmware images (from SD or from the brain over UART) and pushes them to
target devices. Slow-burn feature but real Zigbee networks need it — IKEA
regularly ships bulb firmware updates.

## 6. Network / lifecycle management

Console commands on the brain, forwarded to the C6, to run the network
day-to-day:

- `pair [sec]` — reopen the join window for N seconds without a reboot.
- `unpair <entity>` — send Leave to a device and drop it from the registry
  and NVS.
- `identify <entity>` — flash the device's LED so we can find it physically.
- `chan <N>` — move the whole PAN to a new channel (needs a coordinated
  channel change or forces a re-join).
- `dump` — export the routing table / neighbor table / RSSI/LQI to plug
  into troubleshooting.
- Touchlink (`0x1000`) as an alternative join for devices that support it
  (some IKEA remotes only pair this way).

## 7. Zigbee Green Power (ZGP)

Cluster `0x0021` on endpoint 242, seen on almost every real device today —
it's the protocol for battery-less pushbuttons and similar. Own commissioning
model, own frame format. Ignore for MVP, plan for it before shipping.

## 8. Manufacturer-specific clusters

`0xfc7c`/`0xfc85` on the IKEA plug carry the vendor's own knobs (e.g. LED
brightness on the housing, plug lock). Purely device-specific; case-by-case
when a device needs one and there's no standard cluster equivalent.

## 9. Robustness & diagnostics

- **Radio counters** per device (LQI, RSSI, retries) surfaced through the
  brain's `chips` — see also chip-health in [../backlog.md](backlog.md).
- **Neighbor / router topology** dump.
- **Frequent-flapper detection** — a device that keeps re-joining is a
  battery or signal problem; flag it.
- **Coexistence tuning** with WiFi + Thread on the neighboring C6 (SPEC §10).

## Order-of-work suggestion

1. ✅ Stage 2c (Electrical Measurement — power_w) — done.
2. ✅ Stage 2d (Metering cluster — energy_wh) — done.
3. First **sensor** (occupancy or contact) — enables the first real rule.
4. Multi-endpoint — cheap and unlocks many devices.
5. `pair` / `unpair` / `identify` — makes the coordinator usable day-to-day.
6. IAS Zone family — a lot of practical sensors live here.
7. Level + Color — first real light bulb.
8. Everything else as devices appear.

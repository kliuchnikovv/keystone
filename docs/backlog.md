# Backlog — open questions and future work

Ideas we haven't shaped into a phase yet. Not TODOs on a phase — those live in
[CLAUDE.md § Status](../CLAUDE.md). This file is looser: things worth deciding
before they become urgent.

## Use idle memory on the radio co-processors

Each C6 has ~512 KB SRAM + 4 MB flash. Zigbee/Thread stacks + our translator
use only a fraction (thread-c6 Stage A: 217 KB image, 83% partition free).
The remaining RAM/flash on the two C6s is idle and could carry work that
today sits on the brain — or is missing altogether. Question: **which work
belongs there, without turning the co-processors into second brains?**

Design invariant to preserve (SPEC §3): the *brain* owns entity state and
rules; the co-processors own their radio and a translation layer only. So the
right kind of work to push down is **local-to-that-radio, non-policy** work.
Candidates (roughly in order of value):

- **Local event buffer / send queue.** If the UART link to the brain drops
  (brain reboot, cable), currently STATE/CmdAck are lost. A small ring buffer
  on the C6 (say 128–256 events) would replay on link resume. Rescues short
  outages without any brain change.
- **Device-side attribute cache.** Cache last-known values per Zigbee/Thread
  device so `get` doesn't need a fresh ZCL read every time; the brain already
  has its own cache, but this lets the co-processor answer local diagnostics
  and short-circuit its own periodic reads.
- **Debounce / de-duplication of noisy sensor reports.** Occupancy, contact
  and metering clusters can spam; the C6 can coalesce reports before they
  reach the wire (e.g. no STATE if value unchanged, or rate-limited).
- **Local scaling / unit conversion.** Electrical Measurement returns raw
  int16 × ACPowerDivisor/Multiplier. Reading those divisors is one-shot per
  device; the C6 can pre-scale into the SPEC §6.1 unit before sending STATE,
  so the brain's registry gets clean values.
- **Persistent device metadata cache.** Manufacturer/model, cluster list,
  attribute reporting config — cache in NVS so re-discovery after reboot is
  faster and (partly) works offline from the Zigbee network.
- **Time-series for metering.** Keep the last N power/energy samples in a
  compact circular buffer, so a `power 0x100 last 5m` query gets answered
  without spamming the plug at request time.
- **Compression on the UART wire.** hubproto is COBS+CRC; adding lightweight
  delta encoding for STATE bursts (very small on our scale) could help under
  storms, but only worth doing if we see them.

Non-candidates (belong on the brain, not the co-processor):
- Cross-device rules ("motion in hall → light in kitchen") — brain is the
  single place these converge (SPEC §7 RuleEngine).
- Matter fabric state — Matter itself lives on chip 0x02 (Thread), but the
  bridge model that ties Matter accessories to registry entities is brain.
- Any policy / user-facing configuration.

Decision needed later: pick 1–2 of the above once a concrete pain point shows
up (link outage log, sensor spam, metering scaling), not now on speculation.

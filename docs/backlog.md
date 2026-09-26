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

## Recovery after a crash or reboot — desired vs. actual reconciliation

Today the brain is purely **reactive**: it stores the last-reported value of
each attribute and runs rules on state changes. There is no notion of
*desired* state. So when the brain (or a co-processor, or the whole hub) is
down and comes back, we don't know what *should* have happened during the gap
and can't correct for it. Examples:

- A `in 3600 off 0x100` timer was set at 21:00, brain rebooted at 21:30 —
  the timer is lost; the plug stays on forever.
- A rule "at sunset, close the covers" fires at 20:47; brain was down; at
  boot we don't know the covers should be closed now.
- The plug was physically toggled by the button while the brain was down; on
  return the brain thinks it's still off.

Question: **give each entity a desired-state alongside actual-state, and
reconcile at boot / on link recovery?**

Shape of the answer (rough):

- **Desired state per attribute**, stored in the registry beside actual. Set
  by rules or by explicit commands. Persisted (NVS on brain; or on the
  co-processor for its own entities). Missing = "don't care", so we don't
  fight physical toggles unless something set a desired.
- **Reconciler loop** on the brain: for each entity/attribute with a desired,
  if actual != desired (and actual is known), send a corrective COMMAND. This
  is the CommandDispatcher's retry logic generalized — today it retries a
  single send; a reconciler retries the *goal*.
- **Boot audit**: on start, read actual for every entity (already partly done
  in the C6 restore path via read_device_state), compare to persisted desired,
  emit corrective COMMANDs. Log a `reconcile` event so it's visible what was
  fixed.
- **Persistent scheduled actions**: today `in <sec> …` timers live in RAM;
  move them to NVS with an absolute wall-clock deadline. On boot, drop the
  expired ones by policy (some make sense to still apply — "turn off at 22:00"
  fired at 22:03 is still fine; "briefly flash the light" is stale). A per-
  action `after_deadline: apply | drop | ask` decides.
- **Gap markers** on actual state: after a reboot / long link outage, mark
  attributes as *stale until confirmed*. Rules that care about freshness
  (edge-triggered on a toggle) can then skip; rules that just want current
  value wait for the next read/report.
- **What we *cannot* recover**: transient events with no lasting state (a
  button press, a motion pulse that already ended). Those we drop honestly
  and note the gap in the recorder log (Phase 7) rather than fake them.

The invariant to keep: desired state is authoritative *only if something set
it*. We never fabricate a desired from an actual — otherwise a physical human
toggle would be silently reverted, which is the classic worst behavior of
smart-home hubs.

Depends on: persistent timer table (small); a `desired` field on AttrState
(small); a reconciler tick in the brain loop (small); Phase 7 recorder for the
gap-annotation part (later).

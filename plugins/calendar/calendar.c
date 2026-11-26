/* calendar — a Keystone plugin that exposes a calendar to the rule engine.
 *
 * This is a LOGIC plugin: it does not model a physical thing. It projects
 * an external data source (the user's calendar) into the hub as a sensor
 * entity, so a rule can react to meetings and events the same way it
 * reacts to motion or temperature:
 *
 *   rule add office_calendar imminent == on then lamp_office on_off on
 *   rule add office_calendar busy == on then speaker_office volume 10
 *
 * It declares ONE Sensor entity with three read-only, reportable attributes:
 *
 *   in_minutes (u16)  minutes until the next event starts; 0 means an event
 *                     is happening now; 0xFFFF means no event is known.
 *   busy       (bool) true while an event is in progress.
 *   imminent   (bool) true when in_minutes <= IMMINENT_MIN, false otherwise.
 *                     This is the attribute rules most often want — it's a
 *                     rising edge the rule engine can latch on.
 *
 * The attribute ids are PLUGIN-PRIVATE (0x80+). The hub's shared attribute
 * table (hubproto::attr, SPEC §6.1) is for things real devices speak;
 * plugin-specific attributes live above 0x7F so they never collide.
 *
 * The device type is KS_DEVICE_SENSOR (0x02). A calendar is read-only, it
 * reports, and that's what a sensor is on the wire. The UI will show it
 * under "Sensors" with unknown attribute names — that's fine for now;
 * when the app learns about plugin-side attribute metadata, this is where
 * it hooks in.
 *
 * ---------------------------------------------------------------------
 * WHAT IS STUBBED, AND WHY
 * ---------------------------------------------------------------------
 *
 * The SDK today has no HTTP, no KV and no clock. So two things that a real
 * calendar plugin would do are faked:
 *
 *   1. FETCHING EVENTS. Events are a static array in this file. A real
 *      plugin would do ks_http_get() against a CalDAV or .ics URL (from
 *      the manifest's `allow` or from KV where the app wrote them) in
 *      on_start and in a slow heartbeat, parse it, and refill the table.
 *
 *   2. KNOWING THE TIME. Instead of a wall clock, the plugin counts its
 *      own ticks: a 60 s repeating timer advances g_now_min, and event
 *      offsets are in minutes "from plugin start". So Event A at
 *      {start=2, dur=15} fires 2 minutes after this plugin loads. That is
 *      good enough to prove the shape — rules fire, imminent latches, busy
 *      flips — but it is not a real calendar. A real plugin would call
 *      ks_time_now() (not yet in the SDK) and events would carry absolute
 *      civil time.
 *
 * Every TODO below marks where the real implementation hooks in.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "keystone.h"

/* ----- plugin-private attribute ids (above the shared table at 0x7F) ----- */
#define ATTR_IN_MINUTES 0x80
#define ATTR_BUSY       0x81
#define ATTR_IMMINENT   0x82

/* Timer handle. */
#define TIMER_TICK 1

/* One minute between recalculations — a rule that reacts "10 min before"
 * does not need finer resolution than a minute, and a slow heartbeat means
 * the plugin burns essentially no budget. */
#define TICK_MS 60000u

/* "imminent" is a 10-minute warning. The one knob a user of this plugin
 * would want to tweak — a real plugin would read it from KV. */
#define IMMINENT_MIN 10u

/* The sentinel "no event known". 0xFFFF minutes is ~45 days — well past
 * anything a 24-hour calendar would carry. */
#define NO_EVENT 0xFFFFu

/* ----- the (fake) event table --------------------------------------------
 *
 * TODO(http): replace with events refilled from a .ics / CalDAV URL.
 * TODO(time): offsets are "minutes since plugin start" only because the
 *             plugin has no clock. With ks_time_now(), these become
 *             absolute civil-time timestamps and g_now_min goes away. */
typedef struct {
  unsigned short start_min;    /* minutes since plugin start */
  unsigned short duration_min;
} Event;

static const Event g_events[] = {
    {  2, 15},   /* "stand-up" at +2 min, 15 min long */
    { 20,  5},   /* "coffee"   at +20 min, 5 min */
    { 90, 60},   /* "focus"    at +1h30, 1 h */
    {240, 30},   /* "review"   at +4h, 30 min */
};
static const unsigned g_event_count = sizeof(g_events) / sizeof(g_events[0]);

/* ----- live state ---------------------------------------------------------- */

static unsigned g_entity = 0;

/* Minutes elapsed since on_start. Advanced by the 60 s heartbeat. */
static unsigned g_now_min = 0;

/* What we last published, so we don't flood the hub with duplicates.
 * The hub de-duplicates STATE itself, but a plugin that is tidy on the
 * wire is cheaper to debug. */
static unsigned g_last_in = NO_EVENT + 1u;  /* impossible → first tick publishes */
static int      g_last_busy = -1;
static int      g_last_imminent = -1;

/* ----- helpers ------------------------------------------------------------- */

/* Compute in_minutes/busy from the table. in_minutes==0 implies busy. */
static void compute(unsigned* in_min_out, int* busy_out) {
  unsigned best = NO_EVENT;
  int      busy = 0;
  for (unsigned i = 0; i < g_event_count; ++i) {
    unsigned start = g_events[i].start_min;
    unsigned end   = start + g_events[i].duration_min;
    if (g_now_min >= start && g_now_min < end) {
      busy = 1;
      best = 0;
      break;      /* an event in progress wins; next one doesn't matter */
    }
    if (start > g_now_min) {
      unsigned delta = start - g_now_min;
      if (delta < best) best = delta;
    }
  }
  *in_min_out = best;
  *busy_out   = busy;
}

static void publish(unsigned in_min, int busy, int imminent) {
  unsigned char buf[4];

  if (in_min != g_last_in) {
    ks_put_u16(buf, in_min);
    ks_state_publish(g_entity, ATTR_IN_MINUTES, buf, 2);
    g_last_in = in_min;
  }
  if (busy != g_last_busy) {
    buf[0] = (unsigned char)(busy ? 1 : 0);
    ks_state_publish(g_entity, ATTR_BUSY, buf, 1);
    g_last_busy = busy;
  }
  if (imminent != g_last_imminent) {
    buf[0] = (unsigned char)(imminent ? 1 : 0);
    ks_state_publish(g_entity, ATTR_IMMINENT, buf, 1);
    g_last_imminent = imminent;
  }
}

/* ----- plugin entry points ------------------------------------------------- */

unsigned ks_plugin_abi_version(void) { return KS_ABI_VERSION; }

void on_start(void) {
  static const unsigned char attr_ids[3]    = {ATTR_IN_MINUTES,
                                               ATTR_BUSY,
                                               ATTR_IMMINENT};
  static const unsigned char value_types[3] = {KS_VALUE_U16,
                                               KS_VALUE_BOOL,
                                               KS_VALUE_BOOL};
  static const unsigned char flags[3]       = {
      KS_FLAG_READABLE | KS_FLAG_REPORTABLE,
      KS_FLAG_READABLE | KS_FLAG_REPORTABLE,
      KS_FLAG_READABLE | KS_FLAG_REPORTABLE};
  unsigned char desc[KS_DECLARE_SIZE(3)];

  ks_build_declare(desc, KS_DEVICE_SENSOR, attr_ids, value_types, flags, 3);
  g_entity = ks_entity_declare(desc, KS_DECLARE_SIZE(3));
  if (g_entity == 0) {
    static const char err[] = "calendar: entity_declare refused";
    ks_log((const unsigned char*)err, ks_strlen(err));
    return;
  }

  /* TODO(http): fetch the first page of events here, before the first tick,
   * so on-boot state is correct instead of "event in 2 min" from the stub. */

  static const char hello[] = "calendar: loaded, projecting events as a Sensor";
  ks_log((const unsigned char*)hello, ks_strlen(hello));

  /* Push an initial STATE immediately so the app does not see "—" for 60 s,
   * then start the tick. */
  on_timer(TIMER_TICK);
  ks_timer_set_repeating(TICK_MS, TIMER_TICK);
}

void on_timer(unsigned id) {
  (void)id;
  if (g_entity == 0) return;

  unsigned in_min;
  int      busy;
  compute(&in_min, &busy);

  /* imminent is the one latched attribute: true at <= IMMINENT_MIN or
   * while busy, false otherwise. It is derived, not stored — every tick
   * recomputes it from in_min. */
  int imminent = (in_min <= IMMINENT_MIN) ? 1 : 0;

  publish(in_min, busy, imminent);

  /* Advance AFTER publishing, so the first tick after on_start sees
   * g_now_min == 0 and the imminent + busy edges land at the right
   * boundaries. */
  if (g_now_min < 0xFFFFu) g_now_min += 1u;
}

void on_stop(void) {
  static const char bye[] = "calendar: stopping";
  ks_log((const unsigned char*)bye, ks_strlen(bye));
}

/* smart-plug — a virtual metered outlet.
 *
 * It declares ONE Switch entity with three attributes:
 *
 *   on_off     (bool, R/W/reportable, SPEC §6.1 kOnOff      0x01)
 *   power_w    (u16, R/reportable,                kPowerW   0x13)
 *   energy_wh  (u32, R/reportable,                kEnergyWh 0x14)
 *
 * The plug's "load" draws ~45..55 W when it is on and 0 W when it is off.
 * The energy counter only grows while on_off is true — just like a real
 * metered outlet.
 *
 * Idiomatic bits a new plugin author should steal:
 *
 *   - ONE entity, THREE attributes of three different types (bool / u16 /
 *     u32). The Switch's writable attribute is on_off; the two meters are
 *     read-only.
 *   - on_command is a MEANING-CHANGER, not an echo: it flips g_on, and the
 *     heartbeat decides what power_w and energy_wh look like now.
 *   - A plugin has no clock — ks_timer_set_repeating is the clock. Keep the
 *     tick period a named constant, and accumulate quantities in the units
 *     the tick is natural in (watt-seconds here), then convert when you
 *     publish. A Wh is only emitted when enough watt-seconds have piled up;
 *     the remainder stays inside the plugin, so the long-run average is
 *     exact instead of drifting by the integer truncation every tick.
 *   - on_stop exists for completeness: a well-behaved plugin says goodbye.
 *     The hub calls it whether the plugin is being unloaded cleanly or
 *     after a fault.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "keystone.h"

/* Shared attribute ids (hubproto::attr). */
#define ATTR_ON_OFF    0x01
#define ATTR_POWER_W   0x13
#define ATTR_ENERGY_WH 0x14

/* Timer handles the plugin owns. Small integers, no collisions. */
#define TIMER_HEARTBEAT 1

/* 10 s between reports. Slower = quieter log and app, faster = snappier
 * rules; 10 s is a good compromise and matches the climate-sensor example. */
#define HEARTBEAT_MS 10000u
#define HEARTBEAT_S  10u

static unsigned g_entity = 0;         /* 0 until on_start succeeds */
static unsigned g_rng    = 0xDEADBEEFu;
static int      g_on     = 0;         /* current on_off state */
static unsigned g_power_w = 0;        /* last published W */
static unsigned g_energy_wh = 0;      /* total Wh since on_start */

/* The leftover watt-seconds that have not yet become a Wh. Carrying it
 * between ticks means the long-run average is exact — a plain
 * `energy_wh += power_w * tick_s / 3600` would truncate every tick and
 * drift. */
static unsigned g_energy_ws_remainder = 0;

static unsigned next_rand(void) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return g_rng;
}

static void publish_all(void) {
  unsigned char buf[4];

  buf[0] = (unsigned char)(g_on ? 1 : 0);
  ks_state_publish(g_entity, ATTR_ON_OFF, buf, 1);

  ks_put_u16(buf, g_power_w);
  ks_state_publish(g_entity, ATTR_POWER_W, buf, 2);

  ks_put_u32(buf, g_energy_wh);
  ks_state_publish(g_entity, ATTR_ENERGY_WH, buf, 4);
}

unsigned ks_plugin_abi_version(void) { return KS_ABI_VERSION; }

void on_start(void) {
  static const unsigned char attr_ids[3]    = {ATTR_ON_OFF,
                                               ATTR_POWER_W,
                                               ATTR_ENERGY_WH};
  static const unsigned char value_types[3] = {KS_VALUE_BOOL,
                                               KS_VALUE_U16,
                                               KS_VALUE_U32};
  static const unsigned char flags[3]       = {
      KS_FLAG_READABLE | KS_FLAG_WRITABLE | KS_FLAG_REPORTABLE,
      KS_FLAG_READABLE | KS_FLAG_REPORTABLE,
      KS_FLAG_READABLE | KS_FLAG_REPORTABLE};
  unsigned char desc[KS_DECLARE_SIZE(3)];

  ks_build_declare(desc, KS_DEVICE_SWITCH, attr_ids, value_types, flags, 3);
  g_entity = ks_entity_declare(desc, KS_DECLARE_SIZE(3));
  if (g_entity == 0) {
    static const char err[] = "smart-plug: entity_declare refused";
    ks_log((const unsigned char*)err, ks_strlen(err));
    return;
  }

  static const char hello[] = "smart-plug: off, meter at 0 Wh";
  ks_log((const unsigned char*)hello, ks_strlen(hello));

  /* Publish once right away so the app does not show "—" for 10 s. */
  publish_all();
  ks_timer_set_repeating(HEARTBEAT_MS, TIMER_HEARTBEAT);
}

void on_command(unsigned entity, unsigned attr, const unsigned char* value,
                unsigned value_len, unsigned seq) {
  (void)seq;
  if (g_entity == 0 || entity != g_entity) return;
  if (attr != ATTR_ON_OFF) return;      /* only on_off is writable */
  if (value_len < 1) return;

  int want_on = value[0] != 0;
  if (want_on == g_on) return;          /* idempotent */

  g_on = want_on;

  /* Power follows the switch instantly. The next heartbeat updates Wh. */
  g_power_w = g_on ? 50u : 0u;
  g_energy_ws_remainder = 0;            /* restart the accumulation window */

  publish_all();
}

void on_timer(unsigned id) {
  (void)id;
  if (g_entity == 0) return;

  if (g_on) {
    /* 45..55 W — a modestly noisy appliance. */
    g_power_w = 45u + (next_rand() % 11u);

    /* watt-seconds carried over since the last tick. */
    g_energy_ws_remainder += g_power_w * HEARTBEAT_S;
    while (g_energy_ws_remainder >= 3600u) {
      g_energy_wh += 1u;
      g_energy_ws_remainder -= 3600u;
    }
  } else {
    g_power_w = 0u;
  }

  publish_all();
}

void on_stop(void) {
  static const char bye[] = "smart-plug: stopping";
  ks_log((const unsigned char*)bye, ks_strlen(bye));
}

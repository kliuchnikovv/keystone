/* climate-sensor — a read-only Keystone plugin.
 *
 * It behaves like a battery-powered climate sensor on the wire:
 *
 *   on_start declares a Sensor with three REPORTABLE attributes —
 *     - temperature (i16, 0.1 °C, SPEC §6.1 kTemperature 0x10)
 *     - humidity    (u16, 0.1 % RH,                kHumidity    0x11)
 *     - battery     (u8,  %,                       kBattery     0x1F)
 *   and arms one repeating timer: every 10 s on_timer republishes the three
 *   values. There is no on_command — a sensor is read-only, so the plugin
 *   never asks for the Command capability.
 *
 * The values are not from real hardware: this is an example, so the plugin
 * drifts them with a tiny LCG so the hub sees changing STATE across ticks
 * (that's what a rule or the history tab would see from a real sensor).
 *
 * Idiomatic bits a new plugin author should steal:
 *
 *   - ONE DECLARE with several attributes in one buffer (ks_build_declare).
 *   - A repeating timer is the heartbeat; no polling, no busy loops.
 *   - Each wire type has a put-helper in the SDK (ks_put_i16/u16/bool/…);
 *     byte-order is LE and sign is two's complement, so a negative
 *     temperature goes on the wire the way the hub expects it.
 *   - ks_log is harmless and always granted; use it for the "hello, I'm
 *     here" line and for anything a human might want to see in the console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "keystone.h"

/* Attribute ids — the shared table in hubproto::attr. */
#define ATTR_TEMPERATURE 0x10
#define ATTR_HUMIDITY    0x11
#define ATTR_BATTERY     0x1F

/* The handle of the heartbeat timer; any small number the plugin owns. */
#define TIMER_HEARTBEAT 1

/* 10 s between reports is a reasonable sensor cadence — long enough to be
 * quiet, short enough that a rule reacts within one tick. */
#define HEARTBEAT_MS 10000u

static unsigned g_entity = 0;   /* 0 until on_start succeeds */

/* The seed of the drift generator. Same seed every boot is fine for an
 * example — a real sensor reads an ADC. */
static unsigned g_rng = 0x12345678u;

/* The simulated state, kept in units the hub speaks on the wire. */
static int      g_temp_x10 = 215;   /* 21.5 °C */
static unsigned g_hum_x10  = 480;   /* 48.0 % RH */
static unsigned g_battery  = 100;   /* % */

/* One tick of a classic LCG — good enough for a wiggle. */
static unsigned next_rand(void) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return g_rng;
}

unsigned ks_plugin_abi_version(void) { return KS_ABI_VERSION; }

void on_start(void) {
  static const unsigned char attr_ids[3]    = {ATTR_TEMPERATURE,
                                               ATTR_HUMIDITY,
                                               ATTR_BATTERY};
  static const unsigned char value_types[3] = {KS_VALUE_I16,
                                               KS_VALUE_U16,
                                               KS_VALUE_U8};
  /* Readable + reportable, but NOT writable: nobody writes a sensor. */
  static const unsigned char flags[3]       = {KS_FLAG_READABLE | KS_FLAG_REPORTABLE,
                                               KS_FLAG_READABLE | KS_FLAG_REPORTABLE,
                                               KS_FLAG_READABLE | KS_FLAG_REPORTABLE};
  unsigned char desc[KS_DECLARE_SIZE(3)];

  ks_build_declare(desc, KS_DEVICE_SENSOR, attr_ids, value_types, flags, 3);
  g_entity = ks_entity_declare(desc, KS_DECLARE_SIZE(3));
  if (g_entity == 0) {
    /* The hub refused us — manifest caps, or the registry is full.
     * There is nothing a plugin can do but say so and stop. */
    static const char err[] = "climate-sensor: entity_declare refused";
    ks_log((const unsigned char*)err, ks_strlen(err));
    return;
  }

  static const char hello[] = "climate-sensor: ready, reporting every 10 s";
  ks_log((const unsigned char*)hello, ks_strlen(hello));

  /* One STATE straight away, so the app does not show "—" for 10 s. */
  on_timer(TIMER_HEARTBEAT);

  ks_timer_set_repeating(HEARTBEAT_MS, TIMER_HEARTBEAT);
}

/* The heartbeat: drift the readings a little and publish them. */
void on_timer(unsigned id) {
  (void)id;
  if (g_entity == 0) return;

  /* ±0.3 °C around a 21.5 °C indoor baseline. */
  g_temp_x10 += (int)(next_rand() % 7u) - 3;
  if (g_temp_x10 < 180) g_temp_x10 = 180;
  if (g_temp_x10 > 260) g_temp_x10 = 260;

  /* ±0.5 % RH around 48 %. */
  g_hum_x10 += (next_rand() % 11u);
  g_hum_x10 -= 5u;
  if (g_hum_x10 < 300u) g_hum_x10 = 300u;
  if (g_hum_x10 > 700u) g_hum_x10 = 700u;

  /* A battery only ever drops; every ~100 ticks (~17 min) it loses a
   * percent, down to 1. A real sensor reads a divider on a coin cell. */
  if (g_battery > 1u && (next_rand() % 100u) == 0u) g_battery -= 1u;

  unsigned char buf[4];

  ks_put_i16(buf, g_temp_x10);
  ks_state_publish(g_entity, ATTR_TEMPERATURE, buf, 2);

  ks_put_u16(buf, g_hum_x10);
  ks_state_publish(g_entity, ATTR_HUMIDITY, buf, 2);

  buf[0] = (unsigned char)g_battery;
  ks_state_publish(g_entity, ATTR_BATTERY, buf, 1);
}

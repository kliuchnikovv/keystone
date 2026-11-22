/* test-switch — a Keystone plugin written against the SDK.
 *
 * It behaves like a wall switch: on_start declares a Switch entity with one
 * writable, reportable Bool attribute `on_off`; on_command echoes the
 * incoming value back as the entity's state.
 *
 * Built with:
 *   clang --target=wasm32 -nostdlib -O2 \
 *         -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
 *         -Wl,--export=ks_plugin_abi_version -Wl,--export=memory \
 *         -o test-switch.wasm test-switch.c
 *
 * With -DKS_PLUGIN_CRASH=1 the plugin's on_command never returns: it spins
 * in an endless loop. That is the watchdog's target — the hub must notice
 * the overrun, fault the plugin, withdraw its entity and keep running
 * (design §10, "принудительно упавший плагин").
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "keystone.h"

/* The attribute id of `on_off` in the Switch device type (SPEC §6.1). */
#define ATTR_ON_OFF 0x01

/* The handle of the plugin's timer. */
#define TIMER_ID 7

/* The entity id the hub assigned in on_start. 0 means "not declared yet". */
static unsigned g_entity = 0;

/* Scratch space for the value bytes (a bool is one byte). */
static unsigned char g_value[4];

unsigned ks_plugin_abi_version(void) { return KS_ABI_VERSION; }

void on_start(void) {
  /* The DECLARE payload, laid out by hand — the hub reads it as:
   *   u16 entity_id | u8 device_type | u8 attr_count | n x {id, type, flags}
   */
  static const unsigned char attr_ids[1] = {ATTR_ON_OFF};
  static const unsigned char value_types[1] = {KS_VALUE_BOOL};
  static const unsigned char flags[1] =
      {KS_FLAG_READABLE | KS_FLAG_WRITABLE | KS_FLAG_REPORTABLE};
  unsigned char desc[KS_DECLARE_SIZE(1)];

  ks_build_declare(desc, KS_DEVICE_SWITCH, attr_ids, value_types, flags, 1);
  g_entity = ks_entity_declare(desc, KS_DECLARE_SIZE(1));

  /* Arm a repeating timer: the hub calls on_timer(TIMER_ID) every 5 s. */
  ks_timer_set_repeating(5000, TIMER_ID);
}

void on_command(unsigned entity, unsigned attr, const unsigned char* value,
                unsigned value_len, unsigned seq) {
  (void)seq;
  if (g_entity == 0) return;                 /* never declared */
  if (entity != g_entity) return;            /* not ours */
  if (attr != ATTR_ON_OFF) return;           /* not our attribute */
  if (value_len < 1) return;

#ifdef KS_PLUGIN_CRASH
  /* The watchdog's target: an endless loop that never returns from the
   * callback. The hub must interrupt it (the interpreter's instruction cap
   * breaks the loop), fault the plugin and keep running. */
  for (;;) {
    /* spin */
  }
#else
  /* Echo the command back as the entity's state: the plugin's device has
   * "applied" the command. */
  ks_put_bool(g_value, value[0] != 0);
  ks_state_publish(g_entity, ATTR_ON_OFF, g_value, 1);
#endif
}

/* The plugin's timer fired. */
void on_timer(unsigned id) {
  (void)id;
  /* The timer is a heartbeat: the plugin republishes its state, so the hub
   * (and the app) see the device is still alive. */
  if (g_entity == 0) return;
  ks_put_bool(g_value, 0);
  ks_state_publish(g_entity, ATTR_ON_OFF, g_value, 1);
}

/* keystone.h — the Keystone plugin SDK.
 *
 * A Keystone plugin is a freestanding WebAssembly module. It has no libc, no
 * WASI, no filesystem: everything it needs comes from the hub through the
 * host imports declared below. The hub runs it inside WAMR with a wall-clock
 * budget per callback (100 ms by default) and an instruction cap, so a plugin
 * that loops forever is interrupted and put into the `faulted` state while the
 * hub keeps running.
 *
 * The ABI version is 1. A plugin must export `ks_plugin_abi_version()`
 * returning KS_ABI_VERSION, and the hub refuses anything else.
 *
 * BUILD (see ../README.md for the full recipe):
 *
 *   clang --target=wasm32 -nostdlib -O2 -flto \
 *         -Wl,--no-entry -Wl,--export-dynamic \
 *         -Wl,--allow-undefined \
 *         -Wl,--export=ks_plugin_abi_version \
 *         -Wl,--export=memory \
 *         -o plugin.wasm plugin.c
 *
 * The host imports live in the module "env"; the hub registers them with
 * `wasm_runtime_register_natives("env", ...)`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef KEYSTONE_PLUGIN_SDK_H
#define KEYSTONE_PLUGIN_SDK_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ABI                                                                 */
/* ------------------------------------------------------------------ */

/* The ABI version this header speaks. The hub refuses a plugin that
 * exports anything else (see core/plugin_host_api.hpp: kAbiVersion). */
#define KS_ABI_VERSION 1u

/* ------------------------------------------------------------------ */
/* Error codes (mirrors core::plugin::KsErr)                          */
/* ------------------------------------------------------------------ */

typedef enum {
  KS_OK = 0,
  KS_DENIED,     /* the capability was not granted, or the target is out of bounds */
  KS_BAD_ARG,    /* malformed arguments */
  KS_NO_MEM,     /* out of buffer / heap */
  KS_NOT_FOUND,  /* no such entity / key */
  KS_AGAIN,      /* would block */
  KS_CLOSED      /* socket/resource closed */
} ks_err;

/* ------------------------------------------------------------------ */
/* Device types and value types (mirrors hubproto)                    */
/* ------------------------------------------------------------------ */

typedef enum {
  KS_DEVICE_LIGHT     = 0x01,
  KS_DEVICE_SENSOR    = 0x02,
  KS_DEVICE_SWITCH    = 0x03,
  KS_DEVICE_COVER     = 0x04,
  KS_DEVICE_LOCK      = 0x05,
  KS_DEVICE_CLIMATE   = 0x06,
  KS_DEVICE_OCCUPANCY = 0x07,
  KS_DEVICE_CONTACT   = 0x08,
  KS_DEVICE_BUTTON    = 0x09
} ks_device_type;

typedef enum {
  KS_VALUE_BOOL    = 0x01,  /* 1 byte 0/1 */
  KS_VALUE_U8      = 0x02,  /* 1 byte */
  KS_VALUE_I16     = 0x03,  /* 2 bytes LE */
  KS_VALUE_U16     = 0x04,  /* 2 bytes LE */
  KS_VALUE_F32     = 0x05,  /* 4 bytes IEEE754 */
  KS_VALUE_XY      = 0x06,  /* 4 bytes: u16 x, u16 y */
  KS_VALUE_ENUM8   = 0x07,  /* 1 byte */
  KS_VALUE_U32     = 0x08   /* 4 bytes LE */
} ks_value_type;

/* Attribute flags. */
#define KS_FLAG_READABLE   0x01
#define KS_FLAG_WRITABLE   0x02
#define KS_FLAG_REPORTABLE 0x04

/* ------------------------------------------------------------------ */
/* Host imports                                                        */
/* ------------------------------------------------------------------ */

/* Declare a virtual entity.
 *
 * `desc` points into the plugin's linear memory and holds a DECLARE payload:
 *
 *     u16 entity_id   (the hub overrides it: the id is assigned by the hub)
 *     u8  device_type (KS_DEVICE_*)
 *     u8  attr_count
 *     attr_count x { u8 attr_id, u8 value_type, u8 flags }
 *
 * The hub assigns the entity id and returns it; 0 means the declaration was
 * refused (no capability, too many entities, or a malformed descriptor).
 * The hub also fills in the entity's name from the plugin's manifest.
 */
__attribute__((import_module("env"), import_name("ks_entity_declare")))
unsigned ks_entity_declare(const unsigned char* desc, unsigned desc_len);

/* Publish the STATE of one of the plugin's own entities.
 *
 * `value` points into the plugin's linear memory and holds the attribute's
 * value in its wire encoding (see KS_VALUE_*): a bool is one byte 0/1, a u16
 * is two bytes little-endian, and so on. The hub validates the type against
 * the declaration.
 */
__attribute__((import_module("env"), import_name("ks_state_publish")))
int ks_state_publish(unsigned entity, unsigned attr, const unsigned char* value,
                     unsigned value_len);

/* Write a line to the hub's log. Always granted (the Log capability is
 * harmless and given to every plugin). */
__attribute__((import_module("env"), import_name("ks_log")))
int ks_log(const unsigned char* msg, unsigned len);

/* Arm a timer.
 *
 * `ms` is the delay in milliseconds; `id` is the plugin's own handle — it
 * comes back in on_timer(id). Arming the same id again replaces the
 * previous timer, so a plugin has exactly one timer per id.
 *
 * The Timer capability is harmless and granted to every plugin.
 */
__attribute__((import_module("env"), import_name("ks_timer_set")))
int ks_timer_set(unsigned ms, unsigned id);

/* Arm a REPEATING timer: the hub calls on_timer(id) every `period_ms`
 * until the timer is replaced or the plugin is unloaded. This is what a
 * heartbeat is. A plugin that was asleep for a while gets one tick, not a
 * burst of catch-up ticks. */
__attribute__((import_module("env"), import_name("ks_timer_set_repeating")))
int ks_timer_set_repeating(unsigned period_ms, unsigned id);

/* ------------------------------------------------------------------ */
/* Exports — the plugin's side of the contract                        */
/* ------------------------------------------------------------------ */

/* REQUIRED. Returns the ABI version the plugin was built against. The hub
 * refuses a plugin that does not export it, or that returns anything but
 * KS_ABI_VERSION.
 *
 * This export is checked when the plugin is LOADED, before it runs and
 * before it declares anything: the hub reads it out of the binary and
 * refuses a plugin that speaks a different ABI. The manifest declares an
 * ABI too, but the manifest is data and can lie — this export lives inside
 * the binary, so it is the ground truth. */
__attribute__((export_name("ks_plugin_abi_version")))
unsigned ks_plugin_abi_version(void);

/* OPTIONAL. Called once when the plugin starts. Declare the entities here. */
__attribute__((export_name("on_start")))
void on_start(void);

/* OPTIONAL. Called when the plugin stops. */
__attribute__((export_name("on_stop")))
void on_stop(void);

/* OPTIONAL. A COMMAND arrived for one of the plugin's entities.
 *
 *   entity    the entity id the hub assigned
 *   attr      the attribute id
 *   value     points into the plugin's linear memory, `value_len` bytes
 *   seq       the hub's command sequence number
 *
 * The plugin answers by publishing the new state with ks_state_publish().
 */
__attribute__((export_name("on_command")))
void on_command(unsigned entity, unsigned attr, const unsigned char* value,
                unsigned value_len, unsigned seq);

/* OPTIONAL. A timer the plugin armed with ks_timer_set() fired. `id` is
 * the handle the plugin passed to ks_timer_set(). */
__attribute__((export_name("on_timer")))
void on_timer(unsigned id);

/* ------------------------------------------------------------------ */
/* Helpers for the freestanding build (no libc)                       */
/* ------------------------------------------------------------------ */

/* The size of a DECLARE payload with `attr_count` attributes. */
#define KS_DECLARE_SIZE(attr_count) (4u + 3u * (attr_count))

/* Fill a DECLARE payload in the plugin's linear memory.
 *
 * `out` must have room for KS_DECLARE_SIZE(attr_count) bytes. The entity id
 * is ignored by the hub (it assigns one) but is set to 0 here for clarity.
 */
static inline void ks_build_declare(unsigned char* out,
                                    ks_device_type device_type,
                                    const unsigned char* attr_ids,
                                    const unsigned char* value_types,
                                    const unsigned char* flags,
                                    unsigned attr_count) {
  unsigned i;
  out[0] = 0; /* entity id — the hub assigns it */
  out[1] = 0;
  out[2] = (unsigned char)device_type;
  out[3] = (unsigned char)attr_count;
  for (i = 0; i < attr_count; ++i) {
    out[4 + 3 * i + 0] = attr_ids[i];
    out[4 + 3 * i + 1] = value_types[i];
    out[4 + 3 * i + 2] = flags[i];
  }
}

/* Store a bool attribute value (one byte) into the plugin's memory. */
static inline void ks_put_bool(unsigned char* out, int value) {
  out[0] = value ? 1 : 0;
}

/* Store a u16 attribute value (two bytes, little-endian) into the plugin's
 * memory. */
static inline void ks_put_u16(unsigned char* out, unsigned value) {
  out[0] = (unsigned char)(value & 0xFFu);
  out[1] = (unsigned char)((value >> 8) & 0xFFu);
}

/* Store an i16 attribute value (two bytes, little-endian two's complement).
 * Signed temperatures and setpoints go on the wire as i16 in 0.1 °C. */
static inline void ks_put_i16(unsigned char* out, int value) {
  unsigned u = (unsigned)(value & 0xFFFF);
  out[0] = (unsigned char)(u & 0xFFu);
  out[1] = (unsigned char)((u >> 8) & 0xFFu);
}

/* Store a u32 attribute value (four bytes, little-endian) into the plugin's
 * memory. */
static inline void ks_put_u32(unsigned char* out, unsigned value) {
  out[0] = (unsigned char)(value & 0xFFu);
  out[1] = (unsigned char)((value >> 8) & 0xFFu);
  out[2] = (unsigned char)((value >> 16) & 0xFFu);
  out[3] = (unsigned char)((value >> 24) & 0xFFu);
}

/* The length of a NUL-terminated string — the freestanding replacement for
 * strlen(). */
static inline unsigned ks_strlen(const char* s) {
  unsigned n = 0;
  while (s[n] != '\0') ++n;
  return n;
}

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_PLUGIN_SDK_H */

// Names a person can read.
//
// The hub enforces the same rule (core/include/core/app_requests.hpp: valid_entity_name),
// so the app refuses early what the hub would refuse anyway. Fifteen characters:
// a name is a label, not a sentence. Letters of any script, digits, spaces and a
// little punctuation. Scene names keep the older, machine-like rule (see below).

export const MAX_NAME_CHARS = 15;
export const MAX_NAME_BYTES = 30;

/** The characters a name may use besides letters and digits of any script. */
const ALLOWED_ASCII = /^[A-Za-z0-9 _.,&'()-]+$/;

/** UTF-8 length in bytes — the hub stores a name in a 32-byte field. */
export function utf8Length(s: string): number {
  let n = 0;
  for (const ch of s) {
    const c = ch.codePointAt(0) ?? 0;
    n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  }
  return n;
}

/** What is wrong with a device name, or null when the hub will take it. */
export function nameProblem(name: string): string | null {
  const chars = Array.from(name);
  if (chars.length === 0) return 'Give the device a name';
  if (chars.length > MAX_NAME_CHARS) return `At most ${MAX_NAME_CHARS} characters`;
  if (utf8Length(name) > MAX_NAME_BYTES) return `At most ${MAX_NAME_CHARS} characters`;
  if (name.startsWith(' ') || name.endsWith(' ')) return 'No spaces at the start or the end';
  if (!ALLOWED_ASCII.test(name.replace(/[^\x00-\x7F]/g, ''))) {
    return 'Letters, digits, spaces and . , & \' ( ) - _ only';
  }
  if (!/[A-Za-z]/.test(name) && !/[^\x00-\x7F]/.test(name)) return 'The name needs a letter';
  if (/^[+-]?(0[xX][0-9a-fA-F]+|\d+)$/.test(name)) return 'The name must not look like an id';
  return null;
}

/** Scene names stay machine-like: they are keys in a table, not labels on a wall. */
export function sceneNameProblem(name: string): string | null {
  if (!/^[a-z][a-z0-9_]{0,22}$/.test(name)) {
    return 'Use a-z, 0-9 and _, start with a letter, at most 23 characters';
  }
  return null;
}

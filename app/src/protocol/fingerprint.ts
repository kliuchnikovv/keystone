// Certificate fingerprints as people copy them: "AA:BB:…", "aabb…", "sha256:AA BB …".
const HEX64 = /^[0-9A-F]{64}$/;

/** Canonical "AA:BB:…:FF" (uppercase, colon separated), or null if it is not a SHA-256 fingerprint. */
export function normalizeFingerprint(input: string): string | null {
  const hex = input
    .trim()
    .replace(/^sha-?256\s*[:=]?\s*/i, '')
    .replace(/[\s:]/g, '')
    .toUpperCase();
  if (!HEX64.test(hex)) return null;
  return hex.match(/.{2}/g)!.join(':');
}

export function sameFingerprint(a: string, b: string): boolean {
  const x = normalizeFingerprint(a);
  const y = normalizeFingerprint(b);
  return x !== null && y !== null && x === y;
}

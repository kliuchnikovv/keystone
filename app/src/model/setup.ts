// What the first-time setup screen needs that is not UI: the setup code as a person types it, the token the app
// invents for the hub, and what to say when something goes wrong. The code rules mirror core/setup_mode.hpp.
import type { SetupFailure } from '../protocol/setup';

const ALPHABET = '23456789ABCDEFGHJKMNPQRSTUVWXYZ'; // no 0/O, 1/I/L
export const SETUP_CODE_LENGTH = 12;

/** "abcd-efgh-jkmn" → "ABCDEFGHJKMN"; null if it does not read as a setup code. */
export function normalizeSetupCode(input: string): string | null {
  const code = input.replace(/[-\s]/g, '').toUpperCase();
  if (code.length !== SETUP_CODE_LENGTH) return null;
  for (const c of code) if (!ALPHABET.includes(c)) return null;
  return code;
}

/** "ABCDEFGHJKMN" → "ABCD-EFGH-JKMN", the way it is printed. */
export function formatSetupCode(code: string): string {
  return code.match(/.{1,4}/g)?.join('-') ?? code;
}

/** Why a code does not read as one, in words (before the hub is asked). */
export function setupCodeProblem(input: string): string | null {
  const code = input.replace(/[-\s]/g, '').toUpperCase();
  if (code.length === 0) return 'Enter the setup code from the hub’s label';
  if (code.length !== SETUP_CODE_LENGTH) return `The setup code is ${SETUP_CODE_LENGTH} characters (you typed ${code.length})`;
  if (normalizeSetupCode(input) === null) return 'The setup code uses letters and digits only, never 0, O, 1, I or L';
  return null;
}

/** A random token for the hub: 16 random bytes as 32 hex digits (the hub takes 8..64 printable characters). */
export async function generateToken(): Promise<string> {
  const bytes = new Uint8Array(16);
  const c = (globalThis as { crypto?: { getRandomValues?: (a: Uint8Array) => Uint8Array } }).crypto;
  if (c?.getRandomValues) {
    c.getRandomValues(bytes);
  } else {
    // React Native has no global crypto: use the platform's random source. Never Math.random for a secret.
    const { getRandomBytes } = await import('expo-crypto');
    bytes.set(getRandomBytes(bytes.length));
  }
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

/** What to tell the person when a step of the setup failed. */
export function describeSetupFailure(kind: SetupFailure, detail?: string): string {
  switch (kind) {
    case 'unreachable':
      return 'Could not reach the hub. Is your phone on the hub’s Wi-Fi (Keystone-…)? If the phone says that network has no internet, keep using it anyway.';
    case 'bad_code':
      return 'That is not this hub’s setup code. It is the one on the hub’s label, the same as the Wi-Fi password.';
    case 'not_setup':
      return 'This hub is already set up. Go back and connect to it with its token.';
    case 'unsupported':
      return 'This build of the app cannot set up a new hub (it needs the native Keystone module: use a development build, not Expo Go or the web).';
    case 'busy':
      return 'The hub is busy with another phone. Try again in a moment.';
    case 'incompatible':
      return 'This app and the hub speak different versions of the protocol. Update the app or the hub.';
    case 'timeout':
      return 'The hub did not answer in time. Try again.';
    case 'refused':
      return detail ? `The hub refused: ${detail}` : 'The hub refused that.';
    case 'closed':
      return 'The connection to the hub dropped. Check that your phone is still on the hub’s Wi-Fi and try again.';
  }
}

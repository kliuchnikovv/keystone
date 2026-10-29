// The last known state of the home, in the browser: localStorage. It is the same
// place the token lives, so the state is no more private than the token (see
// ../settings.ts: a browser cannot keep a secret from the page).
const KEY = 'keystone.state';

export async function readStateCache(): Promise<string | null> {
  try {
    return globalThis.localStorage?.getItem(KEY) ?? null;
  } catch {
    return null;
  }
}

export async function writeStateCache(text: string): Promise<void> {
  try {
    globalThis.localStorage?.setItem(KEY, text);
  } catch {
    /* the cache is a convenience: failing to write it must not break the app */
  }
}

export async function clearStateCache(): Promise<void> {
  try {
    globalThis.localStorage?.removeItem(KEY);
  } catch {
    /* nothing to clear */
  }
}

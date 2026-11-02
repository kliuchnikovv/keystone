// The last known state of the home, on the phone: a file in the app's documents.
// (The secure store is for secrets and is too small for this; the file is the
// app's own sandbox and is thrown away with the app.)
import { File, Paths } from 'expo-file-system';

const NAME = 'keystone-state.json';

function file(): File {
  return new File(Paths.document, NAME);
}

export async function readStateCache(): Promise<string | null> {
  try {
    const f = file();
    if (!f.exists) return null;
    return await f.text();
  } catch {
    return null; // no cache is not a problem
  }
}

export async function writeStateCache(text: string): Promise<void> {
  try {
    const f = file();
    if (!f.exists) f.create();
    f.write(text);
  } catch {
    /* the cache is a convenience: failing to write it must not break the app */
  }
}

export async function clearStateCache(): Promise<void> {
  try {
    const f = file();
    if (f.exists) f.delete();
  } catch {
    /* nothing to clear */
  }
}

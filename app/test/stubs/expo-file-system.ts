// Tests run without a native runtime: the file system is a map in memory.
const files = new Map<string, string>();

export class File {
  readonly uri: string;

  constructor(...parts: (string | { uri: string })[]) {
    this.uri = parts.map((p) => (typeof p === 'string' ? p : p.uri)).join('/');
  }

  get exists(): boolean {
    return files.has(this.uri);
  }

  create(): void {
    if (!files.has(this.uri)) files.set(this.uri, '');
  }

  write(text: string): void {
    files.set(this.uri, text);
  }

  async text(): Promise<string> {
    return files.get(this.uri) ?? '';
  }

  delete(): void {
    files.delete(this.uri);
  }
}

export const Paths = {
  document: 'file:///documents',
  cache: 'file:///cache',
};

/** Forget everything that was written (between tests). */
export function __resetFiles(): void {
  files.clear();
}

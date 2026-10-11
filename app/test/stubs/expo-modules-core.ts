// Tests run without a native runtime: there is never a native module (the web/Expo Go case).
export function requireOptionalNativeModule<T = unknown>(_name: string): T | null {
  return null;
}

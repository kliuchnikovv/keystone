// Tests run without a native runtime: there is never a native module (the web/Expo Go case).
export function requireOptionalNativeModule<T = unknown>(_name: string): T | null {
  return null;
}

// Some packages (expo-asset, and through it expo-font) ask for a module at
// import time. There is none here, so the stub is a hollow object: the code
// that would call it never runs in a test.
export function requireNativeModule<T = unknown>(_name: string): T {
  return {} as T;
}

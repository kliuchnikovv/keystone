import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitest/config';

// UI tests render the real components with react-native-web in jsdom (a file opts in
// with `// @vitest-environment jsdom`); everything else runs in plain Node. expo-modules-core
// needs a native runtime, so tests get a stub that reports "no native modules".
export default defineConfig({
  esbuild: { jsx: 'automatic' },
  resolve: {
    alias: {
      'react-native': 'react-native-web',
      // Flow-typed sources; the UI tests only need something that occupies the icon's box.
      'react-native-svg': fileURLToPath(new URL('./test/stubs/react-native-svg.tsx', import.meta.url)),
      'expo-modules-core': fileURLToPath(new URL('./test/stubs/expo-modules-core.ts', import.meta.url)),
      'expo-file-system': fileURLToPath(new URL('./test/stubs/expo-file-system.ts', import.meta.url)),
      // The camera needs a real device; the stub reads the code the test queued.
      'expo-camera': fileURLToPath(new URL('./test/stubs/expo-camera.tsx', import.meta.url)),
    },
  },
  test: { include: ['test/**/*.test.{ts,tsx}'], environment: 'node' },
});

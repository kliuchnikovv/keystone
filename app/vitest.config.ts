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
      'expo-modules-core': fileURLToPath(new URL('./test/stubs/expo-modules-core.ts', import.meta.url)),
    },
  },
  test: { include: ['test/**/*.test.{ts,tsx}'], environment: 'node' },
});

import { defineConfig } from 'vitest/config';

// UI tests render the real components with react-native-web in jsdom (a file opts in
// with `// @vitest-environment jsdom`); everything else runs in plain Node.
export default defineConfig({
  esbuild: { jsx: 'automatic' },
  resolve: { alias: { 'react-native': 'react-native-web' } },
  test: { include: ['test/**/*.test.{ts,tsx}'], environment: 'node' },
});

/// <reference types="node" />
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import path from 'node:path';

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: { '@': path.resolve(__dirname, 'src') },
  },
  server: {
    port: 5173,
    proxy: {
      '/devices': 'http://localhost:7777',
      '/rules': 'http://localhost:7777',
      '/stream': { target: 'http://localhost:7777', changeOrigin: true, ws: false },
      '/health': 'http://localhost:7777',
      '/discover': 'http://localhost:7777',
    },
  },
});

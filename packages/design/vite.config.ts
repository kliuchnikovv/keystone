import { defineConfig } from 'vite';

/**
 * Пакет собирается дважды, потому что у него два разных потребителя.
 *
 *   vite build                — dist/, модули сохранены. apps/web импортирует
 *                               отдельные компоненты, и неиспользованные
 *                               вытрясаются бандлером приложения.
 *   vite build --mode standalone
 *                             — dist/standalone/index.js, один файл. Это
 *                               будущий /design/v1/index.js: плагин-автор
 *                               получает всю систему одним импортом без сборки.
 *
 * lit в обеих сборках external. Инлайнить его нельзя: plugin-ui-integration.md
 * §5.4 требует, чтобы все плагины делили один инстанс через import map. Второй
 * инстанс — это два ReactiveElement-базиса, два кэша css и молча падающие
 * instanceof на границе.
 */
const EXTERNAL = [
  'lit',
  /^lit\//,
  '@lit/react',
  'react',
  'react-dom',
  /^react\//,
];

export default defineConfig(({ mode }) => {
  const standalone = mode === 'standalone';
  const entry: Record<string, string> = standalone
    ? { index: 'src/index.ts' }
    : { index: 'src/index.ts', react: 'src/react.ts' };

  return {
    build: {
      outDir: standalone ? 'dist/standalone' : 'dist',
      emptyOutDir: !standalone,
      target: 'es2022',
      cssCodeSplit: false,
      lib: { entry, formats: ['es'] },
      rollupOptions: {
        external: EXTERNAL,
        output: standalone
          ? { entryFileNames: '[name].js' }
          : {
              preserveModules: true,
              preserveModulesRoot: 'src',
              entryFileNames: '[name].js',
            },
      },
    },
  };
});

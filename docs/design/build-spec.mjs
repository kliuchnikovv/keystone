#!/usr/bin/env node
// Builds docs/design/keystone-spec.html from keystone-spec.src.html:
// inlines the icon sprite (so <use href="#i-name"> works from file://) and
// stamps the build date.
//
//   node docs/design/build-spec.mjs

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = readFileSync(join(here, 'keystone-spec.src.html'), 'utf8');
const sprite = readFileSync(join(here, 'icons', 'sprite.svg'), 'utf8').trim();

if (!src.includes('<!-- @@SPRITE@@ -->')) {
  console.error('No @@SPRITE@@ marker in keystone-spec.src.html');
  process.exit(1);
}

const out = src
  .replace('<!-- @@SPRITE@@ -->', sprite)
  .replace(/\{\{DATE\}\}/g, new Date().toISOString().slice(0, 10))
  // Every <use> needs a sized, scaled viewport: a bare <svg> would default to 300x150
  // and blow the layout apart. CSS rules still win over these attributes.
  .replace(
    /<svg><use href="#(i-[a-z]+)"\/><\/svg>/g,
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><use href="#$1"/></svg>',
  );

writeFileSync(join(here, 'keystone-spec.html'), out);
console.log(`keystone-spec.html written (${out.length} bytes)`);

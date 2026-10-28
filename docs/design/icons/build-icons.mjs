#!/usr/bin/env node
// Generates docs/design/icons/*.svg from the app's icon source (app/src/ui/Glyph.tsx),
// so the design icon sheet can never drift from what the app draws.
//
//   node docs/design/icons/build-icons.mjs
//
// The geometry is copied verbatim: 24x24 grid, 2 px stroke, round caps and joins.
// A shape that the app fills (the "on" variant) becomes fill="currentColor"
// fill-opacity="0.22"; a stroke-only shape stays fill="none". Colour comes from
// currentColor, so the same file works in light and dark.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const src = readFileSync(join(repo, 'app', 'src', 'ui', 'Glyph.tsx'), 'utf8');

const names = [...src.matchAll(/case '([a-zA-Z]+)':/g)].map((m) => m[1]);
if (names.length === 0) {
  console.error('No icons found in Glyph.tsx');
  process.exit(1);
}

const attr = (s, name) => {
  const m = s.match(new RegExp(`\\b${name}="([^"]*)"`));
  return m ? m[1] : null;
};

const svgFor = (name) => {
  const start = src.indexOf(`case '${name}':`);
  const next = src.indexOf("case '", start + 10);
  const body = src.slice(start, next === -1 ? src.length : next);
  const shapes = [...body.matchAll(/<(Path|Circle|Rect)\b([\s\S]*?)\/>/g)];
  const parts = shapes.map(([, tag, raw]) => {
    const filled = raw.includes('{...on}');
    const bits = [`d="${attr(raw, 'd') ?? ''}"`];
    if (tag === 'Circle') {
      bits.push(`cx="${attr(raw, 'cx')}"`, `cy="${attr(raw, 'cy')}"`, `r="${attr(raw, 'r')}"`);
    } else if (tag === 'Rect') {
      bits.push(
        `x="${attr(raw, 'x')}"`,
        `y="${attr(raw, 'y')}"`,
        `width="${attr(raw, 'width')}"`,
        `height="${attr(raw, 'height')}"`,
        `rx="${attr(raw, 'rx') ?? 0}"`,
      );
    }
    bits.push(filled ? 'fill="currentColor" fill-opacity="0.22"' : 'fill="none"');
    return `  <${tag.toLowerCase()} ${bits.join(' ')} />`;
  });
  return [
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"',
    '     fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">',
    ...parts,
    '</svg>',
    '',
  ].join('\n');
};

mkdirSync(here, { recursive: true });
const symbols = [];
for (const name of names) {
  writeFileSync(join(here, `${name.replace(/([A-Z])/g, '-$1').toLowerCase()}.svg`), svgFor(name));
  // The id is kebab-case: eyeOff -> i-eye-off.
  const id = name.replace(/([A-Z])/g, '-$1').toLowerCase();
  symbols.push(
    `  <symbol id="i-${id}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">\n` +
      svgFor(name)
        .split('\n')
        .slice(2, -2)
        .map((l) => '  ' + l)
        .join('\n') +
      '\n  </symbol>',
  );
}

writeFileSync(
  join(here, 'sprite.svg'),
  [
    '<svg xmlns="http://www.w3.org/2000/svg" style="display:none">',
    ...symbols,
    '</svg>',
    '',
  ].join('\n'),
);

console.log(`${names.length} icons written to ${here}: ${names.join(', ')}`);

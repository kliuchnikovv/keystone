#!/usr/bin/env node
/**
 * Проверяет, что CSS-изменение не поменяло ни одного пикселя.
 *
 * Сравнивать текст бесполезно: переименование токена меняет каждую строку,
 * ничего не меняя на экране. Поэтому мы резолвим все var() против объявлений
 * в :root и сравниваем РАЗРЕШЁННЫЕ значения. Токеновый рефакторинг обязан
 * давать пустой diff; всё, что вылезло, — настоящее визуальное изменение.
 *
 *   node scripts/css-diff.mjs snapshot dist/assets/index-*.css /tmp/before.json
 *   node scripts/css-diff.mjs diff /tmp/before.json /tmp/after.json
 *
 * Выход 1, если разрешённые значения разошлись.
 */
import { readFileSync, writeFileSync } from 'node:fs';
import postcss from 'postcss';

const MAX_DEPTH = 16;

/**
 * Собирает кастомные свойства.
 *
 * Глобальные (:root и подобные) резолвятся до значений. Компонентно-локальные
 * вроде --w/--thumb у Toggle объявлены под разными селекторами с разными
 * значениями — их резолвить нельзя, но и терять нельзя: помечаем ambiguous,
 * чтобы настоящая поломка (переменная не объявлена нигде) осталась заметной.
 */
function collectVars(root) {
  const global = new Map();
  const local = new Map();
  root.walkRules((rule) => {
    const isGlobal = /(^|,)\s*(:root|html|\[data-theme[^\]]*\])/.test(rule.selector);
    rule.walkDecls(/^--/, (decl) => {
      if (isGlobal) global.set(decl.prop, decl.value);
      else if (!local.has(decl.prop)) local.set(decl.prop, decl.value);
      else if (local.get(decl.prop) !== decl.value) local.set(decl.prop, AMBIGUOUS);
    });
  });
  for (const [prop, value] of local) if (!global.has(prop)) global.set(prop, value);
  return global;
}

const AMBIGUOUS = Symbol('ambiguous');

/**
 * Разворачивает var(--name, fallback) до литералов.
 * Парсим вручную, а не регуляркой: фолбэк сам может содержать var() со
 * своими запятыми, и любая регулярка на этом ломается.
 */
function resolve(value, vars, depth = 0) {
  if (depth > MAX_DEPTH || !value.includes('var(')) return value;

  const start = value.indexOf('var(');
  let i = start + 4;
  let nesting = 1;
  while (i < value.length && nesting > 0) {
    if (value[i] === '(') nesting++;
    else if (value[i] === ')') nesting--;
    i++;
  }
  if (nesting > 0) return value; // незакрытая скобка — оставляем как есть

  const inner = value.slice(start + 4, i - 1);
  const comma = splitTopLevel(inner);
  const name = comma[0].trim();
  const fallback = comma.length > 1 ? comma.slice(1).join(',').trim() : null;

  let replacement;
  if (vars.get(name) === AMBIGUOUS) {
    replacement = `<local:${name}>`;
  } else if (vars.has(name)) {
    replacement = resolve(vars.get(name), vars, depth + 1);
  } else if (fallback !== null) {
    replacement = resolve(fallback, vars, depth + 1);
  } else {
    // Переменная не объявлена и фолбэка нет — свойство невалидно.
    // Помечаем явно, иначе такая поломка выглядит как «значение не изменилось».
    replacement = `<UNDEFINED:${name}>`;
  }

  return resolve(value.slice(0, start) + replacement + value.slice(i), vars, depth + 1);
}

/** Делит по запятым верхнего уровня, игнорируя запятые внутри скобок. */
function splitTopLevel(text) {
  const parts = [];
  let depth = 0;
  let current = '';
  for (const ch of text) {
    if (ch === '(') depth++;
    else if (ch === ')') depth--;
    if (ch === ',' && depth === 0) {
      parts.push(current);
      current = '';
    } else {
      current += ch;
    }
  }
  parts.push(current);
  return parts;
}

const normalize = (s) => s.replace(/\s+/g, ' ').trim();

/**
 * Гасит хэши CSS Modules.
 *
 * Vite генерирует `_local_HASH_LINE`, где HASH считается от содержимого файла.
 * Любая правка CSS — даже переименование токена — меняет каждый хэш, и без
 * этой нормализации diff показывает «изменилось всё». Номер строки оставляем:
 * он различает одноимённые классы из разных файлов.
 */
const stripModuleHash = (s) => s.replace(/_([A-Za-z][\w-]*?)_[a-z0-9]{4,8}_(\d+)\b/g, '_$1_L$2');

function snapshot(cssPath) {
  const root = postcss.parse(readFileSync(cssPath, 'utf8'));
  const vars = collectVars(root);
  const entries = [];

  root.walkDecls((decl) => {
    // Сами объявления токенов пропускаем: их имена и есть то, что меняется.
    // Значение токена доедет до снимка через каждое место, где он используется.
    if (decl.prop.startsWith('--')) return;

    const parents = [];
    for (let node = decl.parent; node && node.type !== 'root'; node = node.parent) {
      parents.unshift(node.type === 'atrule' ? `@${node.name} ${node.params}` : node.selector);
    }
    entries.push(
      stripModuleHash(
        `${parents.map(normalize).join(' >> ')} { ${decl.prop}: ${normalize(resolve(decl.value, vars))}${decl.important ? ' !important' : ''} }`,
      ),
    );
  });

  entries.sort();
  return entries;
}

function diff(beforePath, afterPath) {
  const before = JSON.parse(readFileSync(beforePath, 'utf8'));
  const after = JSON.parse(readFileSync(afterPath, 'utf8'));

  const count = (list) => {
    const map = new Map();
    for (const item of list) map.set(item, (map.get(item) ?? 0) + 1);
    return map;
  };
  const b = count(before);
  const a = count(after);

  const removed = [];
  const added = [];
  for (const [key, n] of b) {
    const delta = n - (a.get(key) ?? 0);
    for (let i = 0; i < delta; i++) removed.push(key);
  }
  for (const [key, n] of a) {
    const delta = n - (b.get(key) ?? 0);
    for (let i = 0; i < delta; i++) added.push(key);
  }

  const undefinedVars = after.filter((line) => line.includes('<UNDEFINED:'));
  if (undefinedVars.length > 0) {
    console.error(`Необъявленные переменные (${undefinedVars.length}):`);
    for (const line of undefinedVars.slice(0, 20)) console.error(`  ! ${line}`);
  }

  if (removed.length === 0 && added.length === 0) {
    console.log(`Разрешённые значения идентичны (${after.length} объявлений).`);
    return undefinedVars.length > 0 ? 1 : 0;
  }

  console.error(`Разошлось: -${removed.length} +${added.length}`);
  for (const line of removed) console.error(`  - ${line}`);
  for (const line of added) console.error(`  + ${line}`);
  return 1;
}

const [command, ...args] = process.argv.slice(2);
if (command === 'snapshot') {
  const [cssPath, outPath] = args;
  const entries = snapshot(cssPath);
  writeFileSync(outPath, JSON.stringify(entries, null, 2));
  console.log(`${entries.length} объявлений → ${outPath}`);
} else if (command === 'diff') {
  process.exit(diff(args[0], args[1]));
} else {
  console.error('usage: css-diff.mjs snapshot <css> <out.json> | diff <before.json> <after.json>');
  process.exit(2);
}

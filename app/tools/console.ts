// Interactive remote console for the Keystone hub.
//   npm run console -- --url ws://192.168.0.202:8080/ws --token <token>
//   npm run console -- --url wss://192.168.0.202:8443/ws --token <token> --fingerprint <sha256>
// Connects over the app protocol (WebSocket+JSON), shows live state updates and gives a
// REPL that mirrors the brain's USB console. Type `help` for the command list.
import * as readline from 'node:readline';
import { HubClient, HubError } from '../src/protocol/client';
import type { AckStatus, Entity, HistoryEvent, Value } from '../src/protocol/messages';
import type { HomeState } from '../src/protocol/store';
import { pinnedSocketFactory } from './pinned-socket';
import { hexId, deviceTitle, deviceLabel, primarySwitch, isReachable, canManageDevice } from '../src/model/devices';
import { formatValue, attrLabel, formatUptime, formatEnergy, describeAck } from '../src/model/format';
import { describeRule, describeSchedule, describeTimer, describeScene, describeLocation, describeWifi, formatCountdown, signalText } from '../src/model/automation';
import { describeEvent, entityName as histEntityName, formatEventTime, type Described } from '../src/model/history';

// ---- ANSI helpers -------------------------------------------------------------------

const C = {
  reset: '\x1b[0m',
  dim: '\x1b[2m',
  bold: '\x1b[1m',
  green: '\x1b[32m',
  yellow: '\x1b[33m',
  red: '\x1b[31m',
  cyan: '\x1b[36m',
  magenta: '\x1b[35m',
  blue: '\x1b[34m',
};

// ---- CLI args -----------------------------------------------------------------------

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}

const url = arg('url') ?? 'ws://127.0.0.1:8080/ws';
const token = arg('token') ?? 'dev-token';
const fingerprint = arg('fingerprint');
const secure = /^wss:/i.test(url);
const quiet = process.argv.includes('--quiet');

if (secure && !fingerprint) {
  console.error('wss:// needs --fingerprint <sha256> (the hub console prints it with `webtoken`)');
  process.exit(2);
}

// ---- state --------------------------------------------------------------------------

let client: HubClient;
let prevStatus = '';
let timerTick: ReturnType<typeof setInterval> | null = null;

function state(): HomeState {
  return client.getSnapshot();
}

// ---- resolve entities ---------------------------------------------------------------

function resolveEntity(token: string): Entity | null {
  const s = state();
  const lo = token.toLowerCase();
  const byName = s.entities.find((e) => e.name === lo);
  if (byName) return byName;
  const n = lo.startsWith('0x') ? parseInt(lo, 16) : parseInt(lo, 10);
  if (Number.isFinite(n)) {
    const byId = s.entities.find((e) => e.id === n);
    if (byId) return byId;
  }
  return null;
}

function requireEntity(tok: string): Entity | null {
  const e = resolveEntity(tok);
  if (!e) println(`${C.red}Unknown device: ${tok}${C.reset}`);
  return e;
}

// ---- output -------------------------------------------------------------------------

let rl: readline.Interface | null = null;
let shutdownCalled = false;

function println(text: string): void {
  if (shutdownCalled) return;
  if (rl) {
    process.stdout.write(`\r\x1b[K${text}\n`);
    rl.prompt(true);
  } else {
    console.log(text);
  }
}

function printLive(text: string): void {
  if (!quiet) println(`${C.dim}${text}${C.reset}`);
}

// ---- display helpers ----------------------------------------------------------------

function fmtVal(a: { name: string; value: Value }): string {
  return formatValue(a);
}

function fmtEntity(e: Entity, s: HomeState): string {
  const id = hexId(e.id);
  const name = e.name ? `${C.bold}${e.name}${C.reset}` : `${C.dim}(unnamed)${C.reset}`;
  const type = deviceLabel(e.device);
  const reachable = isReachable(s, e);
  const status = reachable ? '' : ` ${C.red}[offline]${C.reset}`;
  const chip = `chip ${e.chip}`;
  const attrs = e.attrs
    .map((a) => {
      const flags = a.flags.includes('w') ? '' : `${C.dim} (ro)${C.reset}`;
      return `    ${attrLabel(a.name)} (${a.name}, ${C.dim}0x${a.id.toString(16)}${C.reset}): ${fmtVal(a)}${flags}`;
    })
    .join('\n');
  return `  ${C.cyan}${id}${C.reset} ${name}  ${C.dim}${type} · ${chip}${C.reset}${status}\n${attrs}`;
}

function printHome(): void {
  const s = state();
  if (s.entities.length === 0) {
    println('No devices.');
    return;
  }
  const sorted = [...s.entities].sort((a, b) => {
    if ((a.name === null) !== (b.name === null)) return a.name === null ? 1 : -1;
    if (a.name !== null && b.name !== null) return a.name.localeCompare(b.name);
    return a.id - b.id;
  });
  println(sorted.map((e) => fmtEntity(e, s)).join('\n'));
}

function printChips(): void {
  const s = state();
  if (s.chips.length === 0) {
    println('No chips.');
    return;
  }
  for (const c of s.chips) {
    const statusColor = c.status === 'ready' ? C.green : c.status === 'silent' ? C.red : C.yellow;
    const t = c.telemetry;
    const tel = [
      t.heap != null ? `heap ${(t.heap / 1024).toFixed(0)} KB` : null,
      t.uptime_s != null ? `up ${formatUptime(t.uptime_s)}` : null,
      t.tx_frames != null ? `tx ${t.tx_frames}` : null,
      t.crc_errors != null && t.crc_errors > 0 ? `${C.yellow}crc_err ${t.crc_errors}${C.reset}` : null,
    ]
      .filter(Boolean)
      .join(', ');
    println(
      `  ${C.cyan}chip ${c.id}${C.reset} ${C.bold}${c.label}${C.reset}  ${statusColor}${c.status}${C.reset}` +
        `  boot_id=${c.boot_id}` +
        (c.reboots > 0 ? `  ${C.yellow}reboots=${c.reboots}${C.reset}` : '') +
        (tel ? `  ${C.dim}${tel}${C.reset}` : ''),
    );
  }
}

function printHub(): void {
  const s = state();
  const h = s.hub;
  if (!h) {
    println('No hub info yet.');
    return;
  }
  println(`  ${C.bold}${h.name}${C.reset}  fw ${h.fw}`);
  if (h.time) {
    const d = new Date(h.time * 1000);
    println(`  Time: ${d.toLocaleString()}  tz ${h.tz}  ${h.synced ? `${C.green}synced${C.reset}` : `${C.yellow}not synced${C.reset}`}`);
  }
  println(`  Wi-Fi: ${describeWifi(h)}`);
  println(`  Location: ${describeLocation(h)}`);
}

function printRules(): void {
  const s = state();
  if (s.rules.length === 0) {
    println('No rules.');
    return;
  }
  for (const r of s.rules) {
    const d = describeRule(r, s.entities);
    const extras = d.extras.length > 0 ? `\n      ${C.dim}${d.extras.join(' · ')}${C.reset}` : '';
    println(`  ${C.cyan}${r.index + 1}${C.reset}  ${d.when} → ${d.then}${extras}`);
  }
}

function printSchedules(): void {
  const s = state();
  if (s.schedules.length === 0) {
    println('No schedules.');
    return;
  }
  const ctx = { hasLocation: s.hub?.location != null };
  for (const sc of s.schedules) {
    const d = describeSchedule(sc, s.entities, ctx);
    println(`  ${C.cyan}${sc.index + 1}${C.reset}  ${d.title}  ${C.dim}${d.detail}${C.reset}`);
  }
}

function printTimers(): void {
  const s = state();
  if (s.timers.length === 0) {
    println('No timers.');
    return;
  }
  for (const t of s.timers) {
    const d = describeTimer(t, s.entities, t.left_s);
    println(`  ${C.cyan}${t.index + 1}${C.reset}  ${d.title}  ${C.dim}${d.detail}${C.reset}`);
  }
}

function printScenes(): void {
  const s = state();
  if (s.scenes.length === 0) {
    println('No scenes.');
    return;
  }
  for (const sc of s.scenes) {
    const d = describeScene(sc, s.entities);
    println(`  ${C.bold}${d.title}${C.reset}  ${C.dim}${d.detail}${C.reset}`);
  }
}

async function printEvents(count: number): Promise<void> {
  const s = state();
  try {
    const result = (await client.req('history', { limit: Math.min(count, 24) })) as Record<string, unknown>;
    const events = (result.events ?? []) as HistoryEvent[];
    const now = (result.now as number) ?? 0;
    if (events.length === 0) {
      println('No events.');
      return;
    }
    for (const ev of events) {
      const d = describeEvent(ev, s);
      const age = ev.age;
      const timeStr = now > 0 ? new Date((now - age) * 1000).toLocaleTimeString() : `${age}s ago`;
      const toneColor = d.tone === 'bad' ? C.red : d.tone === 'warn' ? C.yellow : '';
      const toneReset = d.tone !== 'normal' ? C.reset : '';
      println(`  ${C.dim}${timeStr}${C.reset}  ${toneColor}${d.text}${toneReset}`);
    }
  } catch (e) {
    println(`${C.red}${describeErr(e)}${C.reset}`);
  }
}

// ---- command helpers ----------------------------------------------------------------

function describeErr(e: unknown): string {
  if (e instanceof HubError) return `Error: ${e.code}${e.message && e.message !== e.code ? ` — ${e.message}` : ''}`;
  return e instanceof Error ? `Error: ${e.message}` : 'Error';
}

async function doCmd(entity: number, attr: number, value: Value): Promise<void> {
  try {
    const status = await client.cmd(entity, attr, value);
    const msg = describeAck(status);
    if (msg) println(`${C.yellow}${msg}${C.reset}`);
    else println(`${C.green}ok${C.reset}`);
  } catch (e) {
    println(`${C.red}${describeErr(e)}${C.reset}`);
  }
}

async function doReq(op: string, args?: Record<string, unknown>): Promise<Record<string, unknown> | null> {
  try {
    return await client.req(op, args);
  } catch (e) {
    println(`${C.red}${describeErr(e)}${C.reset}`);
    return null;
  }
}

// ---- commands -----------------------------------------------------------------------

async function runCommand(line: string): Promise<void> {
  const parts = line.trim().split(/\s+/);
  if (parts.length === 0 || parts[0] === '') return;
  const cmd = parts[0]!.toLowerCase();
  const args = parts.slice(1);

  switch (cmd) {
    case 'home':
    case 'devices':
    case 'list':
      printHome();
      return;

    case 'get': {
      if (args.length === 0) {
        println('Usage: get <entity>');
        return;
      }
      const e = requireEntity(args[0]!);
      if (e) println(fmtEntity(e, state()));
      return;
    }

    case 'on':
    case 'off':
    case 'toggle': {
      if (args.length === 0) {
        println(`Usage: ${cmd} <entity>`);
        return;
      }
      const e = requireEntity(args[0]!);
      if (!e) return;
      const sw = primarySwitch(e);
      if (!sw) {
        println(`${C.red}${deviceTitle(e)} has no switch${C.reset}`);
        return;
      }
      let v: Value;
      if (cmd === 'toggle') v = !sw.value;
      else v = cmd === 'on';
      await doCmd(e.id, sw.id, v);
      return;
    }

    case 'set': {
      if (args.length < 3) {
        println('Usage: set <entity> <attr_name> <value>');
        return;
      }
      const e = requireEntity(args[0]!);
      if (!e) return;
      const attrName = args[1]!.toLowerCase();
      const a = e.attrs.find((x) => x.name === attrName);
      if (!a) {
        println(`${C.red}Unknown attribute: ${attrName}. Known: ${e.attrs.map((x) => x.name).join(', ')}${C.reset}`);
        return;
      }
      if (!a.flags.includes('w')) {
        println(`${C.red}${attrLabel(a.name)} is read-only${C.reset}`);
        return;
      }
      const raw = args.slice(2).join(' ');
      let val: Value;
      if (a.vt === 'bool') {
        if (raw === 'on' || raw === 'true' || raw === '1') val = true;
        else if (raw === 'off' || raw === 'false' || raw === '0') val = false;
        else {
          println(`${C.red}Value for a bool: on/off, true/false, 1/0${C.reset}`);
          return;
        }
      } else {
        val = Number(raw);
        if (!Number.isFinite(val)) {
          println(`${C.red}Not a number: ${raw}${C.reset}`);
          return;
        }
      }
      await doCmd(e.id, a.id, val);
      return;
    }

    case 'chips':
      printChips();
      return;

    case 'hub':
      printHub();
      return;

    case 'rules':
      printRules();
      return;

    case 'schedules':
      printSchedules();
      return;

    case 'timers':
      printTimers();
      return;

    case 'scenes':
      printScenes();
      return;

    case 'events':
    case 'history': {
      const n = args.length > 0 ? parseInt(args[0]!, 10) : 24;
      await printEvents(Number.isFinite(n) && n > 0 ? n : 24);
      return;
    }

    case 'scene': {
      if (args.length === 0) {
        printScenes();
        return;
      }
      if (args[0] === 'del' || args[0] === 'delete') {
        if (args.length < 2) {
          println('Usage: scene del <name>');
          return;
        }
        const result = await doReq('scene.del', { name: args[1] });
        if (result) println(`${C.green}Scene deleted${C.reset}`);
        return;
      }
      if (args[0] === 'apply' || !['del', 'delete'].includes(args[0]!)) {
        const name = args[0] === 'apply' ? args[1] : args[0];
        if (!name) {
          println('Usage: scene <name> | scene apply <name> | scene del <name>');
          return;
        }
        const result = await doReq('scene.apply', { name }) as { sent?: number; skipped?: number } | null;
        if (result) {
          const sent = result.sent ?? 0;
          const skipped = result.skipped ?? 0;
          println(`${C.green}Scene applied${C.reset}: ${sent} sent${skipped > 0 ? `, ${C.yellow}${skipped} skipped${C.reset}` : ''}`);
        }
        return;
      }
      return;
    }

    case 'rule': {
      if (args.length === 0) {
        printRules();
        return;
      }
      if (args[0] === 'del' || args[0] === 'delete') {
        if (args.length < 2) {
          println('Usage: rule del <n>');
          return;
        }
        const idx = parseInt(args[1]!, 10) - 1;
        const result = await doReq('rule.del', { index: idx });
        if (result) println(`${C.green}Rule deleted${C.reset}`);
        return;
      }
      println('Usage: rule | rule del <n>');
      println(`${C.dim}(Adding rules from the CLI is complex — use the app or the brain console)${C.reset}`);
      return;
    }

    case 'schedule': {
      if (args.length === 0) {
        printSchedules();
        return;
      }
      if (args[0] === 'del' || args[0] === 'delete') {
        if (args.length < 2) {
          println('Usage: schedule del <n>');
          return;
        }
        const idx = parseInt(args[1]!, 10) - 1;
        const result = await doReq('schedule.del', { index: idx });
        if (result) println(`${C.green}Schedule deleted${C.reset}`);
        return;
      }
      if (args[0] === 'add') {
        // schedule add <HH:MM|sunset|sunrise[+/-N]> <on|off|toggle> <entity> [daily|weekdays|weekends|mon,tue,...]
        if (args.length < 4) {
          println('Usage: schedule add <time> <on|off|toggle> <entity> [days]');
          println('  time: 22:00, sunset, sunset+30, sunrise-15');
          println('  days: daily (default), weekdays, weekends, or mon,tue,...');
          return;
        }
        const time = args[1]!;
        const action = args[2]!.toLowerCase();
        if (!['on', 'off', 'toggle'].includes(action)) {
          println(`${C.red}Action must be on, off, or toggle${C.reset}`);
          return;
        }
        const e = requireEntity(args[3]!);
        if (!e) return;
        let days: string | string[] = 'daily';
        if (args[4]) {
          const d = args[4].toLowerCase();
          if (d === 'daily' || d === 'weekdays' || d === 'weekends') days = d;
          else days = d.split(',');
        }
        const result = await doReq('schedule.add', { time, entity: e.id, action, days });
        if (result) println(`${C.green}Schedule added${C.reset}`);
        return;
      }
      println('Usage: schedule | schedule add <time> <action> <entity> [days] | schedule del <n>');
      return;
    }

    case 'timer': {
      if (args.length === 0) {
        printTimers();
        return;
      }
      if (args[0] === 'cancel' || args[0] === 'del' || args[0] === 'delete') {
        if (args.length < 2) {
          println('Usage: timer cancel <n>');
          return;
        }
        const idx = parseInt(args[1]!, 10) - 1;
        const result = await doReq('timer.cancel', { index: idx });
        if (result) println(`${C.green}Timer cancelled${C.reset}`);
        return;
      }
      if (args[0] === 'add' || args[0] === 'in') {
        // timer add <seconds> <on|off|toggle> <entity>
        if (args.length < 4) {
          println('Usage: timer add <seconds> <on|off|toggle> <entity>');
          return;
        }
        const delay = parseInt(args[1]!, 10);
        if (!Number.isFinite(delay) || delay < 1) {
          println(`${C.red}Delay must be a positive number of seconds${C.reset}`);
          return;
        }
        const action = args[2]!.toLowerCase();
        if (!['on', 'off', 'toggle'].includes(action)) {
          println(`${C.red}Action must be on, off, or toggle${C.reset}`);
          return;
        }
        const e = requireEntity(args[3]!);
        if (!e) return;
        const result = await doReq('timer.add', { delay_s: delay, entity: e.id, action });
        if (result) println(`${C.green}Timer set${C.reset}`);
        return;
      }
      println('Usage: timer | timer add <seconds> <action> <entity> | timer cancel <n>');
      return;
    }

    case 'in': {
      // Shortcut: in <seconds> <on|off|toggle> <entity>
      if (args.length < 3) {
        println('Usage: in <seconds> <on|off|toggle> <entity>');
        return;
      }
      const delay = parseInt(args[0]!, 10);
      if (!Number.isFinite(delay) || delay < 1) {
        println(`${C.red}Delay must be a positive number of seconds${C.reset}`);
        return;
      }
      const action = args[1]!.toLowerCase();
      if (!['on', 'off', 'toggle'].includes(action)) {
        println(`${C.red}Action must be on, off, or toggle${C.reset}`);
        return;
      }
      const e = requireEntity(args[2]!);
      if (!e) return;
      const result = await doReq('timer.add', { delay_s: delay, entity: e.id, action });
      if (result) println(`${C.green}Timer set${C.reset}`);
      return;
    }

    case 'at': {
      // Shortcut: at <time> <on|off|toggle> <entity> [days]
      if (args.length < 3) {
        println('Usage: at <time> <on|off|toggle> <entity> [days]');
        return;
      }
      const time = args[0]!;
      const action = args[1]!.toLowerCase();
      if (!['on', 'off', 'toggle'].includes(action)) {
        println(`${C.red}Action must be on, off, or toggle${C.reset}`);
        return;
      }
      const e = requireEntity(args[2]!);
      if (!e) return;
      let days: string | string[] = 'daily';
      if (args[3]) {
        const d = args[3].toLowerCase();
        if (d === 'daily' || d === 'weekdays' || d === 'weekends') days = d;
        else days = d.split(',');
      }
      const result = await doReq('schedule.add', { time, entity: e.id, action, days });
      if (result) println(`${C.green}Schedule added${C.reset}`);
      return;
    }

    case 'name': {
      if (args.length === 0) {
        const s = state();
        const named = s.entities.filter((e) => e.name);
        if (named.length === 0) {
          println('No named devices.');
          return;
        }
        for (const e of named) println(`  ${hexId(e.id)} → ${C.bold}${e.name}${C.reset}`);
        return;
      }
      if (args[0] === 'clear') {
        if (args.length < 2) {
          println('Usage: name clear <entity>');
          return;
        }
        const e = requireEntity(args[1]!);
        if (!e) return;
        const result = await doReq('name.clear', { entity: e.id });
        if (result) println(`${C.green}Name cleared${C.reset}`);
        return;
      }
      if (args.length < 2) {
        println('Usage: name <entity> <new_name> | name clear <entity>');
        return;
      }
      const e = requireEntity(args[0]!);
      if (!e) return;
      const result = await doReq('name.set', { entity: e.id, name: args[1] });
      if (result) println(`${C.green}Renamed to ${args[1]}${C.reset}`);
      return;
    }

    case 'pair': {
      const sec = args.length > 0 ? parseInt(args[0]!, 10) : undefined;
      const result = await doReq('pair', sec ? { seconds: sec } : undefined);
      if (result) println(`${C.green}Pairing mode opened${C.reset}`);
      return;
    }

    case 'unpair': {
      if (args.length === 0) {
        println('Usage: unpair <entity>');
        return;
      }
      const e = requireEntity(args[0]!);
      if (!e) return;
      const result = await doReq('unpair', { entity: e.id });
      if (result) println(`${C.green}Device removed${C.reset}`);
      return;
    }

    case 'identify':
    case 'blink': {
      if (args.length === 0) {
        println('Usage: identify <entity>');
        return;
      }
      const e = requireEntity(args[0]!);
      if (!e) return;
      const result = await doReq('identify', { entity: e.id });
      if (result) println(`${C.green}Blinking for ~10 s${C.reset}`);
      return;
    }

    case 'wifi': {
      if (args.length === 0) {
        const h = state().hub;
        println(`  ${describeWifi(h)}`);
        return;
      }
      if (args[0] === 'scan') {
        println(`${C.dim}Scanning…${C.reset}`);
        const result = (await doReq('wifi.scan')) as { networks?: Array<{ ssid: string; rssi: number; secure: boolean }> } | null;
        if (result?.networks) {
          if (result.networks.length === 0) {
            println('No networks found.');
            return;
          }
          for (const n of result.networks) {
            println(`  ${n.ssid}  ${C.dim}${n.rssi} dBm (${signalText(n.rssi)})${n.secure ? '' : `  ${C.yellow}open${C.reset}`}${C.reset}`);
          }
        }
        return;
      }
      println('Usage: wifi | wifi scan');
      println(`${C.dim}(To change the network use the app or the brain console — it is disruptive)${C.reset}`);
      return;
    }

    case 'loc':
    case 'location': {
      if (args.length === 0) {
        println(`  ${describeLocation(state().hub)}`);
        return;
      }
      if (args[0] === 'clear') {
        const result = await doReq('location.clear');
        if (result) println(`${C.green}Location cleared${C.reset}`);
        return;
      }
      if (args.length < 2) {
        println('Usage: loc <lat> <lon> | loc clear');
        return;
      }
      const lat = parseFloat(args[0]!);
      const lon = parseFloat(args[1]!);
      if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
        println(`${C.red}Latitude and longitude must be numbers${C.reset}`);
        return;
      }
      const result = await doReq('location.set', { lat, lon });
      if (result) println(`${C.green}Location set${C.reset}`);
      return;
    }

    case 'status': {
      const s = state();
      println(`  Connection: ${s.status}${s.error ? ` (${s.error})` : ''}`);
      println(`  Entities: ${s.entities.length}  Chips: ${s.chips.length}  Rules: ${s.rules.length}`);
      println(`  Schedules: ${s.schedules.length}  Timers: ${s.timers.length}  Scenes: ${s.scenes.length}`);
      return;
    }

    case 'help':
    case '?':
      println(`${C.bold}Devices${C.reset}`);
      println('  home                           List all devices and values');
      println('  get <device>                   Show one device');
      println('  on/off/toggle <device>         Switch a device');
      println('  set <device> <attr> <value>    Set an attribute');
      println('');
      println(`${C.bold}Automations${C.reset}`);
      println('  rules                          List rules');
      println('  rule del <n>                   Delete rule #n');
      println('  schedules                      List schedules');
      println('  schedule add <time> <action> <device> [days]');
      println('  schedule del <n>               Delete schedule #n');
      println('  at <time> <action> <device> [days]   (shortcut for schedule add)');
      println('  timers                         List timers');
      println('  timer add <sec> <action> <device>');
      println('  timer cancel <n>               Cancel timer #n');
      println('  in <sec> <action> <device>     (shortcut for timer add)');
      println('  scenes                         List scenes');
      println('  scene <name>                   Apply a scene');
      println('  scene del <name>               Delete a scene');
      println('');
      println(`${C.bold}Hub${C.reset}`);
      println('  hub                            Hub info (time, Wi-Fi, location)');
      println('  chips                          Chip health and telemetry');
      println('  events [n]                     Recent events (default 24)');
      println('  wifi                           Current network');
      println('  wifi scan                      Scan for networks');
      println('  loc <lat> <lon>                Set location');
      println('  loc clear                      Clear location');
      println('');
      println(`${C.bold}Devices management${C.reset}`);
      println('  name <device> <name>           Name a device');
      println('  name clear <device>            Clear a name');
      println('  pair [sec]                     Open network for pairing');
      println('  unpair <device>                Remove a device');
      println('  identify <device>              Blink a device (~10 s)');
      println('');
      println(`${C.bold}Other${C.reset}`);
      println('  status                         Connection state summary');
      println('  help                           This list');
      println('  quit / exit / Ctrl-C           Disconnect and exit');
      println('');
      println(`${C.dim}A <device> is a name (plug1) or a hex/decimal id (0x0100, 256).${C.reset}`);
      println(`${C.dim}Live state updates are shown as they happen (--quiet to suppress).${C.reset}`);
      return;

    case 'quit':
    case 'exit':
      shutdown();
      return;

    default:
      println(`${C.red}Unknown command: ${cmd}${C.reset}. Type ${C.bold}help${C.reset} for the list.`);
  }
}

// ---- live events --------------------------------------------------------------------

function onStateChange(): void {
  const s = state();

  // Status transitions
  if (s.status !== prevStatus) {
    const statusColor = s.status === 'ready' ? C.green : s.status === 'connecting' || s.status === 'reconnecting' || s.status === 'authenticating' ? C.yellow : C.red;
    println(`${statusColor}[${s.status}]${C.reset}${s.error ? ` ${s.error}` : ''}`);
    if (s.status === 'ready' && prevStatus !== 'ready') {
      const ents = s.entities.length;
      const chips = s.chips.length;
      println(`${C.dim}Connected. ${ents} device${ents !== 1 ? 's' : ''}, ${chips} chip${chips !== 1 ? 's' : ''}. Type ${C.reset}help${C.dim} for commands.${C.reset}`);
    }
    prevStatus = s.status;
  }
}

function onHistoryPush(push: { events: HistoryEvent[]; now: number }): void {
  if (quiet) return;
  const s = state();
  for (const ev of [...push.events].reverse()) {
    const d = describeEvent(ev, s);
    const timeStr = push.now > 0 ? new Date((push.now - ev.age) * 1000).toLocaleTimeString() : 'now';
    const toneColor = d.tone === 'bad' ? C.red : d.tone === 'warn' ? C.yellow : C.dim;
    const toneReset = C.reset;
    printLive(`${timeStr}  ${toneColor}${d.text}${toneReset}`);
  }
}

// ---- main ---------------------------------------------------------------------------

function shutdown(): void {
  if (shutdownCalled) return;
  shutdownCalled = true;
  if (timerTick) clearInterval(timerTick);
  client.close();
  rl?.close();
  process.exit(0);
}

function main(): void {
  console.log(`${C.bold}Keystone Console${C.reset}  ${C.dim}${url}${C.reset}`);

  client = new HubClient({
    url,
    token,
    clientName: 'keystone-console',
    socketFactory: pinnedSocketFactory,
    ...(fingerprint ? { pinnedFingerprint: fingerprint } : {}),
    reconnect: true,
    staleAfterMs: 35_000,
  });

  client.subscribe(onStateChange);
  client.subscribeHistory(onHistoryPush);
  client.connect();

  rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    prompt: `${C.cyan}keystone>${C.reset} `,
    terminal: true,
  });

  rl.on('line', (line) => {
    runCommand(line).catch((e) => println(`${C.red}${e}${C.reset}`));
  });
  rl.on('close', shutdown);
  rl.on('SIGINT', shutdown);

  // Wait for ready before showing the prompt
  const unsub = client.subscribe(() => {
    if (state().status === 'ready') {
      unsub();
      rl!.prompt();
    }
  });
}

main();

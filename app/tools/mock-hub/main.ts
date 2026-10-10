// Run the mock hub for manual development: `npm run mock-hub [-- --port 8080 --token secret]`.
// It also drifts power_w and flips the occupancy sensor now and then so a UI has
// live events to show.
import { MockHub } from './hub';

function arg(name: string, fallback: string): string {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] ? (process.argv[i + 1] as string) : fallback;
}

const port = Number(arg('port', '8080'));
const token = arg('token', 'dev-token');
const hub = await MockHub.start({ port, token });
console.log(`mock hub listening on ${hub.url} (token: ${token})`);

setInterval(() => {
  hub.setValue(0x0100, 0x13, Math.round(Math.random() * 40));
}, 3000);
setInterval(() => {
  const cur = hub.entities.find((e) => e.id === 0x0102)?.attrs[0]?.value;
  hub.setValue(0x0102, 0x50, !cur);
}, 11000);

process.on('SIGINT', () => {
  void hub.close().then(() => process.exit(0));
});

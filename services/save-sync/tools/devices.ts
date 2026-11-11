import { createHash, randomBytes } from 'node:crypto';
import { readFileSync, writeFileSync, chmodSync, mkdirSync, renameSync, unlinkSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

// Only token digests go to the Worker. This tool never reads Cloudflare secrets.
// Keep the private registry and generated connection files outside the repository.
const args = process.argv.slice(2);
function option(name: string, fallback: string) {
  const at = args.indexOf(name); if (at < 0) return fallback;
  if (!args[at + 1]) throw Error('Missing ' + name); return args.splice(at, 2)[1];
}
const registry = resolve(option('--registry', 'devices.private.json'));
const config = resolve(option('--config', 'wrangler.jsonc'));
const testing = args.includes('--test'); if (testing) args.splice(args.indexOf('--test'), 1);
const [action, name, endpoint] = args;
if (!['add', 'revoke', 'list', 'publish'].includes(action)) throw Error('Use add <name> <https-origin>, revoke <id>, list, or publish; options: --registry <private-path> --config <wrangler-config> --test');
mkdirSync(dirname(registry), { recursive: true, mode: 0o700 });
const lock = registry + '.lock'; writeFileSync(lock, '', { flag: 'wx', mode: 0o600 });
try {
  let devices: Record<string, { hash: string; name: string }> = {};
  try { devices = JSON.parse(readFileSync(registry, 'utf8')); } catch (e: any) { if (e.code !== 'ENOENT') throw e; }
  if (action === 'add') {
    const url = new URL(endpoint);
    if (!name || name.length > 80 || /[\x00-\x1f\x7f]/.test(name) || url.protocol !== 'https:' || url.origin !== endpoint)
      throw Error('A device name and HTTPS origin without a path are required');
    const id = randomBytes(16).toString('hex'), token = randomBytes(32).toString('base64url');
    devices[id] = { name, hash: createHash('sha256').update(token).digest('hex') };
    const file = resolve(dirname(registry), 'connection-' + id + '.txt');
    writeFileSync(file, JSON.stringify({ url: endpoint, device: id, token, ...(testing ? { test: true } : {}) }), { mode: 0o600, flag: 'wx' });
    console.log('Connection file: ' + file + '\nDevice: ' + id);
  } else if (action === 'revoke') {
    if (!name || !devices[name]) throw Error('Unknown device ID'); delete devices[name];
  } else if (action === 'list') {
    for (const [id, d] of Object.entries(devices)) console.log(id + ' ' + d.name);
  } else {
    const cli = fileURLToPath(new URL('../node_modules/wrangler/bin/wrangler.js', import.meta.url));
    const result = spawnSync(process.execPath, [cli, 'secret', 'put', 'DEVICES', '--config', config], {
      input: JSON.stringify(devices), stdio: ['pipe', 'inherit', 'inherit']
    });
    if (result.error || result.status !== 0) throw Error('Device registry was not published; local registry is unchanged');
  }
  if (action === 'add' || action === 'revoke') {
    writeFileSync(registry + '.new', JSON.stringify(devices, null, 2) + '\n', { mode: 0o600 });
    chmodSync(registry + '.new', 0o600); renameSync(registry + '.new', registry);
    console.log('Local registry saved. Run publish with the same --registry and --config to apply it.');
  }
} finally { unlinkSync(lock); }

import { randomBytes, webcrypto } from 'node:crypto';
import { zipSync } from 'fflate';
import { handle, sha, type Env } from '../src/index.ts';
if (!globalThis.crypto) Object.defineProperty(globalThis, 'crypto', { value: webcrypto });
export class Bucket {
  objects = new Map<string, { bytes: Uint8Array; etag: string; uploaded: Date; customMetadata: Record<string, string> }>();
  async get(key: string) {
    const o = this.objects.get(key); if (!o) return null;
    return { ...o, key, size: o.bytes.length, body: new Response(new Uint8Array(o.bytes).buffer).body, arrayBuffer: async () => o.bytes.slice().buffer,
      json: async () => JSON.parse(new TextDecoder().decode(o.bytes)), text: async () => new TextDecoder().decode(o.bytes) };
  }
  async head(key: string) { return this.get(key); }
  async put(key: string, value: string | Uint8Array, options: any = {}) {
    const bytes = typeof value === 'string' ? new TextEncoder().encode(value) : value.slice();
    const digest = await sha(bytes);
    if (options.sha256 && options.sha256 !== digest) throw new Error('checksum');
    const existing = this.objects.get(key);
    if (options.onlyIf?.etagMatches && existing?.etag !== options.onlyIf.etagMatches) return null;
    if (options.onlyIf?.etagDoesNotMatch === '*' && existing) return null;
    this.objects.set(key, { bytes, etag: digest, uploaded: new Date(), customMetadata: options.customMetadata ?? {} });
    return this.get(key);
  }
  async delete(key: string) { this.objects.delete(key); }
  async list(options: { prefix: string }) {
    return { objects: [...this.objects].filter(([k]) => k.startsWith(options.prefix)).map(([key, v]) => ({ key, ...v })), truncated: false };
  }
}
export const id = () => randomBytes(16).toString('hex');
export async function environment() {
  const bucket = new Bucket(), device = id(), token = randomBytes(32).toString('base64url');
  const env = { SAVES: bucket as unknown as R2Bucket, DEVICES: JSON.stringify({ [device]: { hash: await sha(token), name: 'test-device' } }) };
  return { bucket, device, token, env };
}
export function character(level = 5) {
  const b = new Uint8Array(512), v = new DataView(b.buffer);
  v.setUint32(0, 0xaa55aa55, true); v.setUint32(4, 105, true); v.setUint32(8, b.length, true); b[0x18] = 7; b[0x1b] = level;
  b.set(new TextEncoder().encode('hero'), 0x12b);
  let checksum = 0; for (const x of b) checksum = (((checksum << 1) | (checksum >>> 31)) + x) >>> 0;
  v.setUint32(12, checksum, true); return b;
}
export async function archive(device: string, level = 5, extra: Record<string, Uint8Array> = {}) {
  const files: Record<string, Uint8Array> = level ? { 'hero/hero.d2s': character(level), ...extra } : extra;
  const manifest: any = { protocol: 1, format: 1, game: '3.3.94012', device, parent: null, savedAt: Date.now(), source: 'synthetic-test',
    characters: level ? [{ name: 'hero', level, classId: 7 }] : [], files: [] };
  let canonical = '';
  for (const path of Object.keys(files).sort((a,b)=>Buffer.compare(Buffer.from(a),Buffer.from(b)))) { const digest = await sha(files[path]); canonical += path + '\0' + digest + '\n'; manifest.files.push({ path, size: files[path].length, sha256: digest }); }
  manifest.contentHash = await sha(canonical);
  return zipSync({ 'manifest.json': new TextEncoder().encode(JSON.stringify(manifest)),
    ...Object.fromEntries(Object.entries(files).map(([k, v]) => ['save/' + k, v])) }, { level: 0 });
}
export async function call(env: Env, token: string, method: string, path: string, body?: unknown) {
  return handle(new Request('https://sync.test' + path, { method, headers: { Authorization: 'Bearer ' + token },
    body: body === undefined ? undefined : body instanceof Uint8Array ? new Uint8Array(body).buffer : JSON.stringify(body) }), env);
}

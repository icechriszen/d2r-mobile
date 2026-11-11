import { unzipSync } from 'fflate';

export interface Env { SAVES: R2Bucket; DEVICES: string }
type Device = { id: string; name: string };
type Hero = { name: string; level: number; classId: number };
type Manifest = {
  protocol: 1; game: '3.3.94012'; format: 1; device: string; source: string;
  savedAt: number; contentHash: string; characters: Hero[];
  parent: string | null;
  files: { path: string; size: number; sha256: string }[];
};
type Revision = {
  id: string; snapshot: string; parent: string | null; contentHash: string;
  device: Device; createdAt: number; savedAt: number; characters: Hero[];
};
type Operation = { fingerprint: string; revision?: Revision; conflict?: string };
type Index = { head: Revision | null; history: Revision[]; conflicts: Revision[]; operations: Record<string, Operation> };
const KEY = 'v1/index.json', DAY = 86400000, MAX = 16 * 1024 * 1024;
const idOK = (s: unknown): s is string => typeof s === 'string' && /^[a-f0-9]{32}$/.test(s);
const hashOK = (s: unknown): s is string => typeof s === 'string' && /^[a-f0-9]{64}$/.test(s);
const blank = (): Index => ({ head: null, history: [], conflicts: [], operations: {} });
class Fault extends Error { constructor(public status: number, public code: string) { super(code); } }
function fail(status: number, code: string): never { throw new Fault(status, code); }
function response(body: unknown, status = 200) {
  return new Response(JSON.stringify(body), { status, headers: {
    'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store'
  } });
}
export async function sha(data: Uint8Array | string): Promise<string> {
  const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : data;
  return [...new Uint8Array(await crypto.subtle.digest('SHA-256', new Uint8Array(bytes).buffer))].map(x => x.toString(16).padStart(2, '0')).join('');
}
export function allowedPath(p: string): boolean {
  if (typeof p !== 'string' || !p || new TextEncoder().encode(p).length > 240 || /[\x00-\x1f\x7f\\:]/.test(p)) return false;
  const parts = p.split('/');
  if (parts.some(x => !x || x === '.' || x === '..' || x.startsWith('.'))) return false;
  const first = parts[0];
  if (/^(SharedStash|ModernSharedStash)[^/]*\.(d2i|swvault)$/.test(p)) return true;
  if (/^ModernSharedStash[A-Za-z0-9_-]*$/.test(first)) return parts.length === 2;
  return parts.length === 2 &&
    !['settings', 'temp', 'logs', 'cache'].includes(first.toLowerCase()) &&
    !first.toLowerCase().startsWith('save.') && !first.startsWith('_');
}
function validateSidecar(path: string, bytes: Uint8Array) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.length), n = bytes.length;
  const magic = (at: number, text: string) => [...text].every((c, i) => bytes[at + i] === c.charCodeAt(0));
  if (path.endsWith('.d2i')) {
    if (!n) fail(422, 'invalid_stash');
    for (let at = 0; at < n;) {
      if (n - at < 68 || v.getUint32(at, true) !== 0xaa55aa55 || v.getUint32(at + 8, true) !== 105) fail(422, 'incompatible_stash');
      const len = v.getUint32(at + 16, true);
      if (len < 68 || len > n - at || (!magic(at + 64, 'JM') &&
        (len < 76 || v.getUint32(at + 64, true) !== 0xc0eaedc0 || v.getUint16(at + 68, true) !== 1))) fail(422, 'invalid_stash');
      at += len;
    }
  }
  const vault = path.endsWith('.swvault'), pouch = path.endsWith('.swpouch');
  if (vault || pouch) {
    if (n < 8 || !magic(0, vault ? 'SWVT' : 'SWPU') || (vault ? v.getUint16(4, true) !== 1 : bytes[4] !== 1)) fail(422, 'incompatible_sidecar');
    let at = 8;
    for (let i = 0, count = v.getUint16(6, true); i < count; i++) {
      if (n - at < 6) fail(422, 'invalid_sidecar');
      const len = v.getUint16(at + 4, true); at += 6;
      if (!len || len > n - at) fail(422, 'invalid_sidecar'); at += len;
    }
    if (at !== n) fail(422, 'invalid_sidecar');
  }
  if (path.endsWith('.swpage') && (n < 8 || !magic(0, 'SWPG') || ![1, 2].includes(bytes[4]))) fail(422, 'incompatible_sidecar');
}
const crcTable = Uint32Array.from({ length: 256 }, (_, i) => {
  let c = i; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (c & 1 ? 0xedb88320 : 0); return c >>> 0;
});
function checkZipStructure(b: Uint8Array) {
  const n = b.length, v = new DataView(b.buffer, b.byteOffset, n);
  const u16 = (p: number) => v.getUint16(p, true), u32 = (p: number) => v.getUint32(p, true);
  if (n < 22 || u32(n - 22) !== 0x06054b50 || u16(n - 18) || u16(n - 16) || u16(n - 2)) fail(422, 'invalid_zip');
  const count = u16(n - 12), cd = u32(n - 6); let at = cd, next = 0;
  if (count > 2049 || count !== u16(n - 14) || cd + u32(n - 10) !== n - 22) fail(422, 'invalid_zip');
  for (let i = 0; i < count; i++) {
    if (n - 22 - at < 46 || u32(at) !== 0x02014b50) fail(422, 'invalid_zip');
    const len = u16(at + 28), size = u32(at + 24), off = u32(at + 42);
    if (!len || len > 255 || u16(at + 10) || u16(at + 30) || u16(at + 32) || u16(at + 34) ||
      (u16(at + 8) & ~0x800) || u32(at + 20) !== size || at + 46 + len > n - 22 || off !== next || off + 30 > cd) fail(422, 'invalid_zip');
    if (u32(off) !== 0x04034b50 || u16(off + 8) || u16(off + 28) || u16(off + 26) !== len ||
      u16(off + 6) !== u16(at + 8) || u32(off + 14) !== u32(at + 16) || u32(off + 18) !== size ||
      u32(off + 22) !== size || off + 30 + len + size > cd) fail(422, 'invalid_zip');
    for (let k = 0; k < len; k++) if (!b[at + 46 + k] || b[at + 46 + k] !== b[off + 30 + k]) fail(422, 'invalid_zip');
    let crc = 0xffffffff;
    for (let k = off + 30 + len; k < off + 30 + len + size; k++) crc = (crc >>> 8) ^ crcTable[(crc ^ b[k]) & 255];
    if ((crc ^ 0xffffffff) >>> 0 !== u32(at + 16)) fail(422, 'zip_checksum');
    next = off + 30 + len + size; at += 46 + len;
  }
  if (at !== n - 22 || next !== cd) fail(422, 'invalid_zip');
}
export async function validateZip(bytes: Uint8Array): Promise<Manifest> {
  if (bytes.length > MAX) fail(413, 'snapshot_too_large');
  checkZipStructure(bytes);
  let total = 0, count = 0;
  const entries = unzipSync(bytes, { filter(file) {
    total += file.originalSize; count++;
    if (total > MAX || count > 2049 || file.originalSize < 0 || file.compression !== 0) fail(413, 'expanded_limit');
    return true;
  } });
  const raw = entries['manifest.json'];
  if (!raw || raw.length > 512 * 1024) fail(422, 'manifest_missing');
  const m = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(raw)) as Manifest;
  if (!m || typeof m !== 'object' || m.protocol !== 1 || m.format !== 1 || m.game !== '3.3.94012') fail(422, 'incompatible_format');
  if (!idOK(m.device) || !hashOK(m.contentHash) || !Number.isSafeInteger(m.savedAt) || m.savedAt<0 || m.savedAt>253402300799999 ||
      !(m.parent === null || idOK(m.parent)) || typeof m.source !== 'string' || m.source.length > 128 ||
      !Array.isArray(m.files) || m.files.length > 2048 || !Array.isArray(m.characters) || m.characters.length > 128)
    fail(422, 'invalid_manifest');
  if (!m.files.every(f => f && typeof f === 'object' && typeof f.path === 'string') ||
      !m.characters.every(h => h && typeof h === 'object')) fail(422, 'invalid_manifest');
  const names = new Set<string>(), folded = new Set<string>();
  let canonical = '';
  // The native manifest sorts UTF-8 bytes (strcmp), not UTF-16 code units.
  const encoded = new Map(m.files.map(f => [f.path, new TextEncoder().encode(f.path)]));
  const ordered = [...m.files].sort((a, b) => {
    const x=encoded.get(a.path)!,y=encoded.get(b.path)!;
    for(let i=0;i<Math.min(x.length,y.length);i++)if(x[i]!==y[i])return x[i]-y[i];
    return x.length-y.length;
  });
  for (const f of ordered) {
    if (!allowedPath(f.path) || !hashOK(f.sha256) || !Number.isSafeInteger(f.size) || f.size < 0 ||
        names.has(f.path) || folded.has(f.path.normalize('NFC').toLowerCase())) fail(422, 'invalid_path');
    names.add(f.path); folded.add(f.path.normalize('NFC').toLowerCase());
    const data = entries['save/' + f.path];
    if (!data || data.length !== f.size || await sha(data) !== f.sha256) fail(422, 'file_checksum');
    validateSidecar(f.path, data);
    canonical += f.path + '\0' + f.sha256 + '\n';
  }
  if (Object.keys(entries).length !== m.files.length + 1 || count !== m.files.length + 1 ||
      await sha(canonical) !== m.contentHash) fail(422, 'content_checksum');
  for (const hero of m.characters) {
    if (typeof hero.name !== 'string' || !names.has(hero.name + '/' + hero.name + '.d2s') ||
        !Number.isInteger(hero.level) || hero.level < 1 || hero.level > 99 ||
        !Number.isInteger(hero.classId) || hero.classId < 0 || hero.classId > 7) fail(422, 'invalid_character');
    const bytes = entries['save/' + hero.name + '/' + hero.name + '.d2s'];
    if (bytes.length < 0x15b) fail(422, 'invalid_character');
    const data = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let checksum = 0;
    for (let i = 0; i < bytes.length; i++) checksum = (((checksum << 1) | (checksum >>> 31)) + (i >= 12 && i < 16 ? 0 : bytes[i])) >>> 0;
    if (data.getUint32(0, true) !== 0xaa55aa55 || data.getUint32(4, true) !== 105 ||
        data.getUint32(8, true) !== bytes.length || data.getUint32(12, true) !== checksum ||
        bytes[0x18] !== hero.classId || bytes[0x1b] !== hero.level) fail(422, 'invalid_character');
  }
  if (new Set(m.characters.map(h => h.name)).size !== m.characters.length ||
      m.files.filter(f => f.path.endsWith('.d2s')).length !== m.characters.length) fail(422, 'invalid_character_list');
  return m;
}
async function authenticate(req: Request, env: Env): Promise<Device> {
  const token = req.headers.get('Authorization')?.match(/^Bearer ([A-Za-z0-9_-]{43})$/)?.[1];
  if (!token) fail(401, 'unauthorized');
  const digest = await sha(token);
  const devices = JSON.parse(env.DEVICES || '{}') as Record<string, { hash: string; name: string }>;
  for (const [id, d] of Object.entries(devices)) if (idOK(id) && d.hash === digest) return { id, name: d.name };
  return fail(401, 'unauthorized');
}
async function readIndex(env: Env): Promise<{ value: Index; etag?: string }> {
  const o = await env.SAVES.get(KEY);
  return o ? { value: await o.json<Index>(), etag: o.etag } : { value: blank() };
}
async function writeIndex(env: Env, value: Index, etag?: string): Promise<boolean> {
  return !!await env.SAVES.put(KEY, JSON.stringify(value), {
    onlyIf: etag ? { etagMatches: etag } : { etagDoesNotMatch: '*' },
    httpMetadata: { contentType: 'application/json', cacheControl: 'no-store' }
  });
}
async function bodyJSON(req: Request) {
  const reader=req.body?.getReader();if(!reader)fail(400,'body_required');
  const chunks:Uint8Array[]=[];let size=0;
  for(;;){const p=await reader.read();if(p.done)break;size+=p.value.length;
    if(size>16384){await reader.cancel();fail(413,'request_too_large');}chunks.push(p.value);}
  const bytes=new Uint8Array(size);let at=0;for(const c of chunks){bytes.set(c,at);at+=c.length;}
  const body=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes));
  if(!body||typeof body!=='object'||Array.isArray(body))fail(400,'invalid_request');
  return body as Record<string, unknown>;
}
function retained(index: Index, now: number) {
  index.history = index.history.filter((r, i) => i < 50 || r.createdAt >= now - 30 * DAY);
}
async function publish(req: Request, env: Env, device: Device, resolve: boolean) {
  const b = await bodyJSON(req);
  if (!idOK(b.operation) || !idOK(b.snapshot) ||
      !(b.base === null || idOK(b.base))) fail(400, 'invalid_request');
  const operation = b.operation, snapshot = b.snapshot, fingerprint = await sha(JSON.stringify({
    operation, snapshot, base: b.base, resolve, device: device.id,
    confirmEmpty: b.confirmEmpty === true, conflicts: b.conflicts ?? []
  }));
  // The receipt outlives snapshot retention. A late retry still returns the
  // original outcome without republishing a version or requiring its ZIP.
  const receipt = (await readIndex(env)).value.operations[operation];
  if (receipt) {
    if (receipt.fingerprint !== fingerprint) fail(409, 'operation_reused');
    const { value } = await readIndex(env);
    return response(receipt.revision ? { revision: receipt.revision } :
      { error: 'conflict', conflict: receipt.conflict, head: value.head }, receipt.revision ? 200 : 409);
  }
  const obj = await env.SAVES.get('v1/snapshots/' + snapshot);
  if (!obj) fail(404, 'snapshot_missing');
  const manifest = await validateZip(new Uint8Array(await obj.arrayBuffer()));
  for (let attempt = 0; attempt < 12; attempt++) {
    const { value: index, etag } = await readIndex(env);
    const previous = index.operations[operation];
    if (previous) {
      if (previous.fingerprint !== fingerprint) fail(409, 'operation_reused');
      return response(previous.revision ? { revision: previous.revision } :
        { error: 'conflict', conflict: previous.conflict, head: index.head }, previous.revision ? 200 : 409);
    }
    if (resolve) {
      if (![...index.history, ...index.conflicts].some(r => r.snapshot === snapshot) &&
          !(manifest.device === device.id && Date.now() - obj.uploaded.getTime() < DAY))
        fail(409, 'version_no_longer_available');
      if ((index.head?.id ?? null) !== b.base) return response({ error: 'head_changed', head: index.head }, 409);
    } else {
      if (manifest.device !== device.id) fail(403, 'wrong_device');
      // Unpublished objects cannot be revived while the orphan collector deletes them.
      if (Date.now() - obj.uploaded.getTime() > DAY) fail(410, 'upload_expired');
    }
    const now = Date.now();
    const revision: Revision = { id: operation, snapshot, parent: b.base as string | null,
      contentHash: manifest.contentHash, device, createdAt: now, savedAt: manifest.savedAt, characters: manifest.characters };
    if (!resolve && (index.head?.id ?? null) !== b.base) {
      index.conflicts.push(revision);
      index.operations[operation] = { fingerprint, conflict: revision.id };
      if (await writeIndex(env, index, etag)) return response({ error: 'conflict', conflict: revision.id, head: index.head }, 409);
      continue;
    }
    if (!manifest.characters.length && index.head?.characters.length && b.confirmEmpty !== true)
      return response({ error: 'confirm_empty_required' }, 409);
    index.head = revision; index.history.unshift(revision);
    index.operations[operation] = { fingerprint, revision };
    if (resolve && Array.isArray(b.conflicts)) {
      const ids = new Set(b.conflicts.filter(idOK));
      const resolved = index.conflicts.filter(r => ids.has(r.id));
      index.conflicts = index.conflicts.filter(r => !ids.has(r.id));
      // Losing versions remain recoverable in history for at least 30 days.
      index.history.push(...resolved.map(r => ({ ...r, createdAt: now })));
      index.history.sort((a, z) => z.createdAt - a.createdAt);
    }
    retained(index, now);
    if (await writeIndex(env, index, etag)) return response({ revision }, 201);
  }
  return response({ error: 'busy_retry' }, 503);
}
export async function handle(req: Request, env: Env): Promise<Response> {
  try {
    if (new URL(req.url).protocol !== 'https:' && new URL(req.url).hostname !== 'localhost')
      fail(400, 'https_required');
    const device = await authenticate(req, env);
    const path = new URL(req.url).pathname;
    if (req.method === 'GET' && path === '/v1/status') {
      const { value } = await readIndex(env);
      return response({ protocol: 1, format: 1, game: '3.3.94012', head: value.head, conflicts: value.conflicts });
    }
    if (req.method === 'GET' && path === '/v1/history') {
      const offset = Number(new URL(req.url).searchParams.get('offset') || 0);
      if (!Number.isSafeInteger(offset) || offset < 0) fail(400, 'invalid_offset');
      const { value } = await readIndex(env), items = value.history.slice(offset, offset + 50);
      return response({ items, conflicts: value.conflicts,
        next: offset + items.length < value.history.length ? offset + items.length : null });
    }
    const match = path.match(/^\/v1\/snapshots\/([a-f0-9]{32})$/);
    if (match && req.method === 'PUT') {
      if (Number(req.headers.get('Content-Length')) > MAX) fail(413, 'snapshot_too_large');
      const reader = req.body?.getReader(); if (!reader) fail(400, 'body_required');
      const chunks: Uint8Array[] = []; let size = 0;
      while (true) { const part = await reader.read(); if (part.done) break; size += part.value.length;
        if (size > MAX) { await reader.cancel(); fail(413, 'snapshot_too_large'); } chunks.push(part.value); }
      const bytes = new Uint8Array(size); let at = 0; for (const c of chunks) { bytes.set(c, at); at += c.length; }
      const m = await validateZip(bytes); if (m.device !== device.id) fail(403, 'wrong_device');
      const digest = await sha(bytes), key = 'v1/snapshots/' + match[1];
      const stored = await env.SAVES.put(key, bytes, { onlyIf: { etagDoesNotMatch: '*' },
        sha256: digest, customMetadata: { sha256: digest }, httpMetadata: { contentType: 'application/zip' } });
      if (!stored && (await env.SAVES.head(key))?.customMetadata?.sha256 !== digest) fail(409, 'snapshot_immutable');
      return response({ snapshot: match[1], sha256: digest }, stored ? 201 : 200);
    }
    if (match && req.method === 'GET') {
      const o = await env.SAVES.get('v1/snapshots/' + match[1]); if (!o) fail(404, 'snapshot_missing');
      return new Response(o.body, { headers: { 'Content-Type': 'application/zip',
        'Cache-Control': 'no-store', 'X-Snapshot-SHA256': o.customMetadata?.sha256 || '' } });
    }
    if (path === '/v1/commit' && req.method === 'POST') return await publish(req, env, device, false);
    if (path === '/v1/resolve' && req.method === 'POST') return await publish(req, env, device, true);
    return response({ error: 'not_found' }, 404);
  } catch (e) {
    if (e instanceof Fault) return response({ error: e.code }, e.status);
    if (e instanceof SyntaxError || typeof e === 'string' || (e as { code?: number })?.code)
      return response({ error: 'invalid_snapshot_or_json' }, 422);
    // Never log authorization headers, save bytes, or token-bearing requests.
    console.error('save-sync request failed', e instanceof Error ? e.name : 'unknown');
    return response({ error: 'service_unavailable' }, 503);
  }
}
export async function collect(env: Env) {
  // Index references are removed by CAS first. New commits only accept uploads <24h;
  // collection only considers objects >48h. Resolve must still find the version in the index.
  for (let i = 0; i < 12; i++) {
    const { value, etag } = await readIndex(env); retained(value, Date.now());
    if (await writeIndex(env, value, etag)) break;
    if (i === 11) return;
  }
  let cursor: string | undefined;
  do {
    const page = await env.SAVES.list({ prefix: 'v1/snapshots/', cursor });
    const { value } = await readIndex(env);
    const live = new Set([value.head, ...value.history, ...value.conflicts].filter(Boolean).map(r => r!.snapshot));
    for (const o of page.objects)
      if (o.uploaded.getTime() < Date.now() - 2 * DAY && !live.has(o.key.split('/').at(-1)!))
        await env.SAVES.delete(o.key);
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);
}
export default { fetch: handle, async scheduled(_event: ScheduledController, env: Env) { await collect(env); } };

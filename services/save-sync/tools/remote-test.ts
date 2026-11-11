/* Run only against an explicitly pinned, empty isolated Worker. */
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { archive, id } from '../test/helpers.ts';
const config = JSON.parse(await readFile(process.argv[2], 'utf8'));
const expectedOrigin = process.env.DM_TEST_WORKER_ORIGIN;
if (!expectedOrigin || config.test !== true || new URL(expectedOrigin).protocol !== 'https:' ||
    new URL(expectedOrigin).origin !== expectedOrigin || config.url !== expectedOrigin)
  throw Error('Set DM_TEST_WORKER_ORIGIN to the isolated test HTTPS origin and use a test connection');
async function request(path: string, method = 'GET', body?: any, token = config.token) {
  const r = await fetch(config.url + path, { method, signal: AbortSignal.timeout(30000), headers: { Authorization: 'Bearer ' + token },
    body: body instanceof Uint8Array ? new Uint8Array(body).buffer : body === undefined ? undefined : JSON.stringify(body) });
  return { status: r.status, json: await r.json() as any };
}
assert.equal((await request('/v1/status', 'GET', undefined, 'invalid')).status, 401);
const before = (await request('/v1/status')).json;
if(before.head || before.conflicts?.length)throw Error('Synthetic protocol tests require an empty isolated service with no game clients connected. Do not overwrite existing test-game progress.');
const uploads = await Promise.all([5, 6].map(async level => {
  const snapshot = id(); assert.equal((await request('/v1/snapshots/' + snapshot, 'PUT', await archive(config.device, level))).status, 201);
  return { snapshot, operation: id(), base: before.head?.id ?? null };
}));
const results = await Promise.all(uploads.map(b => request('/v1/commit', 'POST', b)));
assert.deepEqual(results.map(r => r.status).sort(), [201, 409]);
const winner = results.findIndex(r => r.status === 201), loser = results[1 - winner];
assert.equal((await request('/v1/commit', 'POST', uploads[winner])).status, 200);
assert.equal((await request('/v1/commit', 'POST', uploads[1 - winner])).json.conflict, loser.json.conflict);
const current = (await request('/v1/status')).json;
assert.ok(current.conflicts.some((r: any) => r.id === loser.json.conflict));
assert.equal((await request('/v1/resolve', 'POST', { operation: id(), snapshot: uploads[1 - winner].snapshot, base: before.head?.id ?? null })).status, 409);
assert.equal((await request('/v1/resolve', 'POST', { operation: id(), snapshot: uploads[1 - winner].snapshot,
  base: current.head.id, conflicts: [loser.json.conflict] })).status, 201);
const h = (await request('/v1/history')).json;
assert.ok(h.items.some((r: any) => r.snapshot === uploads[winner].snapshot));
assert.ok(h.items.some((r: any) => r.snapshot === uploads[1 - winner].snapshot));
console.log('PASS real R2: authentication, immutable uploads, concurrent CAS, replay, conflict preservation, conditional resolution and history.');

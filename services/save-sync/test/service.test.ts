import { test } from 'node:test';
import assert from 'node:assert/strict';
import { collect } from '../src/index.ts';
import { environment, archive, call, id, character } from './helpers.ts';
test('authentication, immutable uploads, publication, replay and revoked device', async () => {
  const { env, token, device } = await environment(), snapshot = id(), operation = id(), zip = await archive(device);
  assert.equal((await call(env, 'invalid', 'GET', '/v1/status')).status, 401);
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + snapshot, zip)).status, 201);
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + snapshot, zip)).status, 200);
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + snapshot, await archive(device, 6))).status, 409);
  const commit = { snapshot, operation, base: null };
  assert.equal((await call(env, token, 'POST', '/v1/commit', commit)).status, 201);
  assert.equal((await call(env, token, 'POST', '/v1/commit', commit)).status, 200);
  assert.equal((await (await call(env, token, 'GET', '/v1/history')).json() as any).items.length, 1);
  env.DEVICES = '{}'; assert.equal((await call(env, token, 'GET', '/v1/status')).status, 401);
});
test('simultaneous stale-base updates preserve the loser; explicit resolution is conditional', async () => {
  const { env, token, device } = await environment();
  const snapshots = [id(), id()]; for (let i = 0; i < 2; i++) await call(env, token, 'PUT', '/v1/snapshots/' + snapshots[i], await archive(device, 5 + i));
  const replies = await Promise.all(snapshots.map(snapshot => call(env, token, 'POST', '/v1/commit', { snapshot, operation: id(), base: null })));
  assert.deepEqual(replies.map(r => r.status).sort(), [201, 409]);
  const state = await (await call(env, token, 'GET', '/v1/status')).json() as any;
  assert.equal(state.conflicts.length, 1);
  assert.equal((await call(env, token, 'POST', '/v1/resolve', { operation: id(), snapshot: state.conflicts[0].snapshot, base: null })).status, 409);
  assert.equal((await call(env, token, 'POST', '/v1/resolve', { operation: id(), snapshot: state.conflicts[0].snapshot, base: state.head.id, conflicts: [state.conflicts[0].id] })).status, 201);
  const history = await (await call(env, token, 'GET', '/v1/history')).json() as any;
  assert.equal(history.conflicts.length, 0); assert.equal(new Set(history.items.map((r: any) => r.snapshot)).size, 2);
});
test('empty-save deletion requires explicit confirmation', async () => {
  const { env, token, device } = await environment(), first = id(), empty = id();
  await call(env, token, 'PUT', '/v1/snapshots/' + first, await archive(device));
  const rev = await (await call(env, token, 'POST', '/v1/commit', { snapshot: first, operation: id(), base: null })).json() as any;
  await call(env, token, 'PUT', '/v1/snapshots/' + empty, await archive(device, 0));
  const commit = { snapshot: empty, operation: id(), base: rev.revision.id };
  assert.equal((await (await call(env, token, 'POST', '/v1/commit', commit)).json() as any).error, 'confirm_empty_required');
  assert.equal((await call(env, token, 'POST', '/v1/commit', { ...commit, confirmEmpty: true })).status, 201);
});
test('reject traversal, settings, damaged archives and unknown save versions; include stash sidecar', async () => {
  const { env, token, device } = await environment();
  for (const path of ['../stolen', 'Settings/Settings.json', 'hero/../../x', 'hero/secret:stream']) {
    assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + id(), await archive(device, 5, { [path]: new Uint8Array([1]) }))).status, 422);
  }
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + id(), new Uint8Array([1, 2, 3]))).status, 422);
  const bad = character(); bad[4] = 106;
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + id(), await archive(device, 5, { 'hero/hero.d2s': bad }))).status, 422);
  const vault = new Uint8Array([83, 87, 86, 84, 1, 0, 0, 0]);
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + id(), await archive(device, 5, { 'ModernSharedStashSoftCoreV2.swvault': vault }))).status, 201);
  vault[4] = 2;
  assert.equal((await call(env, token, 'PUT', '/v1/snapshots/' + id(), await archive(device, 5, { 'ModernSharedStashSoftCoreV2.swvault': vault }))).status, 422);
});
test('collection retains current, 50 recent revisions and unresolved conflicts', async () => {
  const { env, token, device, bucket } = await environment();
  const snapshot = id(); await call(env, token, 'PUT', '/v1/snapshots/' + snapshot, await archive(device));
  await call(env, token, 'POST', '/v1/commit', { snapshot, operation: id(), base: null });
  const index = JSON.parse(new TextDecoder().decode(bucket.objects.get('v1/index.json')!.bytes));
  const old = Date.now() - 40 * 86400000;
  index.history = Array.from({ length: 60 }, (_, i) => ({ ...index.head, id: id(), snapshot: id(), createdAt: old - i }));
  index.head = index.history[0]; index.conflicts = [{ ...index.head, id: id(), snapshot, createdAt: old }];
  await bucket.put('v1/index.json', JSON.stringify(index));
  const orphan = id(); await bucket.put('v1/snapshots/' + orphan, new Uint8Array([0])); bucket.objects.get('v1/snapshots/' + orphan)!.uploaded = new Date(old);
  await collect(env);
  assert.equal((await (await call(env, token, 'GET', '/v1/history')).json() as any).items.length, 50);
  assert.ok(bucket.objects.has('v1/snapshots/' + snapshot)); assert.ok(!bucket.objects.has('v1/snapshots/' + orphan));
});
test('a delayed operation replay remains idempotent after its ZIP was collected',async()=>{
  const {env,token,device,bucket}=await environment();const body={operation:id(),snapshot:id(),base:null};
  await call(env,token,'PUT','/v1/snapshots/'+body.snapshot,await archive(device));
  const first=await call(env,token,'POST','/v1/commit',body);assert.equal(first.status,201);
  await bucket.delete('v1/snapshots/'+body.snapshot);
  const replay=await call(env,token,'POST','/v1/commit',body);assert.equal(replay.status,200);
  assert.equal((await replay.json() as any).revision.id,body.operation);
});

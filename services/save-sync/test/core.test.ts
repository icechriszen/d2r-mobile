import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, writeFile, readFile, readdir, rm, symlink, realpath, utimes, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { environment, character, id, archive, call } from './helpers.ts';
import { handle, sha } from '../src/index.ts';

const driver = process.env.DM_SYNC_DRIVER;
async function execute(args: string[], env: Record<string, string> = {}, expect = 0) {
  return new Promise<string>((resolve, reject) => {
    const child = spawn(driver!, args, { env: { ...process.env, ...env } });
    let out = '', err = ''; child.stdout.on('data', b => out += b); child.stderr.on('data', b => err += b);
    child.on('error', reject); child.on('exit', code => code === expect ? resolve(out) : reject(new Error(`driver ${args[0]} exit=${code}: ${err}\n${out}`)));
  });
}
async function fixture() {
  const { env, bucket, device, token } = await environment();
  let dropCommit = false, offline = false, damageDownload = false, statusDelay = 0, historyFailure = 0, historyDelay = 0;
  const server = createServer(async (req, res) => {
    if (offline) { res.writeHead(503).end(); return; }
    if(historyFailure&&req.url?.startsWith('/v1/history')){res.writeHead(historyFailure).end();return;}
    if(historyDelay&&req.url?.startsWith('/v1/history'))await new Promise(r=>setTimeout(r,historyDelay));
    if(statusDelay&&req.url==='/v1/status')await new Promise(r=>setTimeout(r,statusDelay));
    if(req.destroyed||res.destroyed)return;
    const chunks: Buffer[] = []; for await (const b of req) chunks.push(b);
    const response = await handle(new Request('https://sync.test' + req.url, { method: req.method,
      headers: { Authorization: req.headers.authorization ?? '' },
      body: chunks.length ? Buffer.concat(chunks) : undefined }), env);
    if (dropCommit && req.url === '/v1/commit') { dropCommit = false; req.socket.destroy(); return; }
    const bytes = Buffer.from(await response.arrayBuffer());
    if (damageDownload && req.method === 'GET' && req.url?.startsWith('/v1/snapshots/')) bytes[40] ^= 1;
    res.writeHead(response.status, Object.fromEntries(response.headers)); res.end(bytes);
  });
  await new Promise<void>(resolve => server.listen(0, '127.0.0.1', resolve));
  const root = await realpath(await mkdtemp(join(tmpdir(), 'd2r-sync-test-')));
  const endpoint = `http://localhost:${(server.address() as any).port}`;
  async function client(name: string) {
    const dir = join(root, name), save = join(dir, 'save'), connection = join(dir, 'connection.json');
    await mkdir(join(save, 'Settings'), { recursive: true });
    await writeFile(join(save, 'Settings', 'Settings.json'), name);
    await writeFile(connection, JSON.stringify({ url: endpoint, device, token, test: true }), { mode: 0o600 });
    return { save, connection, dir,
      async boot(action?: string, argument?: string) { return JSON.parse(await execute(['boot', save, ...(action ? [action, argument ?? ''] : [])], { DM_SYNC_CONNECTION: connection })); },
      async progress(level: number) { await mkdir(join(save, 'hero'), { recursive: true }); await writeFile(join(save, 'hero', 'hero.d2s'), character(level)); },
      async level() { return (await readFile(join(save, 'hero', 'hero.d2s')))[0x1b]; },
      async stateDir() { return join(save + '.r2', (await sha(endpoint)).slice(0, 16)); }
    };
  }
  return { env, bucket, device, token, root, client,
    offline(v: boolean) { offline = v; }, drop() { dropCommit = true; }, damage(v: boolean) { damageDownload = v; },
    slowStatus(ms:number){statusDelay=ms;}, historyFailure(code:number){historyFailure=code;}, slowHistory(ms:number){historyDelay=ms;},
    async close() { await new Promise<void>(r => server.close(() => r())); await rm(root, { recursive: true, force: true }); }
  };
}
test('native core: three isolated save directories complete a loop and retain device settings', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const clients = await Promise.all(['iphone', 'android', 'mac'].map(n => f.client(n)));
    await clients[0].progress(5); assert.equal((await clients[0].boot()).status, 'synced');
    for (let i = 1; i <= 3; i++) {
      const c = clients[i % 3]; assert.equal((await c.boot()).status, 'synced'); assert.equal(await c.level(), 4 + i);
      await c.progress(5 + i); assert.equal((await c.boot()).status, 'synced');
    }
    for (const c of clients) assert.equal(await readFile(join(c.save, 'Settings/Settings.json'), 'utf8'), c.dir.split('/').at(-1));
  } finally { await f.close(); }
});
test('native core: conflict remains visible, cloud choice is deferred until restart, and both versions survive', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const a = await f.client('a'), b = await f.client('b'); await a.progress(5); await a.boot(); await b.boot();
    await a.progress(6); await b.progress(7); await a.boot(); assert.equal((await b.boot()).status, 'conflict');
    assert.equal(await b.level(), 7); assert.equal((await b.boot('cloud')).status, 'restart_required');
    assert.equal(await b.level(), 7); assert.equal((await b.boot()).status, 'synced'); assert.equal(await b.level(), 6);
    const h = await (await call(f.env, f.token, 'GET', '/v1/history')).json() as any;
    assert.ok(h.items.some((r: any) => r.characters[0].level === 7));
    assert.ok((await readdir(join(await b.stateDir(), 'backups'))).length > 0);
  } finally { await f.close(); }
});
test('native core: in-game commits refresh character metadata without publishing or hiding a conflict', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a'),b=await f.client('b');await a.progress(5);await a.boot();await b.boot();
    const next=join(f.root,'next.d2s');await writeFile(next,character(6));
    let result=await a.boot('commit',next);assert.equal(result.status,'local_saved');assert.equal(result.local.characters[0].level,6);
    let h=await (await call(f.env,f.token,'GET','/v1/history')).json() as any;assert.equal(h.items.length,1);
    await a.boot();await b.progress(7);assert.equal((await b.boot()).status,'conflict');
    await writeFile(next,character(8));result=await b.boot('commit',next);
    assert.equal(result.status,'conflict');assert.equal(result.local.characters[0].level,8);
    h=await (await call(f.env,f.token,'GET','/v1/history')).json() as any;assert.equal(h.items[0].characters[0].level,6);
  }finally{await f.close();}
});
test('native core: choosing cloud after continued conflict play preserves the latest local progress too', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const a = await f.client('a'), b = await f.client('b'); await a.progress(5); await a.boot(); await b.boot();
    await a.progress(6); await a.boot(); await b.progress(7); await b.boot();
    await b.progress(8); assert.equal((await b.boot('cloud')).status, 'restart_required');
    const h = await (await call(f.env, f.token, 'GET', '/v1/history')).json() as any;
    assert.ok(h.items.some((r: any) => r.characters[0].level === 7)); assert.ok(h.items.some((r: any) => r.characters[0].level === 8));
    await b.boot(); assert.equal(await b.level(), 6);
  } finally { await f.close(); }
});
test('native core: lost commit response retries once; offline changes are never replaced with old cloud data', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const a = await f.client('a'); await a.progress(5); f.drop(); assert.equal((await a.boot()).status, 'pending_upload');
    await a.progress(6); assert.equal((await a.boot()).status, 'synced'); assert.equal(await a.level(), 6);
    let h = await (await call(f.env, f.token, 'GET', '/v1/history')).json() as any; assert.equal(h.items.length, 2);
    f.offline(true); await a.progress(7); assert.equal((await a.boot()).status, 'offline');
    f.offline(false); await a.boot(); assert.equal(await a.level(), 7);
    h = await (await call(f.env, f.token, 'GET', '/v1/history')).json() as any; assert.equal(h.items.length, 3);
  } finally { await f.close(); }
});
test('native core: startup releases offline after five seconds without changing local saves', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a');await a.progress(5);f.slowStatus(6500);
    const start=Date.now(),s=await a.boot();assert.equal(s.status,'offline');assert.ok(Date.now()-start<5800);
    assert.equal(await a.level(),5);assert.equal(f.bucket.objects.size,0);
  }finally{await f.close();}
});
test('native core: UTF-8 character sidecar ordering is identical to the service', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a');await a.progress(5);
    await writeFile(join(a.save,'hero/\ue000.map'),new Uint8Array([1]));await writeFile(join(a.save,'hero/😀.map'),new Uint8Array([2]));
    assert.equal((await a.boot()).status,'synced');
  }finally{await f.close();}
});
test('native core: download retains the saved time, without relabelling it as download time', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a'),b=await f.client('b');await a.progress(5);
    const saved=1700000000;await utimes(join(a.save,'hero/hero.d2s'),saved,saved);
    assert.equal((await a.boot()).local.savedAt,saved*1000);await b.boot();
    assert.equal((await stat(join(b.save,'hero/hero.d2s'))).mtimeMs,saved*1000);
    assert.equal((await b.boot()).local.savedAt,saved*1000);
  }finally{await f.close();}
});
test('native core: damaged download, invalid save and symlink cannot replace current progress', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const a = await f.client('a'), b = await f.client('b'); await a.progress(5); await a.boot(); await b.boot();
    await a.progress(6); await a.boot(); f.damage(true); assert.equal((await b.boot()).status, 'invalid_download'); assert.equal(await b.level(), 5);
    f.damage(false); await symlink('/etc/passwd', join(b.save, 'hero/secret')); assert.equal((await b.boot()).status, 'invalid_local');
    assert.equal(await b.level(), 5);
    const bad = character(7); bad[4] = 106; await writeFile(join(a.save, 'hero/hero.d2s'), bad);
    assert.equal((await a.boot()).status, 'invalid_local');
  } finally { await f.close(); }
});
test('native install: unsafe paths and a failed backup leave every original byte intact', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const a = await f.client('a'); await a.progress(5);
    const zip = join(f.root, 'input.zip'); await writeFile(zip, await archive(f.device, 6));
    const blocked = join(f.root, 'backup-file'); await writeFile(blocked, 'not a directory');
    await execute(['apply', zip, a.save, blocked], {}, 3); assert.equal(await a.level(), 5);
    await writeFile(zip, await archive(f.device, 6, { '../escape': new Uint8Array([1]) }));
    await execute(['apply', zip, a.save, join(f.root, 'backup')], {}, 3); assert.equal(await a.level(), 5);
    await writeFile(zip,await archive(f.device,6,{'hero/é.map':new Uint8Array([1]),'hero/e\u0301.map':new Uint8Array([2])}));
    await execute(['apply',zip,a.save,join(f.root,'backup')],{},3);assert.equal(await a.level(),5);
  } finally { await f.close(); }
});
test('native core: process termination after atomic swap recovers the receipt on next boot', { skip: !driver }, async () => {
  const f = await fixture(); try {
    const a=await f.client('a'),b=await f.client('b');await a.progress(5);await a.boot();await b.boot();
    await a.progress(6);await a.boot();
    await execute(['boot',b.save],{DM_SYNC_CONNECTION:b.connection,DM_SYNC_CRASH_AFTER_SWAP:'1'},86);
    assert.equal(await b.level(),6); assert.ok(await readFile(join(await b.stateDir(),'install.json')));
    assert.equal((await b.boot()).status,'synced');
    await assert.rejects(readFile(join(await b.stateDir(),'install.json')));
    const history=await (await call(f.env,f.token,'GET','/v1/history')).json() as any;assert.equal(history.items.length,2);
  }finally{await f.close();}
});
test('native core: termination before swap keeps old saves and cleans abandoned staging on retry', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a'),b=await f.client('b');await a.progress(5);await a.boot();await b.boot();await a.progress(6);await a.boot();
    await execute(['boot',b.save],{DM_SYNC_CONNECTION:b.connection,DM_SYNC_CRASH_BEFORE_SWAP:'1'},85);
    assert.equal(await b.level(),5);assert.ok((await readdir(b.dir)).some(n=>n.startsWith('save.r2-new-')));
    assert.equal((await b.boot()).status,'synced');assert.equal(await b.level(),6);
    assert.ok(!(await readdir(b.dir)).some(n=>n.startsWith('save.r2-new-')));
  }finally{await f.close();}
});
test('native core: complete save sets, deletion confirmation and history restore preserve settings', { skip: !driver }, async () => {
  const f=await fixture();try{
    const a=await f.client('a'),b=await f.client('b');await a.progress(5);
    const extras:Record<string,Uint8Array>={
      'ModernSharedStashSoftCoreV2.swvault':new Uint8Array([83,87,86,84,1,0,0,0]),
      'hero/hero.swpouch':new Uint8Array([83,87,80,85,1,0,0,0]),
      'hero/hero.swpage':new Uint8Array([83,87,80,71,2,0,0,0]),
      'hero/hero.sw_rift':new TextEncoder().encode('tier 8\nunlock 12\nbest 11\n'),
      'hero/hero.sw_kills':new TextEncoder().encode('# dm_kills 2\n'),
      'hero/hero.dm_pets':new TextEncoder().encode('1 100 200\n')
    };
    for(const [name,data] of Object.entries(extras))await writeFile(join(a.save,name),data);
    await a.boot();await b.boot();
    for(const [name,data] of Object.entries(extras))assert.deepEqual(new Uint8Array(await readFile(join(b.save,name))),data);
    const first=(await (await call(f.env,f.token,'GET','/v1/status')).json() as any).head;
    await rm(join(a.save,'hero'),{recursive:true});assert.equal((await a.boot()).status,'confirm_empty');
    assert.equal((await a.boot('confirm-empty')).status,'synced');await b.boot();
    await assert.rejects(readFile(join(b.save,'hero/hero.d2s')));
    assert.equal((await b.boot('restore',first.id)).status,'restart_required');await b.boot();assert.equal(await b.level(),5);
    assert.equal(await readFile(join(b.save,'Settings/Settings.json'),'utf8'),'b');
  }finally{await f.close();}
});
test('native core: a new character supersedes an unconfirmed last-character deletion', { skip: !driver }, async()=>{
  const f=await fixture();try{
    const a=await f.client('a');await a.progress(5);await a.boot();
    await rm(join(a.save,'hero'),{recursive:true});assert.equal((await a.boot()).status,'confirm_empty');
    await a.progress(8);assert.equal((await a.boot()).status,'synced');
    const h=await (await call(f.env,f.token,'GET','/v1/history')).json() as any;
    assert.equal(h.items[0].characters[0].level,8);assert.ok(h.items.every((r:any)=>r.characters.length===1));
  }finally{await f.close();}
});
test('native core: replacement backups retain exactly the five most recent local versions', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a'),b=await f.client('b');await a.progress(1);await a.boot();await b.boot();
    for(let level=2;level<=8;level++){await a.progress(level);await a.boot();await b.boot();}
    const path=join(await b.stateDir(),'backups'),backups=(await readdir(path)).sort();assert.equal(backups.length,5);
    assert.deepEqual(await Promise.all(backups.map(async d=>(await readFile(join(path,d,'hero/hero.d2s')))[0x1b])),[3,4,5,6,7]);
  }finally{await f.close();}
});

test('native core: unavailable history is retryable and does not claim a version is missing', {skip:!driver}, async()=>{
  const f=await fixture();try{
    const a=await f.client('history-network');await a.progress(5);await a.boot();
    const first=(await (await call(f.env,f.token,'GET','/v1/status')).json() as any).head;
    await a.progress(6);await a.boot();f.historyFailure(503);
    assert.equal((await a.boot('restore',first.id)).status,'offline');assert.equal(await a.level(),6);
    f.historyFailure(401);assert.equal((await a.boot('restore',first.id)).status,'unauthorized');
    f.historyFailure(0);f.slowHistory(5500);assert.equal((await a.boot('restore',first.id)).status,'restart_required');
    await a.boot();assert.equal(await a.level(),5);
  }finally{await f.close();}
});

test('native onboarding: explicit import validates isolation, paths and secure-store readback before removing plaintext', {skip:!driver},async()=>{
  const root=await realpath(await mkdtemp(join(tmpdir(),'d2r-connect-test-')));
  try{
    const save=join(root,'save'),file=join(root,'cloud-connection.json'),store=join(root,'vault.json');await mkdir(save);
    const cfg={url:'https://sync.example.test',device:id(),token:'a'.repeat(43),test:true};
    const env={DM_TEST_CREDENTIAL_STORE:store};
    await writeFile(file,JSON.stringify(cfg));
    assert.equal(JSON.parse(await execute(['boot',save],env)).status,'connection_file_ready');
    await assert.rejects(stat(store)); // launching never silently creates access
    const preview=JSON.parse(await execute(['import',save,'inspect'],env));
    assert.deepEqual(preview,{url:cfg.url,device:cfg.device,sourceHash:await sha(JSON.stringify(cfg))});
    assert.equal(await readFile(file,'utf8'),JSON.stringify(cfg)); // inspection never connects
    assert.equal((await execute(['import',save,'accept'],{...env,DM_TEST_STORE_FAIL:'1'})).trim(),'-1');
    assert.ok(await stat(file));
    assert.equal((await execute(['import',save,'accept'],env)).trim(),'0');
    assert.deepEqual(JSON.parse(await readFile(store,'utf8')),cfg);await assert.rejects(stat(file));
    for(const bytes of [JSON.stringify({...cfg,test:false}),JSON.stringify({...cfg,url:'http://evil.test'}),JSON.stringify(cfg)+'junk',JSON.stringify(cfg)+'\0', ' '.repeat(4097)]){
      await writeFile(file,bytes);assert.equal((await execute(['import',save,'inspect'],env)).trim(),'null');
      assert.equal((await execute(['import',save,'accept'],env)).trim(),'-1');assert.ok(await stat(file));
    }
    await rm(file);await symlink(store,file);assert.equal((await execute(['import',save,'inspect'],env)).trim(),'null');
  }finally{await rm(root,{recursive:true,force:true});}
});


test('game UI: a stale confirmation never selects a newer cloud version', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a'),b=await f.client('b');await a.progress(5);const first=await a.boot();await b.boot();
    await a.progress(6);await a.boot();await b.progress(7);await b.boot();
    const result=await b.boot('checked-cloud',JSON.stringify({head:first.remote.id,hash:first.local.contentHash}));
    assert.equal(result.status,'confirmation_changed');assert.equal(await b.level(),7);
    const h=await (await call(f.env,f.token,'GET','/v1/history')).json() as any;
    assert.equal(h.items[0].characters[0].level,6);assert.ok(h.conflicts.some((r:any)=>r.characters[0].level===7));
  }finally{await f.close();}
});
test('game UI: nested back cancels, defaults to cancel, and preserves settings', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a');await a.progress(5);await a.boot();
    const steps=await a.boot('ui','open,disconnect,activate,disconnect,back,history,wait,activate,restore,back,back,back,back');
    assert.equal(steps[1].page,5);assert.equal(steps[1].selection,0);assert.equal(steps[1].buttons[0].action,'back');
    assert.equal(steps[2].page,1);assert.equal(steps[7].page,4);assert.equal(steps[8].page,5);
    assert.equal(steps[9].page,4);assert.equal(steps[10].page,3);assert.equal(steps[11].page,1);assert.equal(steps[12].page,0);
    assert.equal(await readFile(join(a.save,'Settings/Settings.json'),'utf8'),'a');
  }finally{await f.close();}
});
test('game UI: late history does not reopen a closed page, and failures are retryable', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a');await a.progress(5);await a.boot();f.slowHistory(250);
    let steps=await a.boot('ui','open,history,back,back,wait');assert.equal(steps.at(-1).page,0);
    f.historyFailure(503);steps=await a.boot('ui','open,history,wait');assert.equal(steps.at(-1).page,6);assert.match(steps.at(-1).left,/dmSyncHistoryRetry/);
    f.historyFailure(0);steps=await a.boot('ui','open,history,wait');assert.equal(steps.at(-1).page,3);assert.ok(steps.at(-1).buttons.some((b:any)=>/^record-[a-f0-9]{32}$/.test(b.action)));
  }finally{await f.close();}
});
test('game UI: connection preview never renders its token or silently grants access', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('a');await writeFile(join(a.dir,'cloud-connection.json'),await readFile(a.connection));
    const store=join(a.dir,'vault.json');
    const views=JSON.parse(await execute(['boot',a.save,'ui','open,connect-page,paste,back'],{DM_TEST_CREDENTIAL_STORE:store,DM_TEST_PASTE_FILE:a.connection}));
    assert.equal(views[2].page,5);assert.equal(views[3].page,2);assert.ok(!JSON.stringify(views).includes(f.token));
    await assert.rejects(readFile(store));
  }finally{await f.close();}
});

test('native UI: disconnect failure stays connected and retry removes the vault entry', {skip:!driver}, async()=>{
  const f=await fixture();try{
    const a=await f.client('disconnect');await a.progress(5);await a.boot();
    const args=['boot',a.save,'ui','open,disconnect,confirm'];
    const env={DM_SYNC_CONNECTION:a.connection,DM_TEST_CREDENTIAL_STORE:a.connection};
    let views=JSON.parse(await execute(args,{...env,DM_TEST_STORE_FAIL:'1'}));
    assert.equal(views.at(-1).page,6);assert.match(views.at(-1).left,/dmSyncConnectionError/);
    assert.ok((await readFile(a.connection)).length>0);
    views=JSON.parse(await execute(args,env));assert.equal(views.at(-1).page,1);
    assert.equal(views.at(-1).status,'dmSyncDisconnected');
    await assert.rejects(readFile(a.connection),{code:'ENOENT'});
  }finally{await f.close();}
});

test('game UI: identical history captions restore the explicitly bound revision ID', {skip:!driver},async()=>{
  const f=await fixture();try{
    const a=await f.client('identical');await a.progress(5);
    await utimes(join(a.save,'hero/hero.d2s'),1700000000,1700000000);
    const path=join(a.save,'hero/hero.ctl'),bytes=Buffer.from([1]);
    bytes[0]=1;await writeFile(path,bytes);await utimes(path,1700000000,1700000000);const first=await a.boot();
    bytes[0]=2;await writeFile(path,bytes);await utimes(path,1700000000,1700000000);const second=await a.boot();
    assert.notEqual(first.remote.id,second.remote.id);
    const views=await a.boot('ui',`open,history,wait,record-${first.remote.id},restore,confirm,wait`);
    const rows=views[2].buttons.filter((b:any)=>b.action.startsWith('record-'));
    assert.equal(rows.length,2);assert.equal(rows[0].label,rows[1].label);
    assert.notEqual(rows[0].action,rows[1].action);assert.equal(views[3].page,4);
    await a.boot();assert.equal((await readFile(path))[0],1);
  }finally{await f.close();}
});

# Personal R2 cloud saves

中文：[README.zh-CN.md](README.zh-CN.md)

Protocol 1, shared with `dm/sync`, supports game 1.0.34.0 / saves 3.3.94012. Complete gameplay snapshots include characters, stashes and expansion progress; graphics and touch settings stay on each device. The owner administers deployment and device connections.

For the complete step-by-step deployment, per-device setup, revocation and maintenance walkthrough, see [the Chinese setup guide](README.zh-CN.md). No maintainer service credentials are included.

## Deployment and device access

Use Node.js 22 or a newer version supported by the locked Wrangler package. Copy `wrangler.jsonc` to ignored `wrangler.local.jsonc`, set your own Worker and private bucket names, and consistently pass `--config wrangler.local.jsonc` to deploy/device commands. The names below are examples; replace the hostname and private directory with your own:

```sh
npm ci
npx wrangler login
npx wrangler r2 bucket create d2r-private-saves
npm run deploy -- --config wrangler.local.jsonc
npm run devices -- add iPhone https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev --registry /PRIVATE/devices.json --config wrangler.local.jsonc
npm run devices -- add Android https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev --registry /PRIVATE/devices.json --config wrangler.local.jsonc
npm run devices -- add Mac https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev --registry /PRIVATE/devices.json --config wrangler.local.jsonc
npm run devices -- publish --registry /PRIVATE/devices.json --config wrangler.local.jsonc
```

Bind `SAVES` to the dedicated private bucket in `wrangler.jsonc`. Keep public access disabled. Use the deployed HTTPS origin. Tests require a separate bucket, `wrangler.test.jsonc`, tokens and saves; registration also takes `--test --config wrangler.test.jsonc`.

The tool prints a connection-file path and device ID. Paste the complete JSON file into Cloud Saves → Connect Device. It contains a bearer token granting access to this personal service. Keep the registry and connection files outside Git, public chats and app bundles. The Worker's `DEVICES` secret contains token digests; apps contain no Cloudflare management credentials.

To revoke a device, run `npm run devices -- list --registry /PRIVATE/devices.json`, then `revoke DEVICE_ID` with the same registry, then `publish`. Add/revoke only alter the local registry until publication succeeds. Give replacement devices new tokens and revoke the old ones after connecting. Never delete the current-version pointer to resolve conflicts.

## Player controls

- iPhone/Android/Mac: the first row of in-game Options → Custom → Cloud Saves. There is no separate Mac system-menu entry.
- Home, connection preview, conflicts, history and confirmations use game controls. Touch supports taps/swipes; mouse supports clicks/wheel; keyboard supports Tab/arrows, Enter and Esc; controllers retain the game confirm/back mapping. Back closes one level without changing underlying settings. Confirmations default to Cancel.
- Paste Code reads the clipboard only on request. Import File previews a prepared cloud-connection.json beside the save directory. Confirming stores credentials securely, verifies read-back, then deletes the plaintext file. Tokens are never shown in the game UI.
- Reopen after connecting. Empty local progress downloads from cloud; an empty cloud receives local progress. Different nonempty progress requires a choice.
- Saved Locally means the game committed successfully. Synced to Cloud means publication was confirmed. Saving/exiting or backgrounding attempts upload; gameplay has no periodic upload timer. Forced termination does not guarantee background completion.
- Sync Now checks immediately. Downloads during gameplay are cached. Reopening rechecks current local/cloud progress before applying them.
- Conflicts offer Use Local, Use Cloud or Later. Continued local play does not overwrite the other version. Choosing cloud progress first preserves the latest committed local progress.
- History is scrollable and paginated, and records are bound to unique version IDs. Back from details preserves the list position. Restoration publishes a new version; a changed head requires fresh confirmation. Deleting the last character requires confirmation. Disconnect removes credentials, preserving local saves and backups. Offline play remains available.
- Runtime conflicts and completed downloads show a five-second notice without taking focus. Outstanding status remains on the entry and home page. Downloads can be cancelled and failed checks retried. Once the game UI is ready, cloud-save business dialogs stay in-game.

R2 excludes upstream iCloud synchronization/item merging while connected; upstream iCloud code is preserved. Free Apple accounts support R2 but still require app re-signing when their signature expires.

## Protocol and protection

Every endpoint requires a device bearer token. ZIP STORE snapshots are limited to 16 MiB / 2,048 files. The manifest includes protocol/game compatibility, parent, device, save time, character summaries, file sizes and SHA-256. Source commit IDs are diagnostic only.

| Endpoint | Behavior |
| --- | --- |
| `GET /v1/status` | Current version, conflicts and compatibility |
| `PUT /v1/snapshots/{32-digit-hex-id}` | Validate/store an immutable snapshot; identical retries are safe |
| `GET /v1/snapshots/{id}` | Download a snapshot |
| `POST /v1/commit` | `{operation,snapshot,base,confirmEmpty}`; conditional publication or preserved conflict with HTTP 409 |
| `GET /v1/history?offset=0` | Up to 50 records, conflicts and next offset |
| `POST /v1/resolve` | `{operation,snapshot,base,conflicts}`; explicit restoration/resolution rechecks the head |

ETag-conditional writes to `v1/index.json` maintain current/history/conflict pointers and operation receipts atomically. Snapshots are verified before publication. Clients persist operation IDs, requests and ZIPs; lost responses can be retried without duplicate publication. Device time never chooses the winner.

Clients validate paths, size, CRC32, SHA-256, compatibility and known save formats. Links, traversal, case collisions and Unicode-normalization collisions are rejected. A complete backup and staging directory precede atomic directory exchange; a journal recovers interruption before the state update. Failure preserves original progress. The latest five local backups remain, and device settings are retained.

Daily cleanup deletes published progress only when older than 30 days and outside the latest 50 versions. Unresolved conflicts remain. Orphans remain at least 48 hours; publication requires upload within 24 hours to avoid cleanup races. Expired operations upload a fresh snapshot. Published receipts remain for late retries.

## Verification and maintenance

```sh
npm run check
DM_SYNC_TEST_OUT=/ABSOLUTE/ISOLATED-OUTPUT bash ../../dm/sync/tests/build.sh
DM_SYNC_DRIVER=/ABSOLUTE/ISOLATED-OUTPUT/sync-driver npm test
```

The native driver uses AddressSanitizer/UndefinedBehaviorSanitizer and separate process restarts to test recovery. Service tests use an in-memory bucket; native integration uses a local HTTP service and isolated saves. Real R2 tests use separate test configuration/tokens. Synthetic protocol-test characters must not enter actual gameplay.

`DM_SYNC_TESTING` is exclusively for isolated builds; omit it from normal personal builds. Test entry points require `DM_SYNC_TEST=1`, an isolated save marker and a `test:true` connection. Apple uses a separate test Keychain entry; Android uses a separate package. Plaintext handoff files are deleted only after secure-store readback succeeds.

Test type checks, core tests and real-service behavior before deploying changes. Keep private backups of `v1/index.json`, `v1/snapshots/` and the device registry. Never bind the test bucket to production. Preserve backups/state on invalid formats or disk errors; do not erase state to force replacement.

Build/download/core-test success does not constitute three-platform acceptance. Real-device records must also cover game reads/writes, conflicts, restoration and the full iPhone → Android → Mac → iPhone loop.

Device onboarding also accepts a local `cloud-connection.json` beside the save directory. The mobile UI asks before connecting, validates at most 4 KiB without following links, verifies the secure-store readback, then removes the plaintext. Test and normal credentials remain isolated. Cloud Saves is now the first row of Custom settings and supports controller selection. A new Android device whose first network check times out gets a visible retry page.

Real-service synthetic tests require an empty, isolated Worker and an explicit origin pin: `DM_TEST_WORKER_ORIGIN=https://YOUR-TEST-WORKER.YOUR-SUBDOMAIN.workers.dev npx tsx tools/remote-test.ts /PRIVATE/TEST/connection.txt`. The connection must carry `test:true`; never run it on gameplay or production progress.

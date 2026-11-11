# 个人 R2 云存档服务

English: [README.md](README.md)

与 `dm/sync` 共用协议 1，当前支持游戏 1.0.34.0 / 存档 3.3.94012。服务只保存整套玩法存档，不保存游戏资源或画质、触控设置。部署、设备注册表和连接码均由个人管理员维护。

## 从零配置自己的跨平台服务

### 1. 准备私有桶和本地配置

准备自己的 Cloudflare 账号并启用 R2，以及 Node.js 22 或更新的受 Wrangler 支持版本。以下命令在 `services/save-sync` 目录运行。仓库仅提供通用配置示例，不提供维护者的服务、设备令牌或管理密钥。

```sh
npm ci
npx wrangler login
cp wrangler.jsonc wrangler.local.jsonc
```

编辑 **`wrangler.local.jsonc`**：将 `name` 改为你自己的 Worker 名，例如 `my-d2r-saves`；将 `r2_buckets[0].bucket_name` 改为你自己的桶名，例如 `my-d2r-private-saves`。保留绑定名 `SAVES`、`main`、`compatibility_date` 和清理计划。该本地文件已忽略，不要提交到 Git。若账号下有多个 Cloudflare 账户，按 Wrangler 提示选择你创建 R2 桶的账户。

```sh
npx wrangler r2 bucket create my-d2r-private-saves
npm run deploy -- --config wrangler.local.jsonc
```

不要启用 R2 桶的 `r2.dev` 或公共自定义域名。游戏只访问 Worker，不直连 R2。记录部署输出的 HTTPS 地址，下面的 `YOUR-WORKER.YOUR-SUBDOMAIN` 必须替换成自己的地址。无需创建 S3 Access Key，也无需把 Cloudflare API Token 编进应用。Wrangler 登录凭据只用于本机管理服务。

### 2. 为每台设备生成独立连接文件

在仓库外建立私人目录，注册表保存设备编号、名称和令牌摘要；每份连接文件另含该设备的随机令牌。

```sh
R2_PRIVATE_DIR="$HOME/.config/d2r-save-sync"
mkdir -p "$R2_PRIVATE_DIR"
chmod 700 "$R2_PRIVATE_DIR"
R2_SAVE_URL='https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev'
npm run devices -- add iPhone "$R2_SAVE_URL" --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
npm run devices -- add Android "$R2_SAVE_URL" --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
npm run devices -- add Mac "$R2_SAVE_URL" --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
npm run devices -- publish --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
```

每次 `add` 只输出新设备编号和 `connection-<编号>.txt` 路径；记下它对应哪台设备。`publish` 把完整注册表写入 Worker 的 **`DEVICES` secret**，并非公开变量。未发布或已撤销的设备得到 `401`。注册表必须持续保留：以后新增设备仍使用同一份文件，不能用新空注册表发布，否则会撤销原有设备。

连接文件含授权令牌，持有者可以访问该个人服务的全部存档。不要把它、注册表、Wrangler 登录信息、签名证书、游戏存档或资源放进 Git、安装包、公开日志或工单。连接码丢失时生成新设备令牌；服务只存摘要，不能找回原令牌。

### 3. 构建并连接三端

客户端已集成共用 R2 核心，无需写死服务地址或密钥。按 [iOS 构建说明](../../dm/ios/README.zh-CN.md)、[Android 构建说明](../../dm/android/README.zh-CN.md) 及 [Mac 构建脚本](../../dm/tools/mac_app.sh) 准备游戏资源并构建。免费 Apple 账号使用 `DM_FREE_ACCOUNT=1`。正常构建不要开启 `DM_SYNC_TESTING`、`DM_SYNC_FORCE_ISOLATED` 或 `dmSyncTesting`；这些只供隔离测试。

每台设备使用上一步**自己的**连接文件：

1. 打开游戏内 **设置 → 自定义 → 云存档 → 连接设备**。
2. 将对应文件的完整 JSON 内容复制到设备剪贴板，点击 **粘贴连接码**；iPhone 如出现系统粘贴提示，选择允许。
3. 核对预览里的服务地址和设备编号，点击确认连接。应用将凭据存入 Apple 钥匙串或 Android Keystore 保护的存储，不显示令牌。
4. 完全退出并重新打开应用，等待同步。先连接持有主进度的一台设备，确认显示“云端已同步”，再逐台连接其余设备。两边已有不同进度时必须由玩家选择，不按设备时间覆盖。
5. 在第二台确认角色、背包和共享仓库，游玩并“保存并退出”；等待“云端已同步”后，再到下一台重开验证。每台的画质和触控设置仍独立保存。

若不便粘贴，可把文件命名为 `cloud-connection.json`，放在 `save` 目录旁，再在同一页面选择 **导入连接文件**。iPhone 是应用 Documents 目录；Mac 默认是 `~/Library/Application Support/D2R Mobile/`；Android 是该应用的私有 `files` 目录（仅自建可调试包可通过 `adb run-as` 写入）。不要把连接文件放入 `save` 目录或公共资源包。验证安全存储读回成功后，应用删除已导入的明文文件；管理员私人备份由你自行保管。

### 4. 撤销、换机和维护

```sh
npm run devices -- list --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
npm run devices -- revoke DEVICE_ID --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
npm run devices -- publish --registry "$R2_PRIVATE_DIR/devices.json" --config wrangler.local.jsonc
```

`add` / `revoke` 先改本地注册表，`publish` 成功才影响服务。换机时先添加新设备，确认连接后再撤销旧设备；设备丢失时立即撤销。若真实令牌曾进入公开提交，删除文件并不能消除泄露，应先撤销令牌，再清理历史。应用卸载前备份进度；重签覆盖安装沿用同一标识以保留资源、设置和凭据。

每次更新 Worker 使用同一份 `wrangler.local.jsonc` 和私有桶，更新前备份桶中的 `v1/index.json`、`v1/snapshots/` 及本地设备注册表。不要删除版本指针来解决冲突。错误凭据用重新连接处理；网络超时可在云存档主页“立即同步”重试；游戏中下载完成后须重开应用才生效。

### 5. 独立测试环境

另外创建测试 Worker 和测试桶，复制 `wrangler.test.jsonc` 为 `wrangler.test.local.jsonc` 并修改名称、绑定。使用独立注册表，设备生成时额外传 `--test --config wrangler.test.local.jsonc`；测试和正式凭据不能混用。

`tools/remote-test.ts` 会发布合成测试存档，只能用于**尚无进度、没有游戏客户端连接**的专用测试服务。必须显式指定预期地址：

```sh
DM_TEST_WORKER_ORIGIN='https://YOUR-TEST-WORKER.YOUR-SUBDOMAIN.workers.dev' \
  npx tsx tools/remote-test.ts /PRIVATE/TEST/connection-DEVICE_ID.txt
```

脚本验证 `test:true`、HTTPS 地址完全一致和空服务后才运行并发提交测试。不要把合成测试快照用于真实游戏。真实游玩闭环应使用另一套隔离游戏进度。

## 玩家操作

- iPhone / Android / Mac：游戏内“设置 → 自定义”第一页首行的“云存档”。Mac 不再提供独立系统菜单入口。
- 云存档主页、连接预览、冲突、历史和确认均使用游戏控件。触屏点击/滑动，鼠标点击/滚轮，键盘 Tab/方向键、Enter、Esc，手柄沿用确认/返回映射；逐层返回，不改变下层设置。确认框默认取消。
- 也可把管理员为本设备生成的文件命名为 `cloud-connection.json`，放在应用 `save` 目录旁。在“云存档 → 连接设备”主动选择“导入连接文件”，核对服务和设备后明确确认，才写入安全存储；读回成功才删除明文。文件最大 4 KiB，拒绝链接和测试/正式混用，不包含在安装包内。
- 首次导入连接码后，重新打开应用。空本机下载云端，空云端上传本机；两边不同会提示选择。
- “本机已保存”表示游戏提交成功；只有“云端已同步”表示服务确认发布。保存退出或切后台会尝试上传。游戏中不按定时器上传。
- “立即同步”随时检查；游戏已经打开时下载的新进度只缓存，重新打开应用后再次核对，再安全替换。
- 冲突可选“使用本机”“使用云端”“稍后”。继续本机游玩不会覆盖另一份；选择云端前会先保存最新的本机进度。
- 历史列表支持滚动和翻页；按唯一版本编号选择，详情返回保留列表位置。恢复会产生新版本；确认期间进度改变会要求重新确认。删除最后一个角色必须额外确认。
- 游戏运行中的冲突或下载完成只显示五秒提示；未处理状态保留在入口和主页。下载可取消，失败可在主页重试；游戏界面就绪后不弹出云存档系统窗口。
- 断开连接会删除本机凭据，保留本地进度及备份。服务暂时不可达时仍可本地游玩；强制关闭应用后不保证继续上传。

R2 连接启用时不启动上游 iCloud 同步或物品合并。iCloud 原实现保留供后续上游合并。免费 Apple 账号可使用 R2，应用本身仍需按 Apple 签名有效期续签。

## 协议与数据保护

所有接口要求 `Authorization: Bearer <device-token>`，均限制请求大小。快照为标准 ZIP STORE，最多 16 MiB / 2048 个存档文件，避免解压炸弹。`manifest.json` 含协议、游戏版本、父版本、设备、保存时间、角色摘要、文件大小和 SHA-256；源码提交号只用于诊断。

| 接口 | 行为 |
| --- | --- |
| `GET /v1/status` | 当前版本、冲突及兼容性 |
| `PUT /v1/snapshots/{32位十六进制编号}` | 验证后写入不可变快照，重复相同内容安全重试 |
| `GET /v1/snapshots/{id}` | 读取快照 |
| `POST /v1/commit` | `{operation,snapshot,base,confirmEmpty}`；条件发布，分歧返回 409 并保留冲突 |
| `GET /v1/history?offset=0` | 每页最多 50 条，含冲突及下一页位置 |
| `POST /v1/resolve` | `{operation,snapshot,base,conflicts}`；显式恢复或解决冲突，重新核对当前版本 |

R2 的 `v1/index.json` 通过 ETag 条件写入同时维护当前版本、历史、冲突及操作回执。不可变快照先完整上传校验，再发布指针。客户端将操作编号、请求和 ZIP 持久化，丢失响应后重试不重复发布。恢复与普通提交都不使用设备时间决定胜负。

客户端校验路径、大小、CRC32、SHA-256、游戏版本和已知二进制格式，拒绝链接、路径穿越、大小写及 Unicode 规范化重名。替换先生成完整备份和临时目录，再原子交换目录；失败保留原存档。恢复日志用于识别交换完成但状态尚未落盘的中断。本机保留最近 5 份替换前备份，设备设置原样保留。

每日清理只回收超过 30 天且不属于最近 50 个版本的已发布进度；未解决冲突保持保留。未发布的孤立对象至少保留 48 小时，新提交对象需在上传后 24 小时内发布，以避免清理和新发布竞争。客户端过期操作会重新生成上传。已发布操作的回执保留，避免很迟的重试重新发布。

## 验证与维护

```sh
npm run check
DM_SYNC_TEST_OUT=/ABSOLUTE/ISOLATED-OUTPUT bash ../../dm/sync/tests/build.sh
DM_SYNC_DRIVER=/ABSOLUTE/ISOLATED-OUTPUT/sync-driver npm test
```

原生驱动启用 AddressSanitizer 和 UndefinedBehaviorSanitizer，以独立进程重启验证磁盘恢复。服务测试使用内存桶；原生集成测试用本机 HTTP 测试服务和独立临时存档。真实 R2 测试另用测试配置和测试令牌。合成测试角色不能导入实际游戏。

`DM_SYNC_TESTING` 只用于隔离测试构建；正式个人构建不要启用该宏。测试入口要求 `DM_SYNC_TEST=1`、隔离存档标记及 `test:true` 令牌同时成立。测试安全存储使用 Apple 独立钥匙串条目或独立 Android 包；测试连接文件导入后校验读回，再删除临时明文文件。

更新服务先在测试环境跑类型检查、核心测试和真实服务测试，再部署正式配置。维护时保留 `v1/index.json`、`v1/snapshots/` 及本地设备注册表的私人备份；不要把测试桶绑定到正式 Worker。发现错误格式或磁盘错误时先保留备份和状态文件，修复后重试，不应直接删状态文件强行覆盖。

三端应用能够编译、下载成功或隔离核心测试通过，都不等同于三端真机验收。真机记录必须另外覆盖实际游戏读写、冲突、历史恢复以及 iPhone → Android → Mac → iPhone 完整循环。

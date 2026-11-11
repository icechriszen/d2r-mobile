# 个人 R2 云存档服务

English: [README.md](README.md)

与 `dm/sync` 共用协议 1，当前支持游戏 1.0.34.0 / 存档 3.3.94012。服务只保存整套玩法存档，不保存游戏资源或画质、触控设置。部署、设备注册表和连接码均由个人管理员维护。

## 部署

需要当前项目锁定的 Node、npm 和 Wrangler。先登录自己的 Cloudflare 账号，在本目录运行：

```sh
npm ci
npx wrangler login
npx wrangler r2 bucket create d2r-private-saves
npm run deploy
```

`wrangler.jsonc` 的 `SAVES` 绑定必须指向专用私有桶。不要启用桶的公共访问。服务地址采用部署输出的 HTTPS 地址。测试必须使用 `wrangler.test.jsonc` 和独立桶，不可复用正式令牌或正式存档。

为每台设备单独生成连接码，注册表和连接码放在仓库外的私人目录：

```sh
npm run devices -- add iPhone https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev --registry /PRIVATE/devices.json
npm run devices -- add Android https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev --registry /PRIVATE/devices.json
npm run devices -- add Mac https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev --registry /PRIVATE/devices.json
npm run devices -- publish --registry /PRIVATE/devices.json
```

工具只显示连接文件路径和设备编号。连接码是完整 JSON，含服务地址、设备编号和随机访问令牌；复制文件内容到游戏“云存档 → 连接设备”。任何获得连接码的人都能访问该个人服务的存档，因此不要提交到 Git、发送到公开聊天或放进安装包。Worker 的 `DEVICES` secret 只含令牌摘要；客户端不包含 Cloudflare 管理凭据。

撤销丢失设备：

```sh
npm run devices -- list --registry /PRIVATE/devices.json
npm run devices -- revoke DEVICE_ID --registry /PRIVATE/devices.json
npm run devices -- publish --registry /PRIVATE/devices.json
```

`add` 和 `revoke` 先修改本地注册表，只有 `publish` 成功后才影响服务。测试注册时额外传 `--test --config wrangler.test.jsonc`；测试连接码不能用于正常存档目录。更换设备时生成新令牌、导入成功后撤销旧令牌。不要删除 R2 中的当前版本指针来“解决冲突”。

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

#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vendor/cJSON.h"

#define DS_LIMIT (16u * 1024u * 1024u)
#define DS_GAME "3.3.94012"
/* All functions that return JSON return owned objects. No credential is logged.
 * Transport runs on the sync worker, never on the game or UI thread. */
int ds_transport(const char *method, const char *url, const char *token,
                 const void *body, size_t size, unsigned timeout_ms,
                 unsigned char **response, size_t *response_size);
char *ds_credentials_load(void);
int ds_credentials_store(const char *json); /* NULL removes the credential */
void ds_platform_changed(void);
void ds_platform_show(void);
void ds_transport_cancel(void);
/* UTF-8 NFC + locale-independent lowercase; owned string, NULL for invalid UTF-8. */
char *ds_path_key(const char *path);

int ds_write(const char *path, const void *data, size_t size);
unsigned char *ds_read(const char *path, size_t *size);
int ds_mkdirs(const char *path);
int ds_remove(const char *path);
void ds_hash(const void *data, size_t size, char out[65]);
void ds_uuid(char out[33]);
int ds_path_allowed(const char *path);
/* ZIP uses standard STORE entries: small saves, bounded memory, no zip bombs. */
int ds_snapshot(const char *save, const char *device, const char *parent, unsigned char **zip, size_t *size, cJSON **manifest);
int ds_verify(const unsigned char *zip, size_t size, cJSON **manifest);
int ds_install(const unsigned char *zip, size_t size, const char *save, const char *backup_dir);

/* Native shell lifecycle and UI API. Commands return immediately and run serially. */
int dm_r2_boot(void);              /* before mounting save; 1 also suppresses iCloud */
int dm_r2_enabled(void);
void dm_r2_flush(int background);
void dm_r2_foreground(void);
int dm_r2_wait(double seconds);
void dm_r2_show(void);
void dm_r2_cancel(void);
void dm_r2_progress(size_t received, size_t total);
char *dm_r2_status(void);          /* JSON status, history, local/remote summaries */
const char *dm_r2_message(const char *code);
int dm_r2_connect(const char *connection_json);
/* Explicit local-file onboarding; returns public endpoint metadata only. */
char *dm_r2_connection_file(void);
int dm_r2_import_connection(void); /* 0 success, 1 saved but plaintext cleanup failed */
int dm_r2_disconnect(void);
void dm_r2_command(const char *action, const char *revision);
void dm_r2_on_commit(const char *save);

/* 游戏内界面请求带编号和确认依据；0 表示忙或不可用。
 * In-game UI requests carry an ID and confirmation baseline; zero means busy/unavailable. */
uint64_t dm_r2_submit(const char *action, const char *revision, const char *head, const char *local_hash);
char *dm_r2_connection_preview(const char *json);
void ds_secret_free(char *text);
/* 只有明确的粘贴操作读取剪贴板；回调接收可释放的 UTF-8。
 * Only explicit Paste reads the clipboard; the callback owns the UTF-8 result. */
void ds_platform_paste(void (*complete)(char *));

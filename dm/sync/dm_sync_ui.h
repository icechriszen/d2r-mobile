// 云存档共用页面模型：不依赖平台窗口或游戏控件指针。
// Shared cloud-save page model: independent of platform windows and game widget pointers.
#pragma once
#include <stdint.h>
#define DS_UI_ACTIONS 14
#define DS_UI_TEXT 12288
typedef struct { char action[64], label[512]; int enabled; } ds_ui_button;
typedef struct {
    int page, count, selection, columns; unsigned generation;
    char title[160], status[1024], left[DS_UI_TEXT], right[DS_UI_TEXT];
    ds_ui_button buttons[DS_UI_ACTIONS];
} ds_ui_view;
void dm_sync_ui_open(void);
void dm_sync_ui_changed(void);
void dm_sync_ui_tick(void);
void dm_sync_ui_close(void);
int dm_sync_ui_active(void);
void dm_sync_ui_action(const char *action);
void dm_sync_ui_select(int delta);
const ds_ui_view *dm_sync_ui_view(void);
/* 游戏主线程更新控件；前端可排队键盘操作。
 * The game thread updates widgets; front ends may enqueue keyboard actions. */
void dm_sync_ui_pump(uint64_t root);
uint64_t dm_sync_ui_modal(uint64_t root);
int dm_sync_ui_key(int key, int down);
void dm_sync_ui_scroll(int notches);
int dm_sync_ui_ready(void);
void dm_sync_ui_on_message(const char *category, const char *action, const char *argument);

uint64_t dm_sync_ui_filter(uint64_t buttons,int32_t *lx,int32_t *ly,int32_t *rx,int32_t *ry);

const char *dm_sync_ui_notice(void);
const char *dm_sync_ui_status_text(void);

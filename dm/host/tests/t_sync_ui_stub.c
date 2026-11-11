// 原有输入单测不加载云存档面板；完整状态机另由 sync/tests 覆盖。
// Existing input tests do not load cloud panels; sync/tests covers the full state machine.
#include "../../sync/dm_sync_ui.h"
int dm_sync_ui_active(void){return 0;}
uint64_t dm_sync_ui_modal(uint64_t root){(void)root;return 0;}
int dm_sync_ui_key(int key,int down){(void)key;(void)down;return 0;}
void dm_sync_ui_on_message(const char *category,const char *action,const char *argument){(void)category;(void)action;(void)argument;}
uint64_t dm_sync_ui_filter(uint64_t buttons,int32_t *lx,int32_t *ly,int32_t *rx,int32_t *ry){(void)lx;(void)ly;(void)rx;(void)ry;return buttons;}

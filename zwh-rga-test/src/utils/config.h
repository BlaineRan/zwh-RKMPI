#pragma once

// 输入源与切分配置
#define SRC_WIDTH   1920
#define SRC_HEIGHT  1080
#define SPLIT_ROW   4
#define SPLIT_COL   4
#define TOTAL_CHNS  (SPLIT_ROW * SPLIT_COL) // 16 路子通道
#define SUB_WIDTH   (SRC_WIDTH / SPLIT_COL) // 子画面宽度 480
#define SUB_HEIGHT  (SRC_HEIGHT / SPLIT_ROW) // 子画面高度 270

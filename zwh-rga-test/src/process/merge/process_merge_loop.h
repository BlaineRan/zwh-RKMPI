#pragma once

#include "utils/rtsp_helper.h"
#include "utils/config.h"

// 合并 4x4 子画面到单路输出的处理循环：
// - 当前示例跳过 tile 0，填黑
// - 其余 15 路按原位置放回 1920x1080 画布
// - 输出到新 RTSP session (/live/merged) 和新 VENC 通道
void ProcessMergedFrames(const RtspContext &ctx);

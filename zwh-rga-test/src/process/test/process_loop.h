#pragma once

#include "utils/rtsp_helper.h"
#include "utils/pipeline_init.h"

// 主处理循环：采集 -> 裁剪 -> 编码 -> RTSP 推流
void ProcessFrames(const RtspContext &ctx, MB_POOL subImgPool);

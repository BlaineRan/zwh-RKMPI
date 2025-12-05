#pragma once

#include <vector>
#include "rtsp_demo.h"
#include "config.h"

// RTSP 相关上下文
struct RtspContext {
    rtsp_demo_handle demo = NULL;
    std::vector<rtsp_session_handle> sessions;
};

// 初始化 RTSP 服务与会话
bool InitRtsp(RtspContext &ctx);

// 释放 RTSP 资源
void CleanupRtsp(RtspContext &ctx);

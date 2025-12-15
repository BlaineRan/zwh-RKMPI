#pragma once

#include <cstddef>
#include <cstdint>

#include "utils/rtsp_helper.h"
#include "utils/config.h"

// 初始化模拟接收端（创建合成 VENC/画布池）。成功返回 true。
bool InitSimulatedReceiver(const RtspContext *ctx);

// 当前接收端是否就绪。
bool SimulatedReceiverReady();

// 将单个 tile 的 NV12 数据送入接收端，内部按位置拼成 1080P 再推一路 /live/0。
void SendTileToSimulatedReceiver(int tileId,
                                 uint16_t frameSeq,
                                 uint16_t tileMask,
                                 uint64_t pts,
                                 const void *data,
                                 size_t size);

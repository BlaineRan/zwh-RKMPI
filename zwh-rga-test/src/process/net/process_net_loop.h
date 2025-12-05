#pragma once

#include "utils/luckfox_mpi.h"
#include "utils/config.h"

// 功能：采集一帧 1080P，按 4x4 网格裁剪为子画面，跳过当前秒对应的 tile，
//       将其余子画面的码流（NV12 原始数据）交给模拟的网络发送函数。
// 参数：
//   subImgPool - 供裁剪输出使用的 NV12 内存池（尺寸需满足 480x270，一般复用 CreateSubImgPool 创建的池）
// 返回值：无；内部为死循环，除非发生致命错误（当前实现直接继续或返回）
void ProcessNetLoop(MB_POOL subImgPool);

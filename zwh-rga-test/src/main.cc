/*****************************************************************************
 * RV1106 4x4 多路编码演示流程 (VI -> RGA -> 16x VENC -> 16x RTSP)
 * 说明：演示如何将 1080P 摄像头输入切分为 4x4 共 16 路子画面，
 *      依次送入硬件编码器，再通过 RTSP 推流。重点展示 ISP 初始化、
 *      RGA 裁剪、VENC 编码以及 RTSP 会话管理的基本用法。
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "utils/config.h"
#include "utils/rtsp_helper.h"
#include "utils/pipeline_init.h"
#include "process/test/process_loop.h"
#include "process/merge/process_merge_loop.h"

int main(int argc, char *argv[]) {
    // 切换开关：true 仅输出合并后的 /live/merged；false 输出 16 路 /live/0~15
    const bool kUseMergedOutput = true;

    // 初始化基础 MPI 系统
    if (!InitMpiSys()) return -1;

    // 开启 ISP（RKAIQ），确保自动曝光/白平衡等 3A 生效
    StartIsp();

    // 创建 RTSP Server，并为 16 路编码分别建立 session
    RtspContext rtspCtx;
    if (!InitRtsp(rtspCtx)) return -1;

    // 初始化 VI，打开摄像头并设置 1080P 输入
    InitViInput();

    if (kUseMergedOutput) {
        // 仅跑合并模式：跳过 tile0，其他 15 路拼回 1080P，推送到 /live/merged
        ProcessMergedFrames(rtspCtx);
    } else {
        // 原始模式：16 路独立编码 + 推流
        // 第 3 步：为 16 路子画面创建独立编码通道
        if (!InitVencChannels()) return -1;

        // 第 4 步：预分配子画面内存池，方便 RGA 输出和 VENC 复用
        MB_POOL subImgPool;
        if (!CreateSubImgPool(subImgPool)) return -1;

        // 第 5 步：进入 16 路裁剪编码推流
        ProcessFrames(rtspCtx, subImgPool);

        // 释放子画面池
        RK_MPI_MB_DestroyPool(subImgPool);
    }

    // 第 6 步：退出前关闭 ISP（正常不会走到这里）
    StopIsp();
    CleanupRtsp(rtspCtx);
    RK_MPI_SYS_Exit();
    return 0;
}

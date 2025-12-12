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
#include "process/net/process_net_loop.h"

int main(int argc, char *argv[]) {
    // mode: 0=原始16路推流；1=合并推流；2=网络裁剪传输测试
    int mode = 0;
    if (argc > 1) {
        mode = atoi(argv[1]);
        
    }
    printf("Run mode: %d (0=16ch, 1=merged, 2=net test)\n", mode);

    // 初始化基础 MPI 系统
    if (!InitMpiSys()) {
        printf("InitMpiSys failed\n");
        return -1;
    }

    // 开启 ISP（RKAIQ），确保自动曝光/白平衡等 3A 生效
    StartIsp();

    // 创建 RTSP Server，并为 16 路编码分别建立 session
    RtspContext rtspCtx;

    if (!InitRtsp(rtspCtx)) {
        printf("InitRtsp failed\n");
        return -1;
    }

    // 初始化 VI，打开摄像头并设置 1080P 输入
    InitViInput();

    MB_POOL subImgPool = MB_INVALID_POOLID;
    if (mode != 1) {
        if (!CreateSubImgPool(subImgPool)) {
            printf("CreateSubImgPool failed\n");
            return -1;
        }
    }

    if (mode == 1) {
        // 合并模式：跳过一个 tile 的同时拼回 1080P，推送 /live/merged
        ProcessMergedFrames(rtspCtx, subImgPool);
    } else if (mode == 2) {
        // 网络裁剪传输测试：裁剪子画面后走 SendTileOverNetwork_Test
        ProcessNetLoop(rtspCtx, subImgPool);
    } else {
        // 原始模式：16 路独立编码 + 推流
        if (!InitVencChannels()) {
            printf("InitVencChannels failed\n");
            return -1;
        }
        ProcessFrames(rtspCtx, subImgPool);
    }

    if (subImgPool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(subImgPool);
    }

    // 第 6 步：退出前关闭 ISP（正常不会走到这里）
    StopIsp();
    CleanupRtsp(rtspCtx);
    RK_MPI_SYS_Exit();
    return 0;
}

#include "process_net_loop.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "im2d.h"
#include "rga.h"

// 功能：获取当前时间（毫秒）
// 参数：无
// 返回值：uint64_t 毫秒时间戳
static uint64_t GetMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

// 功能：模拟网络发送接口，实际业务可在此接入 socket/RTSP/自定义协议
// 参数：
//   tileId - 子画面编号（0~15）
//   data   - 指向 NV12 数据的指针
//   size   - 数据字节数
//   pts    - 时间戳（来自 VI 帧的 PTS）
// 返回值：无
static void SendTileOverNetwork(int tileId, const void *data, size_t size, uint64_t pts) {
    (void)tileId;
    (void)data;
    (void)size;
    (void)pts;
    // TODO: 在实际场景中将数据通过网络发送出去
}

// 功能：裁剪并发送子画面到网络（跳过当前秒对应的 tile）
// 参数：
//   subImgPool - 供裁剪输出使用的 NV12 内存池
// 返回值：无（内部死循环）
void ProcessNetLoop(MB_POOL subImgPool) {
    if (subImgPool == MB_INVALID_POOLID) {
        printf("ProcessNetLoop: subImgPool invalid\n");
        return;
    }

    static uint64_t startMs = GetMs();

    VIDEO_FRAME_INFO_S stViFrame;

    while (1) {
        // 从 VI 获取一帧 1080P 原始图像
        RK_S32 s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, 1000);
        if (s32Ret != RK_SUCCESS) {
            continue;
        }

        // 计算本秒需要跳过的 tile（0~15 循环）
        uint64_t elapsedMs = GetMs() - startMs;
        int skipTile = (elapsedMs / 1000) % TOTAL_CHNS;

        int vi_fd = RK_MPI_MB_Handle2Fd(stViFrame.stVFrame.pMbBlk);

        // 源图封装成 RGA buffer
        rga_buffer_t src_img;
        src_img = wrapbuffer_fd(vi_fd, SRC_WIDTH, SRC_HEIGHT, RK_FORMAT_YCbCr_420_SP, SRC_WIDTH, SRC_HEIGHT);

        // 按网格裁剪
        for (int r = 0; r < SPLIT_ROW; r++) {
            for (int c = 0; c < SPLIT_COL; c++) {
                int tileId = r * SPLIT_COL + c;
                if (tileId == skipTile) continue;

                MB_BLK dst_Blk = RK_MPI_MB_GetMB(subImgPool, SUB_WIDTH * SUB_HEIGHT * 3 / 2, RK_TRUE);
                int dst_fd = RK_MPI_MB_Handle2Fd(dst_Blk);

                rga_buffer_t dst_img;
                dst_img = wrapbuffer_fd(dst_fd, SUB_WIDTH, SUB_HEIGHT, RK_FORMAT_YCbCr_420_SP, SUB_WIDTH, SUB_HEIGHT);

                // 裁剪区域
                im_rect src_rect;
                src_rect.x = c * SUB_WIDTH;
                src_rect.y = r * SUB_HEIGHT;
                src_rect.width = SUB_WIDTH;
                src_rect.height = SUB_HEIGHT;

                if (imcheck(src_img, dst_img, src_rect, {}) == IM_STATUS_NOERROR) {
                    imcrop(src_img, dst_img, src_rect);
                }

                // 缓存同步，确保 CPU 读取裁剪结果前刷新
                RK_MPI_SYS_MmzFlushCache(dst_Blk, RK_FALSE);

                // NV12 数据指针与大小
                void *data = RK_MPI_MB_Handle2VirAddr(dst_Blk);
                size_t size = SUB_WIDTH * SUB_HEIGHT * 3 / 2;

                // 发送到模拟网络
                SendTileOverNetwork(tileId, data, size, stViFrame.stVFrame.u64PTS);

                // 释放子画面缓冲
                RK_MPI_MB_ReleaseMB(dst_Blk);
            }
        }

        // 处理完一帧，释放 VI 帧
        RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
    }
}

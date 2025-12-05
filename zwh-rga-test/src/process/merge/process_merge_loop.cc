#include "process_merge_loop.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "utils/luckfox_mpi.h"
#include "rtsp_demo.h"

// 处理参数
// 跳过的 tile 将按秒轮换：当前秒 % TOTAL_CHNS
// 默认使用 VENC 0 通道用于合成输出；如需与 16 路并行，请改成空闲的通道 ID
static const int kMergedChnId = 0;
static const char *kMergedRtspPath = "/live/merged";

// 简单的 VENC 初始化（1920x1080 NV12 -> H.264）
static bool InitMergedVenc() {
    VENC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32Profile = 66;
    attr.stVencAttr.u32PicWidth = SRC_WIDTH;
    attr.stVencAttr.u32PicHeight = SRC_HEIGHT;
    attr.stVencAttr.u32VirWidth = SRC_WIDTH;
    attr.stVencAttr.u32VirHeight = SRC_HEIGHT;
    attr.stVencAttr.u32BufSize = SRC_WIDTH * SRC_HEIGHT * 2;

    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32Gop = 25;
    attr.stRcAttr.stH264Cbr.u32BitRate = 2048; // 更大的分辨率，提升码率
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 25;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;

    RK_S32 ret = RK_MPI_VENC_CreateChn(kMergedChnId, &attr);
    if (ret != RK_SUCCESS) {
        printf("Create merged VENC Chn %d failed: 0x%x\n", kMergedChnId, ret);
        return false;
    }

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    RK_MPI_VENC_StartRecvFrame(kMergedChnId, &recv);
    return true;
}

// 为 1920x1080 画布创建池
static MB_POOL CreateMergedPool() {
    MB_POOL_CONFIG_S cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.u64MBSize = SRC_WIDTH * SRC_HEIGHT * 3 / 2; // NV12
    cfg.u32MBCnt = 2;                               // 双缓冲足够
    cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    return RK_MPI_MB_CreatePool(&cfg);
}

// 将源 NV12 的一块区域按原位置复制到目标画布（CPU 拷贝，带 stride）
static void CopyTileToCanvas(void *srcBase, void *dstBase, int x, int y, int srcStride, int dstStride) {
    uint8_t *src = static_cast<uint8_t *>(srcBase);
    uint8_t *dst = static_cast<uint8_t *>(dstBase);

    // Y 平面
    for (int row = 0; row < SUB_HEIGHT; ++row) {
        uint8_t *srcLine = src + (y + row) * srcStride + x;
        uint8_t *dstLine = dst + (y + row) * dstStride + x;
        memcpy(dstLine, srcLine, SUB_WIDTH);
    }

    // UV 平面（高度减半）
    uint8_t *srcUV = src + srcStride * SRC_HEIGHT;
    uint8_t *dstUV = dst + dstStride * SRC_HEIGHT;
    int uvY = y / 2;
    for (int row = 0; row < SUB_HEIGHT / 2; ++row) {
        uint8_t *srcLine = srcUV + (uvY + row) * srcStride + x;
        uint8_t *dstLine = dstUV + (uvY + row) * dstStride + x;
        memcpy(dstLine, srcLine, SUB_WIDTH);
    }
}

static uint64_t GetMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

void ProcessMergedFrames(const RtspContext &ctx) {
    if (!ctx.demo) {
        printf("RTSP demo not initialized.\n");
        return;
    }

    // 初始化合并编码器与画布池
    static bool inited = false;
    static MB_POOL canvasPool = MB_INVALID_POOLID;
    static rtsp_session_handle mergedSession = NULL;
    static uint64_t startMs = 0;
    if (!inited) {
        if (!InitMergedVenc()) return;
        canvasPool = CreateMergedPool();
        if (canvasPool == MB_INVALID_POOLID) {
            printf("Create merged canvas pool failed.\n");
            return;
        }
        mergedSession = rtsp_new_session(ctx.demo, kMergedRtspPath);
        if (mergedSession) {
            rtsp_set_video(mergedSession, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
            rtsp_sync_video_ts(mergedSession, rtsp_get_reltime(), rtsp_get_ntptime());
            printf("RTSP Merged Session: rtsp://<IP>:554%s\n", kMergedRtspPath);
        }
        startMs = GetMs();
        inited = true;
    }

    VIDEO_FRAME_INFO_S stViFrame;
    VENC_STREAM_S stStream;
    stStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

    while (1) {
        // 拿一帧全幅 1080P
        RK_S32 s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, 1000);
        if (s32Ret == RK_SUCCESS) {
            // 取一块画布并清零（填黑背景）
            MB_BLK canvasBlk = RK_MPI_MB_GetMB(canvasPool, SRC_WIDTH * SRC_HEIGHT * 3 / 2, RK_TRUE);
            void *canvasVir = RK_MPI_MB_Handle2VirAddr(canvasBlk);
            memset(canvasVir, 0, SRC_WIDTH * SRC_HEIGHT * 3 / 2);

            // 源帧虚拟地址
            void *srcVir = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
            int srcStride = stViFrame.stVFrame.u32VirWidth ? stViFrame.stVFrame.u32VirWidth : SRC_WIDTH;
            int dstStride = SRC_WIDTH;

            // 确保 CPU 读取 VI 帧前同步缓存
            RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_FALSE);

            // 当前秒内跳过的 tile：秒数 mod TOTAL_CHNS
            uint64_t elapsedMs = GetMs() - startMs;
            int skipTile = (elapsedMs / 1000) % TOTAL_CHNS;

            // 按 4x4 网格复制，跳过 tile 0（0,0）保持黑色
            for (int r = 0; r < SPLIT_ROW; ++r) {
                for (int c = 0; c < SPLIT_COL; ++c) {
                    int tileId = r * SPLIT_COL + c;
                    if (tileId == skipTile) continue;
                    int x = c * SUB_WIDTH;
                    int y = r * SUB_HEIGHT;
                    CopyTileToCanvas(srcVir, canvasVir, x, y, srcStride, dstStride);
                }
            }

            // 写完画布后，刷新缓存以供 VENC 读取
            RK_MPI_SYS_MmzFlushCache(canvasBlk, RK_TRUE);

            // 封装整幅帧送入合并编码通道
            VIDEO_FRAME_INFO_S vencFrame;
            memset(&vencFrame, 0, sizeof(vencFrame));
            vencFrame.stVFrame.u32Width = SRC_WIDTH;
            vencFrame.stVFrame.u32Height = SRC_HEIGHT;
            vencFrame.stVFrame.u32VirWidth = SRC_WIDTH;
            vencFrame.stVFrame.u32VirHeight = SRC_HEIGHT;
            vencFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
            vencFrame.stVFrame.pMbBlk = canvasBlk;
            vencFrame.stVFrame.u64PTS = stViFrame.stVFrame.u64PTS;

            RK_MPI_VENC_SendFrame(kMergedChnId, &vencFrame, -1);
            RK_MPI_MB_ReleaseMB(canvasBlk);

            // 取码流推送到合并 RTSP
            if (RK_MPI_VENC_GetStream(kMergedChnId, &stStream, 0) == RK_SUCCESS) {
                void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
                if (mergedSession) {
                    rtsp_tx_video(mergedSession, (uint8_t *)pData, stStream.pstPack->u32Len,
                                  stStream.pstPack->u64PTS);
                }
                RK_MPI_VENC_ReleaseStream(kMergedChnId, &stStream);
            }

            if (ctx.demo) rtsp_do_event(ctx.demo);
            RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
        }
    }

    free(stStream.pstPack);
}

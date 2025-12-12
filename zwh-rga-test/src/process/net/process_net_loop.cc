#include "process_net_loop.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "im2d.h"
#include "rga.h"
#include "rtsp_helper.h"

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

// 功能：本地测试用的接收端模拟，将收到的 tile 重新拼成 1080P 并推送到 rtsp://<IP>:554/live/0
// 说明：按照 tileId 的 4x4 网格位置复制到画布，未收到的 tile 保持黑色；帧边界以 PTS 切换
static const RtspContext *g_rtspCtx = nullptr;

static void SendTileOverNetwork_Test(int tileId, const void *data, size_t size, uint64_t pts) {
    // 初始化一次 RTSP、VENC 以及画布池
    const int kTestChnId = 0;
    static bool inited = false;
    static rtsp_demo_handle demo = NULL;
    static rtsp_session_handle session = NULL;
    static MB_POOL canvasPool = MB_INVALID_POOLID;
    static MB_BLK canvasBlk = MB_INVALID_HANDLE;
    static void *canvasVir = NULL;
    static uint64_t currentPts = 0;
    static bool hasPts = false;
    static uint64_t lastFlushMs = 0;
    static VENC_STREAM_S stStream;
    static bool streamInited = false;

    // 初始化辅助：失败时打印一次并直接返回
    if (!inited) {
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
        attr.stRcAttr.stH264Cbr.u32BitRate = 2048;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 25;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;

        if (RK_MPI_VENC_CreateChn(kTestChnId, &attr) != RK_SUCCESS) {
            printf("SendTileOverNetwork_Test: create VENC failed\n");
            return;
        }
        VENC_RECV_PIC_PARAM_S recv;
        memset(&recv, 0, sizeof(recv));
        recv.s32RecvPicNum = -1;
        RK_MPI_VENC_StartRecvFrame(kTestChnId, &recv);

        MB_POOL_CONFIG_S cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.u64MBSize = SRC_WIDTH * SRC_HEIGHT * 3 / 2;
        cfg.u32MBCnt = 2;
        cfg.enAllocType = MB_ALLOC_TYPE_DMA;
        canvasPool = RK_MPI_MB_CreatePool(&cfg);
        if (canvasPool == MB_INVALID_POOLID) {
            printf("SendTileOverNetwork_Test: create canvas pool failed\n");
            return;
        }

        // 复用外部 RTSP demo/session，避免重复开端口
        if (g_rtspCtx && g_rtspCtx->demo) {
            demo = g_rtspCtx->demo;
            if (g_rtspCtx->sessions.size() > 0) {
                session = g_rtspCtx->sessions[0];
            }
        }
        if (!session && demo) {
            session = rtsp_new_session(demo, "/live/0");
        }
        // 确保会话配置好视频属性
        if (session) {
            rtsp_set_video(session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
            rtsp_sync_video_ts(session, rtsp_get_reltime(), rtsp_get_ntptime());
        }
        if (session) {
            printf("SendTileOverNetwork_Test: rtsp://<IP>:554/live/0 ready\n");
        } else {
            printf("SendTileOverNetwork_Test: RTSP session not ready\n");
        }

        if (!streamInited) {
            stStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
            streamInited = true;
        }

        inited = true;
    }

    if (!streamInited || canvasPool == MB_INVALID_POOLID) return;
    if (tileId < 0 || tileId >= TOTAL_CHNS || !data || size < SUB_WIDTH * SUB_HEIGHT * 3 / 2) return;

    // 辅助 lambda：将当前画布送入编码并推 RTSP，然后清空画布句柄
    auto flushAndSend = [&]() {
        if (canvasBlk == MB_INVALID_HANDLE) return;
        RK_MPI_SYS_MmzFlushCache(canvasBlk, RK_TRUE);

        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        frame.stVFrame.u32Width = SRC_WIDTH;
        frame.stVFrame.u32Height = SRC_HEIGHT;
        frame.stVFrame.u32VirWidth = SRC_WIDTH;
        frame.stVFrame.u32VirHeight = SRC_HEIGHT;
        frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        frame.stVFrame.pMbBlk = canvasBlk;
        frame.stVFrame.u64PTS = currentPts;

        RK_S32 sendRet = RK_MPI_VENC_SendFrame(kTestChnId, &frame, -1);
        if (sendRet != RK_SUCCESS) {
            printf("[NET] VENC_SendFrame ret=0x%x pts=%llu\n", sendRet, (unsigned long long)currentPts);
        }
        RK_MPI_MB_ReleaseMB(canvasBlk);
        canvasBlk = MB_INVALID_HANDLE;
        canvasVir = NULL;

        if (RK_MPI_VENC_GetStream(kTestChnId, &stStream, 0) == RK_SUCCESS) {
            void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
            if (session) {
                rtsp_tx_video(session, (uint8_t *)pData, stStream.pstPack->u32Len, stStream.pstPack->u64PTS);
            }
            static uint64_t pushCnt = 0;
            pushCnt++;
            if (pushCnt % 30 == 0) {
                printf("[NET] push #%llu len=%u pts=%llu\n",
                       (unsigned long long)pushCnt,
                       stStream.pstPack->u32Len,
                       (unsigned long long)stStream.pstPack->u64PTS);
            }
            RK_MPI_VENC_ReleaseStream(kTestChnId, &stStream);
            if (demo) rtsp_do_event(demo);
        }
        lastFlushMs = GetMs();
    };

    // 新的 PTS 或超时（同一 PTS 未刷新超过 100ms）到来，先把上一帧送出
    if (hasPts && (pts != currentPts || (GetMs() - lastFlushMs) > 100)) {
        flushAndSend();
        hasPts = false;
    }

    // 为当前 PTS 准备画布
    if (!hasPts) {
        canvasBlk = RK_MPI_MB_GetMB(canvasPool, SRC_WIDTH * SRC_HEIGHT * 3 / 2, RK_TRUE);
        canvasVir = RK_MPI_MB_Handle2VirAddr(canvasBlk);
        memset(canvasVir, 0, SRC_WIDTH * SRC_HEIGHT * 3 / 2); // 未收到的 tile 保持黑色
        currentPts = pts;
        hasPts = true;
        lastFlushMs = GetMs();
    }

    // 按 tileId 的行列位置复制到画布
    int row = tileId / SPLIT_COL;
    int col = tileId % SPLIT_COL;
    int x = col * SUB_WIDTH;
    int y = row * SUB_HEIGHT;
    const uint8_t *src = static_cast<const uint8_t *>(data);
    uint8_t *dst = static_cast<uint8_t *>(canvasVir);
    int dstStride = SRC_WIDTH;
    int srcStride = SUB_WIDTH;

    for (int r = 0; r < SUB_HEIGHT; ++r) {
        memcpy(dst + (y + r) * dstStride + x, src + r * srcStride, SUB_WIDTH);
    }

    const uint8_t *srcUV = src + SUB_WIDTH * SUB_HEIGHT;
    uint8_t *dstUV = dst + SRC_WIDTH * SRC_HEIGHT;
    int uvY = y / 2;
    for (int r = 0; r < SUB_HEIGHT / 2; ++r) {
        memcpy(dstUV + (uvY + r) * dstStride + x, srcUV + r * srcStride, SUB_WIDTH);
    }
}

// 功能：裁剪并发送子画面到网络（跳过当前秒对应的 tile）
// 参数：
//   subImgPool - 供裁剪输出使用的 NV12 内存池
// 返回值：无（内部死循环）
void ProcessNetLoop(const RtspContext &ctx, MB_POOL subImgPool) {
    if (subImgPool == MB_INVALID_POOLID) {
        printf("ProcessNetLoop: subImgPool invalid\n");
        return;
    }

    g_rtspCtx = &ctx;
    printf("ProcessNetLoop start: subImgPool=%p\n", subImgPool);

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
                SendTileOverNetwork_Test(tileId, data, size, stViFrame.stVFrame.u64PTS);

                // 释放子画面缓冲
                RK_MPI_MB_ReleaseMB(dst_Blk);
            }
        }

        // 处理完一帧，释放 VI 帧
        RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);

        // 统一处理 RTSP 事件
        if (ctx.demo) rtsp_do_event(ctx.demo);
    }
}

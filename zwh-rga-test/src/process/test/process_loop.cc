// 采集-裁剪-编码-推流核心循环实现
// - 从 VI 通道抓取一帧 1080P NV12 原始图
// - 使用 RGA 将大画面按 4x4 网格裁剪成 16 个子画面
// - 逐路送入对应 VENC 编码，再推送到各自的 RTSP 会话
#include "process_loop.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "im2d.h"
#include "rga.h"

// 获取当前时间（毫秒），用于统计窗口
static uint64_t GetMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

// 全局统计变量
static uint64_t sentCnt[TOTAL_CHNS] = {0};
static const uint64_t kStatWindowMs = 500; // 修改此值即可调整统计粒度
static uint64_t statStartMs = 0;
static uint64_t statTile0Bytes = 0;
static uint64_t statTotalBytes = 0;
static VIDEO_FRAME_INFO_S viFrame;
static VENC_STREAM_S stream;
static uint16_t frameSeq = 0;      // 本地帧序号，发送时带上供接收端聚合
static uint16_t tileMask = 0xFFFF; // 如果未来只发部分 tile，请更新掩码位
static bool isIdrFrame = false;    // 本帧是否为 I/IDR 帧
static uint64_t framePts = 0;

// 仅用于占位的“网络发送”接口，未来会将编码好的 tile 送往另一台设备
// 关键元信息：
// - tileId：当前子画面编号
// - frameSeq：当前帧的序号（本地递增，接收端据此聚合一帧）
// - tileMask：本帧实际发送的 tile 掩码，接收端可据此判断缺失的 tile 并补黑
// - pts：沿用 VI 帧 PTS，保证时间对齐
// - data/size：编码后的码流数据
static void SendTileOverNetworkPlaceholder(int tileId,
                                           uint16_t frameSeq,
                                           uint16_t tileMask,
                                           uint64_t pts,
                                           const void *data,
                                           size_t size) {
    (void)tileId;
    (void)frameSeq;
    (void)tileMask;
    (void)pts;
    (void)data;
    (void)size;
    // TODO: 未来在此封装协议/传输，携带 frameSeq+tileMask 让接收端按帧拼接
}

// 处理单个 tile 的裁剪、编码、发送及统计
static void ProcessSingleTile(int r,
                              int c,
                              int chnId,
                              const RtspContext &ctx,
                              MB_POOL subImgPool,
                              const rga_buffer_t &src_img,
                              uint64_t pts) {
    MB_BLK dst_Blk = RK_MPI_MB_GetMB(subImgPool, SUB_WIDTH * SUB_HEIGHT * 3 / 2, RK_TRUE);
    int dst_fd = RK_MPI_MB_Handle2Fd(dst_Blk);

    rga_buffer_t dst_img;
    dst_img = wrapbuffer_fd(dst_fd, SUB_WIDTH, SUB_HEIGHT, RK_FORMAT_YCbCr_420_SP, SUB_WIDTH, SUB_HEIGHT);

    im_rect src_rect;
    src_rect.x = c * SUB_WIDTH;
    src_rect.y = r * SUB_HEIGHT;
    src_rect.width = SUB_WIDTH;
    src_rect.height = SUB_HEIGHT;

    if (imcheck(src_img, dst_img, src_rect, {}) == IM_STATUS_NOERROR) {
        imcrop(src_img, dst_img, src_rect);
    }

    RK_MPI_SYS_MmzFlushCache(dst_Blk, RK_TRUE);

    VIDEO_FRAME_INFO_S stVencFrame;
    memset(&stVencFrame, 0, sizeof(VIDEO_FRAME_INFO_S));
    stVencFrame.stVFrame.u32Width = SUB_WIDTH;
    stVencFrame.stVFrame.u32Height = SUB_HEIGHT;
    stVencFrame.stVFrame.u32VirWidth = SUB_WIDTH;
    stVencFrame.stVFrame.u32VirHeight = SUB_HEIGHT;
    stVencFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP; 
    stVencFrame.stVFrame.pMbBlk = dst_Blk; 
    stVencFrame.stVFrame.u64PTS = pts;

    RK_S32 sendRet = RK_MPI_VENC_SendFrame(chnId, &stVencFrame, -1);
    if (sendRet != RK_SUCCESS) {
        printf("VENC_SendFrame ch%d ret=0x%x\n", chnId, sendRet);
    }
    RK_MPI_MB_ReleaseMB(dst_Blk);

    if (RK_MPI_VENC_GetStream(chnId, &stream, 0) == RK_SUCCESS) {
        void *pData = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
        if (ctx.demo && ctx.sessions[chnId]) {
            rtsp_tx_video(ctx.sessions[chnId], (uint8_t *)pData, stream.pstPack->u32Len,
                          stream.pstPack->u64PTS);
        }
        sentCnt[chnId]++;
        SendTileOverNetworkPlaceholder(chnId,
                                       frameSeq,
                                       tileMask,
                                       stream.pstPack->u64PTS,
                                       pData,
                                       stream.pstPack->u32Len);

        statTotalBytes += stream.pstPack->u32Len;
        if (chnId == 0) {
            statTile0Bytes += stream.pstPack->u32Len;
        }
        
        if (stream.pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
            stream.pstPack->DataType.enH264EType == H264E_NALU_ISLICE) {
            isIdrFrame = true;
        }
        uint64_t nowMs = GetMs();
        if (nowMs - statStartMs >= kStatWindowMs) {
            double ratio = (statTotalBytes > 0) ? (statTile0Bytes * 100.0 / statTotalBytes) : 0.0;
            // printf("[STAT %.1fms] tile0=%llu bytes, total=%llu bytes, ratio=%.2f%%\n",
            //        kStatWindowMs * 1.0,
            //        (unsigned long long)statTile0Bytes,
            //        (unsigned long long)statTotalBytes,
            //        ratio);
            statTile0Bytes = 0;
            statTotalBytes = 0;
            statStartMs = nowMs;
        }
        RK_MPI_VENC_ReleaseStream(chnId, &stream);
    } else {
        static uint64_t failCnt[TOTAL_CHNS] = {0};
        failCnt[chnId]++;
        if (failCnt[chnId] % 30 == 0) {
            printf("VENC_GetStream ch%d fail cnt=%llu\n", chnId, (unsigned long long)failCnt[chnId]);
        }
    }
}

// 核心流程入口：把一帧 1080P 原始视频拆成 16 份、分别编码并推流
// 设计思路：
// 1) VI 拉一帧 NV12 原始图（1920x1080）
// 2) 用 RGA 硬件按 4x4 网格裁剪，得到 16 个小块（每块 SUB_WIDTH x SUB_HEIGHT）
// 3) 16 个 VENC 通道一一对应，每块送入对应通道编码
// 4) 取出每个通道的码流，推到对应 RTSP session；同时调用占位的“网络发送”接口
// 这样做的好处：每个 tile 有独立码率/通道，便于统计和按需传输
void ProcessFrames(const RtspContext &ctx, MB_POOL subImgPool) {
    printf("ProcessFrames start: subImgPool=%p\n", subImgPool);

    // stViFrame：从 VI 拿到的一帧原始图，stStream：从 VENC 拿到的一帧码流
    stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

    while(1) {
        
        // STEP 1: 从 VI 拉取一帧图像（NV12，1080P），超时 1000ms
        RK_S32 s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &viFrame, 1000);
        if(s32Ret == RK_SUCCESS) {
            if (statStartMs == 0) {
                statStartMs = GetMs();
            }
            // 每取到一帧源图，递增帧序号；tileMask 默认认为 16 个 tile 都会发送
            frameSeq++;
            tileMask = 0xFFFF; // 如果未来只发部分 tile，请更新掩码位
            isIdrFrame = false;    // 本帧是否为 I/IDR 帧
            framePts = viFrame.stVFrame.u64PTS;
            int vi_fd = RK_MPI_MB_Handle2Fd(viFrame.stVFrame.pMbBlk); // 获取VI的句柄

            // 将 VI 帧封装成 RGA 可识别的源 buffer
            rga_buffer_t src_img;
            src_img = wrapbuffer_fd(vi_fd, SRC_WIDTH, SRC_HEIGHT, RK_FORMAT_YCbCr_420_SP, SRC_WIDTH, SRC_HEIGHT);
            // STEP 2/3: 循环遍历 4x4 网格，对每个 tile 进行裁剪+编码
            for (int r = 0; r < SPLIT_ROW; r++) {
                for (int c = 0; c < SPLIT_COL; c++) {
                    int chnId = r * SPLIT_COL + c;
                    // PTS 应沿用 VI 帧，写入到 stVencFrame 时传递
                    ProcessSingleTile(r,
                                      c,
                                      chnId,
                                      ctx,
                                      subImgPool,
                                      src_img,
                                      framePts);
                }
            }

            if(isIdrFrame){
                printf("[FRAME] seq=%u mod15=%u isI=%d\n",
                        (unsigned int)frameSeq,
                        (unsigned int)(frameSeq % 15),
                        isIdrFrame ? 1 : 0);
            }
            
            
            if (ctx.demo) rtsp_do_event(ctx.demo);
            RK_MPI_VI_ReleaseChnFrame(0, 0, &viFrame);
        }
    }

    free(stream.pstPack);
}

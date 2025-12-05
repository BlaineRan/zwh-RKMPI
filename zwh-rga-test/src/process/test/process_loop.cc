// 采集-裁剪-编码-推流核心循环实现
// - 从 VI 通道抓取一帧 1080P NV12 原始图
// - 使用 RGA 将大画面按 4x4 网格裁剪成 16 个子画面
// - 逐路送入对应 VENC 编码，再推送到各自的 RTSP 会话
#include "process_loop.h"

#include <stdlib.h>
#include <string.h>

#include "im2d.h"
#include "rga.h"

void ProcessFrames(const RtspContext &ctx, MB_POOL subImgPool) {
    VIDEO_FRAME_INFO_S stViFrame;
    VENC_STREAM_S stStream;
    // 仅分配一次码流包描述，内部的物理缓冲由 VENC 管理
    stStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

    while(1) {
        // 从 VI 拉取一帧图像，超时 1000ms
        RK_S32 s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, 1000);
        if(s32Ret == RK_SUCCESS) {
            int vi_fd = RK_MPI_MB_Handle2Fd(stViFrame.stVFrame.pMbBlk);

            rga_buffer_t src_img;
            src_img = wrapbuffer_fd(vi_fd, SRC_WIDTH, SRC_HEIGHT, RK_FORMAT_YCbCr_420_SP, SRC_WIDTH, SRC_HEIGHT);

            // 内层双重循环：按网格计算每个子画面的裁剪区域，RGA 裁剪后送编码
            for (int r = 0; r < SPLIT_ROW; r++) {
                for (int c = 0; c < SPLIT_COL; c++) {
                    int chnId = r * SPLIT_COL + c;
                    
                    // 从子画面池中取一块 NV12 物理缓冲，供 RGA 输出
                    MB_BLK dst_Blk = RK_MPI_MB_GetMB(subImgPool, SUB_WIDTH * SUB_HEIGHT * 3 / 2, RK_TRUE);
                    int dst_fd = RK_MPI_MB_Handle2Fd(dst_Blk);

                    rga_buffer_t dst_img;
                    dst_img = wrapbuffer_fd(dst_fd, SUB_WIDTH, SUB_HEIGHT, RK_FORMAT_YCbCr_420_SP, SUB_WIDTH, SUB_HEIGHT);

                    // 计算当前子画面的裁剪矩形
                    im_rect src_rect;
                    src_rect.x = c * SUB_WIDTH;
                    src_rect.y = r * SUB_HEIGHT;
                    src_rect.width = SUB_WIDTH;
                    src_rect.height = SUB_HEIGHT;

                    // RGA 执行裁剪，将大图指定区域拷贝到子画面缓冲
                    if (imcheck(src_img, dst_img, src_rect, {}) == IM_STATUS_NOERROR) {
                        imcrop(src_img, dst_img, src_rect);
                    }

                    VIDEO_FRAME_INFO_S stVencFrame;
                    memset(&stVencFrame, 0, sizeof(VIDEO_FRAME_INFO_S));
                    stVencFrame.stVFrame.u32Width = SUB_WIDTH;
                    stVencFrame.stVFrame.u32Height = SUB_HEIGHT;
                    stVencFrame.stVFrame.u32VirWidth = SUB_WIDTH;
                    stVencFrame.stVFrame.u32VirHeight = SUB_HEIGHT;
                    stVencFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP; 
                    stVencFrame.stVFrame.pMbBlk = dst_Blk; 
                    stVencFrame.stVFrame.u64PTS = stViFrame.stVFrame.u64PTS;

                    // 将裁剪后的子画面送入对应编码通道
                    RK_MPI_VENC_SendFrame(chnId, &stVencFrame, -1);
                    RK_MPI_MB_ReleaseMB(dst_Blk);

                    // 取出编码码流并推送到 RTSP
                    if (RK_MPI_VENC_GetStream(chnId, &stStream, 0) == RK_SUCCESS) {
                        void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
                        if (ctx.demo && ctx.sessions[chnId]) {
                            rtsp_tx_video(ctx.sessions[chnId], (uint8_t *)pData, stStream.pstPack->u32Len,
                                          stStream.pstPack->u64PTS);
                        }
                        RK_MPI_VENC_ReleaseStream(chnId, &stStream);
                    }
                }
            }
            
            if (ctx.demo) rtsp_do_event(ctx.demo);
            RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
        }
    }

    free(stStream.pstPack);
}

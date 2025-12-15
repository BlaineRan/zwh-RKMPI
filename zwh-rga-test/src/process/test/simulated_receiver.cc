#include "simulated_receiver.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sys/time.h>

#include "utils/luckfox_mpi.h"

static uint64_t GetMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

// 按 tile 位置将 NV12 数据拼成 1080P 大画幅，编码后统一推一路 /live/0
// 采用后台线程执行合成/编码，避免阻塞发送 tile 的调用线程
class SimulatedReceiverDevice {
public:
    bool Init(const RtspContext *ctx) {
        ctx_ = ctx;
        if (!EnsureInited()) return false;
        worker_ = std::thread(&SimulatedReceiverDevice::MergeLoop, this);
        return true;
    }

    void ReceiveTile(int tileId,
                     uint16_t frameSeq,
                     uint16_t mask,
                     uint64_t pts,
                     const void *data,
                     size_t size) {
        if (!ctx_ || ctx_->sessions.empty() || !ctx_->sessions[0]) return;
        if ((mask & (1 << tileId)) == 0) return; // 根据 tileMask 跳过
        if (!inited_) return;

        std::unique_lock<std::mutex> lk(mtx_);
        if (frameSeq != frame_.seq) {
            // 新帧到来，将上一帧（即便缺块）标记待合成
            if (frame_.expectedMask) frame_.dirty = true;
            ResetFrameLocked(frameSeq, mask, pts);
        }

        if (frame_.expectedMask == 0) return; // 全部被跳过

        frame_.tiles[tileId].data.assign(static_cast<const uint8_t *>(data),
                                         static_cast<const uint8_t *>(data) + size);
        frame_.receivedMask |= (1 << tileId);
        frame_.dirty = true;
        frame_.lastUpdateMs = GetMs();
        cv_.notify_one();
    }

private:
    static constexpr int kMergedChnId = TOTAL_CHNS; // 使用未占用的通道 ID

    struct TileBuffer {
        std::vector<uint8_t> data;
    };

    struct FrameState {
        uint16_t seq = 0;
        uint16_t expectedMask = 0;
        uint16_t receivedMask = 0;
        uint64_t pts = 0;
        bool dirty = false;
        uint64_t lastUpdateMs = 0;
        std::vector<TileBuffer> tiles;
    };

    bool EnsureInited() {
        if (inited_) return true;
        if (!InitMergedVenc()) return false;
        if (!CreateCanvasPool()) return false;
        mergedStream_.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
        memset(mergedStream_.pstPack, 0, sizeof(VENC_PACK_S));
        frame_.tiles.assign(TOTAL_CHNS, TileBuffer());
        inited_ = true;
        return true;
    }

    bool InitMergedVenc() {
        VENC_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(VENC_CHN_ATTR_S));
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

    bool CreateCanvasPool() {
        MB_POOL_CONFIG_S cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.u64MBSize = SRC_WIDTH * SRC_HEIGHT * 3 / 2;
        cfg.u32MBCnt = 2;
        cfg.enAllocType = MB_ALLOC_TYPE_DMA;
        canvasPool_ = RK_MPI_MB_CreatePool(&cfg);
        if (canvasPool_ == MB_INVALID_POOLID) {
            printf("Create canvas pool failed.\n");
            return false;
        }
        return true;
    }

    void ResetFrameLocked(uint16_t seq, uint16_t mask, uint64_t pts) {
        frame_.seq = seq;
        frame_.expectedMask = mask;
        frame_.receivedMask = 0;
        frame_.pts = pts;
        frame_.dirty = false;
        frame_.lastUpdateMs = GetMs();
        for (auto &tile : frame_.tiles) tile.data.clear();
    }

    void MergeLoop() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (true) {
            cv_.wait_for(lk, std::chrono::milliseconds(flushIntervalMs_), [&]{ return frame_.dirty; });
            uint64_t now = GetMs();
            bool shouldFlush = frame_.dirty &&
                               ((frame_.receivedMask & frame_.expectedMask) == frame_.expectedMask ||
                                (frame_.expectedMask && now - frame_.lastUpdateMs >= flushIntervalMs_));
            if (!shouldFlush) continue;
            FrameState local = frame_; // 拷贝元信息
            frame_.dirty = false;
            lk.unlock();
            EncodeAndSend(local);
            lk.lock();
        }
    }

    void EncodeAndSend(const FrameState &state) {
        if (!ctx_ || !ctx_->sessions[0]) return;
        if (canvasPool_ == MB_INVALID_POOLID) return;

        MB_BLK canvasBlk = RK_MPI_MB_GetMB(canvasPool_, SRC_WIDTH * SRC_HEIGHT * 3 / 2, RK_TRUE);
        void *canvasVir = RK_MPI_MB_Handle2VirAddr(canvasBlk);
        memset(canvasVir, 0, SRC_WIDTH * SRC_HEIGHT * 3 / 2);

        for (int i = 0; i < TOTAL_CHNS; ++i) {
            if (state.expectedMask & (1 << i)) {
                BlitTile(i, state.tiles[i].data, canvasVir);
            }
        }

        RK_MPI_SYS_MmzFlushCache(canvasBlk, RK_TRUE);

        VIDEO_FRAME_INFO_S vencFrame;
        memset(&vencFrame, 0, sizeof(vencFrame));
        vencFrame.stVFrame.u32Width = SRC_WIDTH;
        vencFrame.stVFrame.u32Height = SRC_HEIGHT;
        vencFrame.stVFrame.u32VirWidth = SRC_WIDTH;
        vencFrame.stVFrame.u32VirHeight = SRC_HEIGHT;
        vencFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        vencFrame.stVFrame.pMbBlk = canvasBlk;
        vencFrame.stVFrame.u64PTS = state.pts;

        RK_MPI_VENC_SendFrame(kMergedChnId, &vencFrame, -1);
        RK_MPI_MB_ReleaseMB(canvasBlk);

        if (RK_MPI_VENC_GetStream(kMergedChnId, &mergedStream_, 0) == RK_SUCCESS) {
            void *pData = RK_MPI_MB_Handle2VirAddr(mergedStream_.pstPack->pMbBlk);
            rtsp_tx_video(ctx_->sessions[0],
                          static_cast<uint8_t *>(pData),
                          mergedStream_.pstPack->u32Len,
                          mergedStream_.pstPack->u64PTS);
            RK_MPI_VENC_ReleaseStream(kMergedChnId, &mergedStream_);
            if (ctx_ && ctx_->demo) rtsp_do_event(ctx_->demo);
        }
    }

    void BlitTile(int tileId, const std::vector<uint8_t> &tile, void *canvasVir) {
        if (tile.size() < SUB_WIDTH * SUB_HEIGHT * 3 / 2) return;
        int r = tileId / SPLIT_COL;
        int c = tileId % SPLIT_COL;
        int x = c * SUB_WIDTH;
        int y = r * SUB_HEIGHT;
        const uint8_t *src = tile.data();
        uint8_t *dst = static_cast<uint8_t *>(canvasVir);
        int dstStride = SRC_WIDTH;

        // Y 平面
        for (int row = 0; row < SUB_HEIGHT; ++row) {
            memcpy(dst + (y + row) * dstStride + x, src + row * SUB_WIDTH, SUB_WIDTH);
        }

        // UV 平面
        const uint8_t *srcUV = src + SUB_WIDTH * SUB_HEIGHT;
        uint8_t *dstUV = dst + SRC_WIDTH * SRC_HEIGHT;
        int uvY = y / 2;
        for (int row = 0; row < SUB_HEIGHT / 2; ++row) {
            memcpy(dstUV + (uvY + row) * dstStride + x, srcUV + row * SUB_WIDTH, SUB_WIDTH);
        }
    }

    const RtspContext *ctx_ = nullptr;
    FrameState frame_;
    bool inited_ = false;
    const uint64_t flushIntervalMs_ = 30;
    MB_POOL canvasPool_ = MB_INVALID_POOLID;
    VENC_STREAM_S mergedStream_;
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

static SimulatedReceiverDevice gReceiver;
static bool gReady = false;

bool InitSimulatedReceiver(const RtspContext *ctx) {
    gReady = gReceiver.Init(ctx);
    if (gReady) {
        printf("SimulatedReceiver inited, merged stream -> /live/0\n");
    } else {
        printf("SimulatedReceiver init failed, keep 16-way streaming.\n");
    }
    return gReady;
}

bool SimulatedReceiverReady() {
    return gReady;
}

void SendTileToSimulatedReceiver(int tileId,
                                 uint16_t frameSeq,
                                 uint16_t tileMask,
                                 uint64_t pts,
                                 const void *data,
                                 size_t size) {
    if (!gReady) return;
    gReceiver.ReceiveTile(tileId, frameSeq, tileMask, pts, data, size);
}

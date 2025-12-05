#include "pipeline_init.h"

#include <stdio.h>
#include <string.h>

bool InitMpiSys() {
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        printf("rk mpi sys init fail!\n");
        return false;
    }
    return true;
}

bool StartIsp() {
    RK_BOOL multi_sensor = RK_FALSE;
    const char *iq_dir = "/etc/iqfiles"; // 板端需预置 IQ 文件目录
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    SAMPLE_COMM_ISP_Init(0, hdr_mode, multi_sensor, iq_dir);
    SAMPLE_COMM_ISP_Run(0);
    printf("ISP (AIQ) Run Success.\n");
    return true;
}

void StopIsp() {
    SAMPLE_COMM_ISP_Stop(0);
}

void InitViInput() {
    vi_dev_init();
    vi_chn_init(0, SRC_WIDTH, SRC_HEIGHT);
}

bool InitVencChannels() {
    for (int i = 0; i < TOTAL_CHNS; i++) {
        VENC_CHN_ATTR_S stVencChnAttr;
        memset(&stVencChnAttr, 0, sizeof(VENC_CHN_ATTR_S));
        
        stVencChnAttr.stVencAttr.enType = RK_VIDEO_ID_AVC;
        stVencChnAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        stVencChnAttr.stVencAttr.u32Profile = 66; 
        stVencChnAttr.stVencAttr.u32PicWidth = SUB_WIDTH;
        stVencChnAttr.stVencAttr.u32PicHeight = SUB_HEIGHT;
        stVencChnAttr.stVencAttr.u32VirWidth = SUB_WIDTH;
        stVencChnAttr.stVencAttr.u32VirHeight = SUB_HEIGHT;
        stVencChnAttr.stVencAttr.u32BufSize = SUB_WIDTH * SUB_HEIGHT * 2;
        
        stVencChnAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        stVencChnAttr.stRcAttr.stH264Cbr.u32Gop = 25;
        stVencChnAttr.stRcAttr.stH264Cbr.u32BitRate = 256; 
        stVencChnAttr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 25;
        stVencChnAttr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;

        RK_S32 s32Ret = RK_MPI_VENC_CreateChn(i, &stVencChnAttr);
        if (s32Ret != RK_SUCCESS) {
            printf("Create VENC Chn %d failed: 0x%x\n", i, s32Ret);
            return false;
        }
        
        VENC_RECV_PIC_PARAM_S stRecvParam;
        memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
        stRecvParam.s32RecvPicNum = -1;
        RK_MPI_VENC_StartRecvFrame(i, &stRecvParam);
    }
    
    printf("Init 16 VENC Channels Success.\n");
    return true;
}

bool CreateSubImgPool(MB_POOL &pool) {
    MB_POOL_CONFIG_S PoolCfg;
    memset(&PoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    PoolCfg.u64MBSize = SUB_WIDTH * SUB_HEIGHT * 3 / 2; // NV12 size
    PoolCfg.u32MBCnt = TOTAL_CHNS * 2; 
    PoolCfg.enAllocType = MB_ALLOC_TYPE_DMA; 
    pool = RK_MPI_MB_CreatePool(&PoolCfg);
    if (pool == MB_INVALID_POOLID) {
        printf("Create Pool Failed!\n");
        return false;
    }
    return true;
}

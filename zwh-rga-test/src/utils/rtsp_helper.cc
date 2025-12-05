#include "rtsp_helper.h"

#include <stdio.h>

bool InitRtsp(RtspContext &ctx) {
    ctx.demo = create_rtsp_demo(554); 
    if (!ctx.demo) {
        printf("Create RTSP demo failed\n");
        return false;
    }

    ctx.sessions.resize(TOTAL_CHNS);
    char rtsp_path[32];
    for (int i = 0; i < TOTAL_CHNS; i++) {
        sprintf(rtsp_path, "/live/%d", i); 
        ctx.sessions[i] = rtsp_new_session(ctx.demo, rtsp_path);
        
        rtsp_set_video(ctx.sessions[i], RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
        rtsp_sync_video_ts(ctx.sessions[i], rtsp_get_reltime(), rtsp_get_ntptime());
        printf("RTSP Session Created: rtsp://<IP>:554%s\n", rtsp_path);
    }
    return true;
}

void CleanupRtsp(RtspContext &ctx) {
    if (ctx.demo) {
        rtsp_del_demo(ctx.demo);
        ctx.demo = NULL;
    }
    ctx.sessions.clear();
}

#pragma once

#include "luckfox_mpi.h"
#include "config.h"

// 初始化 MPI 系统
bool InitMpiSys();

// 开启 ISP（3A）
bool StartIsp();
void StopIsp();

// 初始化 VI 输入
void InitViInput();

// 初始化 16 路编码器
bool InitVencChannels();

// 创建子画面内存池
bool CreateSubImgPool(MB_POOL &pool);

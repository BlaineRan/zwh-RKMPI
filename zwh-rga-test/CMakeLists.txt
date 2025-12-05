cmake_minimum_required(VERSION 3.10)
project(zwh-mpi-test)

# 1. 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)

# 2. 必须设置 SDK 路径 (用于寻找编译器)
# if(NOT DEFINED ENV{LUCKFOX_SDK_PATH})
#     message(FATAL_ERROR "Please export LUCKFOX_SDK_PATH=<Your SDK Path>")
# endif()
# set(SDK_PATH $ENV{LUCKFOX_SDK_PATH})
# 写死sdk路径
set(SDK_PATH "/home")
# 3. 指定编译器 (RV1106 uclibc)
set(CMAKE_C_COMPILER "rkgcc")
set(CMAKE_CXX_COMPILER "rkg++")

# 4. 定义编译选项 (从官方配置复制，确保硬件加速宏生效) 删除了-DROCKIVA
add_definitions(-DRV1106_1103 -DISP_HW_V30 -DRKPLATFORM=ON -DARCH64=OFF 
                 -DUAPI2 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64)

# =======================================================
# 关键路径定义 (因为你在子目录，所以要用 .. 访问根目录资源)
# =======================================================
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LIB_PATH "${REPO_ROOT}/lib/uclibc")       # 库文件位置
set(INC_PATH "${REPO_ROOT}/include")          # 头文件位置

# 5. 设置库搜索路径
link_directories(${LIB_PATH})

# 6. 配置 OpenCV (使用仓库自带的 uclibc 版本)
set(OpenCV_DIR "${LIB_PATH}/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

# 7. 添加源文件 (编译 src 及子目录下的所有 .cc/.cpp)
file(GLOB_RECURSE SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(${PROJECT_NAME} ${SRC_FILES})

# 8. 包含头文件路径 (复刻官方结构，注意都加了 REPO_ROOT)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${INC_PATH}                        # 基础 include
    ${INC_PATH}/rknn
    ${INC_PATH}/librga
    ${INC_PATH}/rkaiq
    ${INC_PATH}/rkaiq/uAPI2
    ${INC_PATH}/rkaiq/common
    ${INC_PATH}/rkaiq/xcore
    ${INC_PATH}/rkaiq/algos
    ${INC_PATH}/rkaiq/iq_parser
    ${INC_PATH}/rkaiq/iq_parser_v2
    ${INC_PATH}/rkaiq/smartIr
    ${REPO_ROOT}
    ${REPO_ROOT}/utils
    ${REPO_ROOT}/common 
    ${REPO_ROOT}/common/isp3.x 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 9. 链接库 (复刻官方 uclibc 的链接列表)
# 注意：sample_comm 是官方示例的公共库，确保它在 ../lib/uclibc 下存在
target_link_libraries(${PROJECT_NAME}
    ${OpenCV_LIBS}
    rknnmrt           # NPU 库
    Threads::Threads
    rockiva           # IVA 分析库
    sample_comm       # 官方示例通用库
    rockit
    rockchip_mpp      # 硬件编解码 MPP
    rkaiq             # ISP 图像质量调节
    pthread
    rtsp              # RTSP 推流库
    rga               # 2D 图形加速 RGA
)

# 10. 修复链接时的动态库路径问题
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${LIB_PATH}:/usr/lib")

message(STATUS "Build setup for uclibc on RV1106 complete.")

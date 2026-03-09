cmake_minimum_required(VERSION 3.16)
project(gst_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================
# 의존성 탐색
# ============================================

# GStreamer (pkg-config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST        REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP    REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_VIDEO  REQUIRED gstreamer-video-1.0)

# DeepX DX-RT — 환경변수 $DXRT_DIR 또는 자동 탐지
if(DEFINED ENV{DXRT_DIR})
    set(DXRT_BASE $ENV{DXRT_DIR})
    message(STATUS "DX-RT: 환경변수 DXRT_DIR 사용 → ${DXRT_BASE}")
else()
    # 자동 탐지: $HOME/dx-all-suite 하위 검색
    file(GLOB DXRT_CANDIDATES "$ENV{HOME}/dx-all-suite/dx-runtime/dx_rt/build_aarch64")
    if(DXRT_CANDIDATES)
        list(GET DXRT_CANDIDATES 0 DXRT_BASE)
        message(STATUS "DX-RT: 자동 탐지 → ${DXRT_BASE}")
    else()
        message(FATAL_ERROR
            "DX-RT SDK를 찾을 수 없습니다.\n"
            "  환경변수를 설정하세요: export DXRT_DIR=/path/to/dx_rt/build_aarch64\n"
            "  또는 ~/dx-all-suite/dx-runtime/dx_rt/build_aarch64 에 SDK를 설치하세요.")
    endif()
endif()

set(DXRT_INCLUDE_DIR ${DXRT_BASE}/include)
set(DXRT_LIB ${DXRT_BASE}/lib/libdxrt.so)

# ============================================
# 빌드 타겟
# ============================================
add_executable(gst_benchmark_pipeline main.cpp)

target_include_directories(gst_benchmark_pipeline PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${DXRT_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gst_benchmark_pipeline PRIVATE
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    ${DXRT_LIB}
    pthread
)

target_link_directories(gst_benchmark_pipeline PRIVATE
    ${GST_LIBRARY_DIRS}
    ${GST_APP_LIBRARY_DIRS}
    ${GST_VIDEO_LIBRARY_DIRS}
)

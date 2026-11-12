cmake_minimum_required(VERSION 3.16)

project(VitaTube C)

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK no esta configurado")
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# Reserva espacio en el segmento 0 para los metadatos de SCE inyectados por vita-elf-create
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym,__sce_headroom=0x8000")

set(VITA_APP_NAME "VitaTube")
set(VITA_TITLEID "VTTB00001")
set(VITA_VERSION "01.00")

add_executable(${PROJECT_NAME}
    src/main.c
    src/log.c
    src/app.c
    src/audio.c
    src/ui/home.c
    src/network/http.c
    src/network/cJSON.c
    src/network/stream_api.c
    src/youtube/youtube.c
    src/youtube/history.c
    src/player/video_player.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/ui
    src/network
    src/youtube
    src/player
    ${CMAKE_CURRENT_SOURCE_DIR}/build/vita-media/include
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/build/vita-media/lib
)

target_link_libraries(${PROJECT_NAME}
    vita2d
    mpv
    avfilter
    avformat
    avcodec
    swresample
    swscale
    avutil
    ass
    harfbuzz
    fribidi
    freetype
    archive
    bz2
    zstd
    png16
    z
    SceVideodec_stub
    SceAudiodec_stub
    SceCodecEngine_stub
    SceGxm_stub
    SceAppMgr_stub
    ScePgf_stub
    SceCommonDialog_stub
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    ScePower_stub
    SceSysmodule_stub
    SceIofilemgr_stub
    debugnet
    SceNet_stub
    SceHttp_stub
    SceSsl_stub
    SceLibKernel_stub
    SceProcessmgr_stub
    jpeg
    pthread
    m
)

set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${PROJECT_NAME}.vpk
    ${VITA_TITLEID}
    ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE assets/icon0.png sce_sys/icon0.png
    FILE assets/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE assets/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE assets/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

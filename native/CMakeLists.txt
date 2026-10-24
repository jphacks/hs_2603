cmake_minimum_required(VERSION 3.21)
project(KantanVideoEditor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

# FFmpeg（共有ライブラリ版）: include/ と lib/*.lib を使う
set(FFMPEG_DIR "" CACHE PATH "FFmpeg shared build root (include, lib, bin)")
if(NOT EXISTS "${FFMPEG_DIR}/include/libavformat/avformat.h")
  message(FATAL_ERROR "FFMPEG_DIR が正しくありません: ${FFMPEG_DIR}")
endif()
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/include")
foreach(lib avformat avcodec avutil swscale swresample avfilter)
  if(MSVC)
    target_link_libraries(ffmpeg INTERFACE "${FFMPEG_DIR}/lib/${lib}.lib")
  else()
    target_link_libraries(ffmpeg INTERFACE "${FFMPEG_DIR}/lib/lib${lib}.dll.a")
  endif()
endforeach()

add_library(miniaudio STATIC third_party/miniaudio/miniaudio.c)
target_include_directories(miniaudio PUBLIC third_party/miniaudio)
target_compile_definitions(miniaudio PRIVATE MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)

set(SOURCES
  src/main.cpp
  src/core/model.h src/core/model.cpp
  src/core/timeline.h src/core/timeline.cpp
  src/core/filters.h src/core/filters.cpp
  src/core/textstyles.h src/core/textstyles.cpp
  src/core/editor.h src/core/editor.cpp
  src/core/projectio.h src/core/projectio.cpp
  src/core/exportplan.h src/core/exportplan.cpp
  src/media/ffutil.h src/media/ffutil.cpp
  src/media/videosource.h src/media/videosource.cpp
  src/media/audioengine.h src/media/audioengine.cpp
  src/media/mediamanager.h src/media/mediamanager.cpp
  src/media/audiopreview.h src/media/audiopreview.cpp
  src/render/imageops.h src/render/imageops.cpp
  src/render/textrender.h src/render/textrender.cpp
  src/render/previewengine.h src/render/previewengine.cpp
  src/export/exporter.h src/export/exporter.cpp
  src/ui/theme.h src/ui/theme.cpp
  src/ui/widgets.h src/ui/widgets.cpp
  src/ui/librarypanel.h src/ui/librarypanel.cpp
  src/ui/previewwidget.h src/ui/previewwidget.cpp
  src/ui/timelinewidget.h src/ui/timelinewidget.cpp
  src/ui/animpanel.h src/ui/animpanel.cpp
  src/ui/inspector.h src/ui/inspector.cpp
  src/ui/exportdialog.h src/ui/exportdialog.cpp
  src/ui/workspacebar.h src/ui/workspacebar.cpp
  src/ai/aiconfig.h src/ai/aiconfig.cpp
  src/ai/bedrock.h src/ai/bedrock.cpp
  src/ai/framegrab.h src/ai/framegrab.cpp
  src/ai/tools.h src/ai/tools.cpp
  src/ai/assistant.h src/ai/assistant.cpp
  src/ai/servercall.h src/ai/servercall.cpp
  src/ai/account.h src/ai/account.cpp
  src/ui/accountdialog.h src/ui/accountdialog.cpp
  src/ui/serverstatus.h src/ui/serverstatus.cpp
  src/ui/aipanel.h src/ui/aipanel.cpp
  src/ui/mainwindow.h src/ui/mainwindow.cpp
  src/uitest.h src/uitest.cpp
)

qt_add_executable(kantan-video-editor WIN32 ${SOURCES} resources/app.qrc resources/app.rc)
target_include_directories(kantan-video-editor PRIVATE src)
target_compile_definitions(kantan-video-editor PRIVATE UNICODE _UNICODE NOMINMAX
  KV_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(kantan-video-editor PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus /MP)
else()
  target_compile_options(kantan-video-editor PRIVATE -Wall -Wno-unused-parameter)
endif()
target_link_libraries(kantan-video-editor PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network ffmpeg miniaudio ole32 shell32 uuid crypt32)

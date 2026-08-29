# 手机端 NDK 原生构建（ImGui + 可靠 UDP 发送）。
# Gradle 会以 ANDROID_NDK（或 ANDROID_NDK_HOME）定位 NDK 的 native_app_glue。
cmake_minimum_required(VERSION 3.18)
project(phone_udp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- NDK native_app_glue（NativeActivity 生命周期胶水）----
if(NOT DEFINED ANDROID_NDK AND DEFINED ENV{ANDROID_NDK_HOME})
  set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
endif()
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# ---- Dear ImGui（vendored：android/third_party/imgui）----
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/imgui)
set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../../../../common)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/imgui_demo.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${IMGUI}
    ${IMGUI}/backends)
target_compile_options(imgui PRIVATE -fPIC)

# ---- App（Android 上 imgui_impl_opengl3.cpp 会自动开启 GLES3）----
# reliable_udp 直接复用 common/（避免两端源码漂移）
add_library(phone_udp SHARED
    android_main.cpp
    ${IMGUI}/backends/imgui_impl_android.cpp
    ${COMMON}/reliable_udp.cpp)
target_include_directories(phone_udp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMMON}
    ${IMGUI}
    ${IMGUI}/backends)
target_link_libraries(phone_udp
    imgui
    native_app_glue
    android
    log
    EGL
    GLESv3)
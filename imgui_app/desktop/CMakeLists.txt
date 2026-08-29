cmake_minimum_required(VERSION 3.18)
project(TraeUdpUnicorn VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 共享核心 ----
set(SHARED_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/reliable_udp.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/emu.cpp)

# ---- Unicorn（可选）：-DUSE_UNICORN=ON -DUNICORN_ROOT=...
option(USE_UNICORN "使用 Unicorn 做模拟（默认关，需自行提供库）" OFF)
set(UNICORN_ROOT "" CACHE PATH "Unicorn 安装目录（含 include/ 与 lib/）")
if(USE_UNICORN AND NOT UNICORN_ROOT)
  message(FATAL_ERROR "请设置 -DUNICORN_ROOT=<uncorn目录> 或关闭 -DUSE_UNICORN=OFF")
endif()

# 原生 Win32 标准控件程序（去 ImGui/GLFW，无第三方 GUI 依赖）。
add_executable(desktop WIN32
  main.cpp
  ${SHARED_SRC})

target_include_directories(desktop PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common)

if(USE_UNICORN)
  target_compile_definitions(desktop PRIVATE USE_UNICORN)
  target_include_directories(desktop PRIVATE ${UNICORN_ROOT}/include)
  find_library(UNICORN_LIB unicorn PATHS ${UNICORN_ROOT}/lib NO_DEFAULT_PATH)
  if(NOT UNICORN_LIB)
    message(FATAL_ERROR "找不到 unicorn 库，请确认 ${UNICORN_ROOT}/lib 下有 unicorn.lib/.a")
  endif()
  target_link_libraries(desktop PRIVATE ${UNICORN_LIB})
endif()

# 平台链接
if(WIN32)
  target_link_libraries(desktop PRIVATE ws2_32 user32 gdi32 comctl32)
else()
  message(FATAL_ERROR "原生 Win32 程序仅支持 Windows，本机请用支持的目标构建。")
endif()
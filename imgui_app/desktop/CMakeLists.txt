cmake_minimum_required(VERSION 3.18)
project(TraeUdpUnicorn VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 依赖：Dear ImGui + GLFW（首次联网自动拉取；imgui 自带 OpenGL loader）----
include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.9)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4)
FetchContent_MakeAvailable(imgui glfw)

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

add_executable(desktop
  main.cpp
  ${SHARED_SRC}
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(desktop PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(desktop PRIVATE glfw)

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
  target_link_libraries(desktop PRIVATE opengl32 ws2_32)
elseif(UNIX AND NOT APPLE)
  target_link_libraries(desktop PRIVATE Threads::Threads)
  find_package(Threads REQUIRED)
  target_link_libraries(desktop PRIVATE ${CMAKE_DL_LIBS})
endif()
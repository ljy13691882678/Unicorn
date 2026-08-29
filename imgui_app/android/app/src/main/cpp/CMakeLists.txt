# 手机端 JNI 原生构建：Java 主界面 -> jni_bridge.cpp -> common 可靠 UDP。
cmake_minimum_required(VERSION 3.18)
project(phone_udp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# common 在仓库根的 imgui_app/common（本文件在 android/app/src/main/cpp，需上 5 层）
set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common)

# 内核驱动 TimeDriver 对接库（固化的 AArch64 静态库）
add_library(time_driver STATIC IMPORTED)
set_target_properties(time_driver PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/time_driver/time_driver.a)

add_library(phone_udp SHARED
    jni_bridge.cpp
    ${COMMON}/reliable_udp.cpp)
target_include_directories(phone_udp PRIVATE
    ${COMMON}
    ${CMAKE_CURRENT_SOURCE_DIR}/time_driver)
target_link_libraries(phone_udp time_driver)

find_library(ANDROID_LOG log)
target_link_libraries(phone_udp ${ANDROID_LOG})
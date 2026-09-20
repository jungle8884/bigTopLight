# XS-XLD5 大路灯控制板 ESP32-C3 项目
# 顶层 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固件版本单一来源: 写入镜像 esp_app_desc, 设备端通过
# esp_app_get_description()->version 读取并上报平台。
# 每次发新固件前只需改这一处。
set(PROJECT_VER "4.0")

project(lamp_controller)

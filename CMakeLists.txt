# XS-XLD5 大路灯控制板 ESP32-C3 项目
# 顶层 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(lamp_controller)

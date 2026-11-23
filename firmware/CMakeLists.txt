cmake_minimum_required(VERSION 3.22)
# Kode-watch firmware: the kodewatch app on the Waveshare ESP32-S3-Touch-AMOLED-2.06.
set(IDF_TARGET esp32s3)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "00.01.00")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kodewatch_s3)

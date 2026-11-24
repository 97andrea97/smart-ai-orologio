# Firmware dell'orologio di smart-AI (ESP-IDF). Si compila su GitHub Actions:
# .github/workflows/firmware.yml. Base: esempio 02_lvgl_demo_v9 di Waveshare.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format)
project(smart_ai_orologio)

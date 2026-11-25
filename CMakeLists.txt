# Firmware dell'orologio di smart-AI (ESP-IDF). Si compila su GitHub Actions:
# .github/workflows/firmware.yml. Base: esempio 02_lvgl_demo_v9 di Waveshare.
cmake_minimum_required(VERSION 3.16)

# La versione e' il commit da cui si compila (la mette firmware.yml): il PC la confronta con quella
# che vuole sull'orologio. Senza, ESP-IDF la ricava da git a modo suo.
if(DEFINED ENV{VERSIONE_FIRMWARE})
    set(PROJECT_VER "$ENV{VERSIONE_FIRMWARE}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format)
project(smart_ai_orologio)

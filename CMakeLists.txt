cmake_minimum_required(VERSION 3.16)
# Party Play (README). Test builds may pass -DPROJECT_VER to tell slot images apart.
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.0.7")
endif()
# The shared firmware directory: this app's own repository has the development kit as its devkit/ submodule (pinned
# at a version); the main repository's firmware/party-play (where Party Play lived up to 1.0.7) has it one level up.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/devkit/firmware/components")
    set(YAYKIT_FIRMWARE "${CMAKE_CURRENT_LIST_DIR}/devkit/firmware")
else()
    set(YAYKIT_FIRMWARE "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
# LVGL and the other registry components are locked in dependencies.lock. No Bluetooth: pairing belongs to the main
# firmware. yaykit_link is here only because yaykit_app reads the slots through it; Party Play never starts it.
# yaykit_voicecode: the system pull-down's 「在手机上打开」 shows the app's 应用码 (app.json voiceCode, kit 1.9).
set(EXTRA_COMPONENT_DIRS
    "${YAYKIT_FIRMWARE}/components/yaykit_app"
    "${YAYKIT_FIRMWARE}/components/yaykit_board"
    "${YAYKIT_FIRMWARE}/components/yaykit_key"
    "${YAYKIT_FIRMWARE}/components/yaykit_link"
    "${YAYKIT_FIRMWARE}/components/yaykit_motion"
    "${YAYKIT_FIRMWARE}/components/yaykit_speak"
    "${YAYKIT_FIRMWARE}/components/yaykit_ui"
    "${YAYKIT_FIRMWARE}/components/yaykit_voicecode")
set(COMPONENTS main espcoredump)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${YAYKIT_FIRMWARE}/reproducible.cmake)
project(yaykit_party_play)

idf_component_register(
    SRCS "main.c" "pp_ui.c" "pp_sound.c" "pp_synth.c"
         "pp_play.c" "pp_seats.c" "pp_dice.c" "pp_wheel.c" "pp_react.c" "pp_bomb.c" "pp_ten.c"
         "pp_record.c" "pp_launch.c" "pp_gesture.c" "pp_motion_map.c"
    INCLUDE_DIRS "."
    REQUIRES yaykit_board lvgl__lvgl yaykit_app yaykit_key yaykit_speak yaykit_motion yaykit_ui yaykit_link yaykit_voicecode json
    PRIV_REQUIRES esp_timer console nvs_flash esp_hw_support esp_psram spi_flash mbedtls)
# The shared Chinese UI font (OFL), from yaykit_board, wherever the shared components are (devkit/ or the main
# repository).
idf_component_get_property(board_dir yaykit_board COMPONENT_DIR)
esptool_py_flash_to_partition(flash "fonts" "${board_dir}/fonts/cjk/yaykit_fonts.bin")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PARTY_VERSION="${PROJECT_VER}")
if(CONFIG_PARTITION_TABLE_CUSTOM_FILENAME MATCHES "yaykit-v1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PARTY_LAYOUT="yaykit-v1")
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PARTY_LAYOUT="factory-v0")
endif()

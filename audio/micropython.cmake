# Create an INTERFACE library for our C module.
add_library(usermod_audio INTERFACE)

# Add our source files to the lib
target_sources(usermod_audio INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/audio_player.c
    ${CMAKE_CURRENT_LIST_DIR}/audio_recorder.c
    ${CMAKE_CURRENT_LIST_DIR}/modaudio.c
    ${CMAKE_CURRENT_LIST_DIR}/vfs_stream.c
)

# Add the current directory as an include directory.
target_include_directories(usermod_audio INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    /root/esp-adf/components/esp-adf-libs/esp_audio/include
    /root/esp-adf/components/esp-adf-libs/esp_codec/include/codec
    /root/esp-adf/components/esp-adf-libs/esp_codec/include/processing
    /root/esp-adf/components/audio_hal/include
    /root/esp-adf/components/audio_pipeline/include
    /root/esp-adf/components/audio_stream/include
    /root/esp-adf/components/audio_sal/include
    /root/esp-adf/components/audio_board/include
    /root/esp-adf/components/audio_board/lyrat_v4_3
    /root/esp-adf/components/esp_peripherals/include
    /root/esp-adf/components/display_service/include
    /root/esp-adf/components/esp_dispatcher/include
)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_audio)
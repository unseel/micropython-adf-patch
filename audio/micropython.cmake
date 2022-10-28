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
    $ENV{ADF_PATH}/components/esp-adf-libs/esp_audio/include
    $ENV{ADF_PATH}/components/esp-adf-libs/esp_codec/include/codec
    $ENV{ADF_PATH}/components/esp-adf-libs/esp_codec/include/processing
    $ENV{ADF_PATH}/components/audio_hal/include
    $ENV{ADF_PATH}/components/audio_pipeline/include
    $ENV{ADF_PATH}/components/audio_stream/include
    $ENV{ADF_PATH}/components/audio_sal/include
    $ENV{ADF_PATH}/components/audio_board/include
    $ENV{ADF_PATH}/components/audio_board/lyrat_v4_3
    $ENV{ADF_PATH}/components/esp_peripherals/include
    $ENV{ADF_PATH}/components/display_service/include
    $ENV{ADF_PATH}/components/esp_dispatcher/include
)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_audio)
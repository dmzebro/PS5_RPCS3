# The libretro core (BUILD_LIBRETRO): rpcs3_emu driven by a libretro frontend
# instead of the Qt application. The output is rpcs3_libretro.so, loaded by
# RetroArch at run time.
add_library(rpcs3_libretro SHARED
    libretro.cpp
    libretro_pad_handler.cpp
    libretro_audio_backend.cpp
    libretro_loader.cpp
    libretro_state.cpp
    libretro_options.cpp
    libretro_patches.cpp
    libretro_game_configs.cpp
    ../rpcs3_version.cpp
    # RPCS3's pad thread and what it needs from the Qt application's input
    # layer; the controllers are RetroArch's (libretro_pad_handler.h).
    ../Input/mouse_gyro_handler.cpp
    ../Input/pad_thread.cpp
    ../Input/product_info.cpp
    ../Input/ps_move_config.cpp
    ../Input/ps_move_tracker.cpp
)
set_target_properties(rpcs3_libretro PROPERTIES PREFIX "" OUTPUT_NAME rpcs3_libretro)
target_include_directories(rpcs3_libretro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/rpcs3)
target_link_libraries(rpcs3_libretro
    PRIVATE
        rpcs3_emu
        3rdparty::stblib
        3rdparty::libusb
        3rdparty::wolfssl
        3rdparty::libcurl
        3rdparty::zlib
        3rdparty::fusion
        3rdparty::yaml-cpp)

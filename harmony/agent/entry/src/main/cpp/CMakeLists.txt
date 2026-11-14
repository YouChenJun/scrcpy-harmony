cmake_minimum_required(VERSION 3.16)
project(scrcpy_agent)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(HARMONY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_library(scrcpy_agent SHARED agent.cpp "${HARMONY_ROOT}/server.cpp")
set_source_files_properties("${HARMONY_ROOT}/server.cpp" PROPERTIES COMPILE_DEFINITIONS "main=scrcpy_harmony_server_main")
target_link_libraries(scrcpy_agent PUBLIC libace_napi.z.so libohcamera.so libnative_avscreen_capture.so
    libnative_media_venc.so libnative_media_core.so libnative_media_codecbase.so libnative_media_acodec.so
    libpasteboard.so libudmf.so libnative_window.so libnative_display_manager.so libohaudio.so)

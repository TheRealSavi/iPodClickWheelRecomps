cmake_minimum_required(VERSION 3.21)
project(ipod_video_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
find_package(SDL3 3.2 CONFIG REQUIRED)
add_executable(video_test video_test.cpp ../src/ipod/platform/sdl3/presenter.cpp
    ../src/ipod/platform/sdl3/nis.cpp ../src/ipod/platform/sdl3/rtx_video.cpp)
target_include_directories(video_test PRIVATE ../src)
target_link_libraries(video_test PRIVATE SDL3::SDL3)
include(../cmake/RtxVideo.cmake)
target_link_libraries(video_test PRIVATE ipod_rtx_video)
ipod_copy_rtx_video_runtime(video_test)
if(MSVC)
    target_compile_options(video_test PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(video_test PRIVATE -fno-exceptions -fno-rtti)
endif()
if(WIN32)
    add_custom_command(TARGET video_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:video_test>)
endif()
add_test(NAME video_presentation COMMAND video_test)
if(WIN32)
    add_test(NAME video_nis_d3d11 COMMAND video_test direct3d11)
    if(RTX_VIDEO_SDK)
        add_test(NAME video_rtx_d3d11 COMMAND video_test direct3d11 rtx)
    endif()
endif()

# Compile every desktop integration without requiring proprietary game files or
# linking a complete game. This catches drift among the six platform adapters.
foreach(title "Vortex" "Cubis2" "HoldEm" "Lost" "Sims Bowling" "Mini Golf")
    string(REPLACE " " "_" target "${title}")
    add_library(${target}_video_compile OBJECT
        "../../${title}/src/platform/sdl3/sdl3_platform.cpp"
        "../../${title}/src/platform/settings.cpp")
    target_include_directories(${target}_video_compile PRIVATE "../../${title}/src" ../src)
    target_link_libraries(${target}_video_compile PRIVATE SDL3::SDL3)
endforeach()

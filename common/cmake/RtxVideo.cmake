# Optional external SDK. No NVIDIA proprietary SDK files are vendored.
include_guard(GLOBAL)
set(RTX_VIDEO_SDK "" CACHE PATH "NVIDIA RTX Video SDK root (Windows MSVC only)")
add_library(ipod_rtx_video INTERFACE)
if(RTX_VIDEO_SDK)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "RTX Video SDK integration requires Windows x64 MSVC")
    endif()
    set(_rtx_lib "${RTX_VIDEO_SDK}/lib/Windows/x64")
    set(_rtx_dll "${RTX_VIDEO_SDK}/bin/Windows/x64/rel/nvngx_vsr.dll")
    foreach(_rtx_file
            "${RTX_VIDEO_SDK}/include/nvsdk_ngx_helpers_vsr.h"
            "${RTX_VIDEO_SDK}/NVIDIA_RTX_Video_SDK_License.pdf"
            "${_rtx_lib}/nvsdk_ngx_d.lib" "${_rtx_lib}/nvsdk_ngx_d_dbg.lib"
            "${_rtx_dll}")
        if(NOT EXISTS "${_rtx_file}")
            message(FATAL_ERROR "Missing RTX Video SDK file: ${_rtx_file}")
        endif()
    endforeach()
    target_compile_definitions(ipod_rtx_video INTERFACE IPOD_HAS_RTX_VIDEO=1)
    target_include_directories(ipod_rtx_video SYSTEM INTERFACE "${RTX_VIDEO_SDK}/include")
    target_link_libraries(ipod_rtx_video INTERFACE
        advapi32
        "$<$<CONFIG:Debug>:${_rtx_lib}/nvsdk_ngx_d_dbg.lib>"
        "$<$<NOT:$<CONFIG:Debug>>:${_rtx_lib}/nvsdk_ngx_d.lib>")
endif()

# Shared by every desktop title and the standalone presenter tests.
function(ipod_copy_rtx_video_runtime target)
    if(RTX_VIDEO_SDK)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RTX_VIDEO_SDK}/bin/Windows/x64/rel/nvngx_vsr.dll"
                "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RTX_VIDEO_SDK}/NVIDIA_RTX_Video_SDK_License.pdf"
                "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

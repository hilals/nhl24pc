cmake_minimum_required(VERSION 3.20)
project(bbport_dlss LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DLSS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../externals/dlss_sdk" CACHE PATH "DLSS SDK root")
set(VULKAN_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../externals/vulkan-headers/include" CACHE PATH "Vulkan include dir")

add_library(bbport_dlss SHARED bbport_dlss_bridge.cpp bbport_dlss_bridge.h)

target_include_directories(bbport_dlss PRIVATE
    "${DLSS_SDK_ROOT}/include"
    "${VULKAN_INCLUDE}"
)

if (WIN32)
    target_link_libraries(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    target_compile_definitions(bbport_dlss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(bbport_dlss PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

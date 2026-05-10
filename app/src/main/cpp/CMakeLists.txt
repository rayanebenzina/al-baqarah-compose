cmake_minimum_required(VERSION 3.22.1)
project(baqarah_vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(baqarah_vk SHARED
    vk_renderer.cpp
    jni_bridge.cpp
)

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)

target_compile_definitions(baqarah_vk PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR=1
)

target_compile_options(baqarah_vk PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-variable
)

target_link_libraries(baqarah_vk
    ${vulkan-lib}
    ${android-lib}
    ${log-lib}
)

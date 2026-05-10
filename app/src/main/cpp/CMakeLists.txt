cmake_minimum_required(VERSION 3.22.1)
project(baqarah_vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLSLC "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
if(NOT EXISTS "${GLSLC}")
    message(FATAL_ERROR "glslc not found at ${GLSLC}")
endif()

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADER_HEADERS "")
function(add_shader STAGE SRC SYM)
    set(src_path ${SHADER_SRC_DIR}/${SRC})
    set(spv_path ${SHADER_OUT_DIR}/${SRC}.spv)
    set(hdr_path ${SHADER_OUT_DIR}/${SRC}.h)
    add_custom_command(
        OUTPUT ${hdr_path}
        COMMAND ${GLSLC} -fshader-stage=${STAGE} -O ${src_path} -o ${spv_path}
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${spv_path}
            -DDST=${hdr_path}
            -DSYM=${SYM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        DEPENDS ${src_path} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        COMMENT "glslc ${SRC} -> ${SYM}"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} ${hdr_path} PARENT_SCOPE)
endfunction()

add_shader(vert textured.vert kTexturedVertSpv)
add_shader(frag textured.frag kTexturedFragSpv)
add_shader(vert outline.vert kOutlineVertSpv)
add_shader(frag outline.frag kOutlineFragSpv)

add_library(baqarah_vk SHARED
    vk_renderer.cpp
    stb_truetype_impl.cpp
    glyph_extractor.cpp
    jni_bridge.cpp
    ${SHADER_HEADERS}
)

target_include_directories(baqarah_vk PRIVATE ${SHADER_OUT_DIR})

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

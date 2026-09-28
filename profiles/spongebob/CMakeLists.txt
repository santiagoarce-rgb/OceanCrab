# SpongeBob's Truth or Square (ULUS-10478) profile.
#
# Linux/Vulkan build. The renderer, SDL platform layer and SAS audio are adapted
# from elmasas/lcs-recomp (MIT). Opción A: no FFmpeg — sceAtrac3plus is stubbed.

set(SPONGEBOB_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Explicit source list (no GLOB) so we control exactly what compiles and keep
# GTA-specific files out of the build by default.
set(SPONGEBOB_HOST_SOURCES
    host/main.cpp
    host/spongebob_profile.cpp
    host/spongebob_profile.hpp
    host/ge_gpu_backend_vulkan.cpp
    host/ge_renderer.cpp
    host/display_window_sdl.cpp
    host/spongebob_render_config.cpp
    host/spongebob_runtime_log.cpp
    host/spongebob_controls.cpp        # GTA camera/vehicle input — clean up in Fase 5
    host/spongebob_fps_overlay.cpp
    host/spongebob_frame_limit.cpp
    host/spongebob_audio_output.cpp
    host/spongebob_sas.cpp
)

add_executable(SpongeBobNative ${SPONGEBOB_HOST_SOURCES})
target_include_directories(SpongeBobNative PRIVATE host)
target_link_libraries(SpongeBobNative PRIVATE psprecomp_core)

if(MSVC)
    target_compile_options(SpongeBobNative PRIVATE /W4 /permissive- /EHsc)
    target_link_options(SpongeBobNative PRIVATE /STACK:67108864)
endif()

if(WIN32)
    # Windows/DX12 path is not in scope for the Linux build. When needed, mirror
    # the lcs-recomp Windows branch (d3d12 dxgi d3dcompiler dxguid + FFmpeg).
    message(WARNING "SpongeBob profile currently targets Linux/Vulkan; Windows build not wired.")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
    find_package(Threads REQUIRED)

    # Enable the Vulkan GE backend (ge_gpu_backend_vulkan.cpp is guarded by this).
    target_compile_definitions(SpongeBobNative PRIVATE SPONGEBOB_VULKAN_GE_BACKEND=1)
    target_link_libraries(SpongeBobNative PRIVATE
        PkgConfig::SDL2 PkgConfig::VULKAN Threads::Threads)

    # Compile ge.vert/ge.frag to SPIR-V with glslangValidator and embed them as
    # a generated header (ge_spv.hpp) that ge_gpu_backend_vulkan.cpp includes.
    find_program(SPONGEBOB_GLSLANG NAMES glslangValidator glslang REQUIRED)
    set(SPONGEBOB_GE_SPV_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan/ge_spv.hpp)
    add_custom_command(
        OUTPUT ${SPONGEBOB_GE_SPV_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DGLSLANG=${SPONGEBOB_GLSLANG}
            -DVERT=${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.vert
            -DFRAG=${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.frag
            -DOUT=${SPONGEBOB_GE_SPV_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/embed_spirv.cmake
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.vert
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.frag
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/embed_spirv.cmake
        COMMENT "Compiling Vulkan GE shaders"
        VERBATIM)
    add_custom_target(spongebob_ge_spirv DEPENDS ${SPONGEBOB_GE_SPV_HEADER})
    add_dependencies(SpongeBobNative spongebob_ge_spirv)
    target_include_directories(SpongeBobNative BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/host/ge_gpu_backend_vulkan.cpp
        PROPERTIES OBJECT_DEPENDS ${SPONGEBOB_GE_SPV_HEADER})
endif()

# Generated AOT corpus (empty until Fase 5 runs the code generator).
file(GLOB SPONGEBOB_GENERATED CONFIGURE_DEPENDS generated/*.cpp)
if(SPONGEBOB_GENERATED)
    add_library(spongebob_generated STATIC ${SPONGEBOB_GENERATED})
    target_include_directories(spongebob_generated PRIVATE host)
    target_link_libraries(spongebob_generated PUBLIC psprecomp_core)
    target_compile_options(spongebob_generated PRIVATE -O2)
    target_link_libraries(SpongeBobNative PRIVATE spongebob_generated)
else()
    target_compile_definitions(SpongeBobNative PRIVATE SPONGEBOB_NO_GENERATED)
endif()

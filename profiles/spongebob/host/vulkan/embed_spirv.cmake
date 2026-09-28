# Compile ge.vert and ge.frag to SPIR-V and write vulkan/ge_spv.hpp.
if(NOT GLSLANG OR NOT VERT OR NOT FRAG OR NOT OUT)
    message(FATAL_ERROR "embed_spirv.cmake requires GLSLANG, VERT, FRAG, and OUT")
endif()

get_filename_component(_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")

function(spongebob_compile_spirv source output)
    execute_process(
        COMMAND "${GLSLANG}" -V --target-env vulkan1.0 -o "${output}" "${source}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE command_out
        ERROR_VARIABLE command_err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "glslangValidator failed for ${source} (exit ${rc})\n${command_out}${command_err}")
    endif()
endfunction()

function(spongebob_spirv_words path result)
    file(READ "${path}" hex HEX)
    string(LENGTH "${hex}" hex_len)
    math(EXPR hex_remainder "${hex_len} % 8")
    if(hex_len EQUAL 0 OR NOT hex_remainder EQUAL 0)
        message(FATAL_ERROR "${path} is not a sequence of SPIR-V words")
    endif()
    math(EXPR word_count "${hex_len} / 8")
    math(EXPR last_word "${word_count} - 1")
    set(lines "")
    set(row "")
    set(column 0)
    foreach(index RANGE ${last_word})
        math(EXPR byte_at "${index} * 8")
        string(SUBSTRING "${hex}" ${byte_at} 2 b0)
        math(EXPR byte_at "${byte_at} + 2")
        string(SUBSTRING "${hex}" ${byte_at} 2 b1)
        math(EXPR byte_at "${byte_at} + 2")
        string(SUBSTRING "${hex}" ${byte_at} 2 b2)
        math(EXPR byte_at "${byte_at} + 2")
        string(SUBSTRING "${hex}" ${byte_at} 2 b3)
        string(APPEND row "    0x${b3}${b2}${b1}${b0}u,")
        math(EXPR column "${column} + 1")
        if(column EQUAL 8)
            string(APPEND lines "${row}\n")
            set(row "")
            set(column 0)
        endif()
    endforeach()
    if(NOT row STREQUAL "")
        string(APPEND lines "${row}\n")
    endif()
    set(${result} "${lines}" PARENT_SCOPE)
endfunction()

spongebob_compile_spirv("${VERT}" "${_dir}/ge.vert.spv")
spongebob_compile_spirv("${FRAG}" "${_dir}/ge.frag.spv")
spongebob_spirv_words("${_dir}/ge.vert.spv" vert_words)
spongebob_spirv_words("${_dir}/ge.frag.spv" frag_words)

file(WRITE "${OUT}"
"#pragma once
#include <cstdint>
// Generated from ge.vert and ge.frag by spongebob/host/vulkan/embed_spirv.cmake.
namespace spongebob {
inline constexpr std::uint32_t kVulkanGeVertexSpirv[] = {
${vert_words}};
inline constexpr std::uint32_t kVulkanGeFragmentSpirv[] = {
${frag_words}};
}
")

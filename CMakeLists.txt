cmake_minimum_required(VERSION 3.28)
project(gpu_bugs C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Vulkan REQUIRED)

# REPRO_SHADERS OFF builds only the programs; run them on the checked-in modules with
# --spv-dir <case>/spirv. No DXC, Slang or SPIRV-Tools needed then.
option(REPRO_SHADERS "Compile the shaders (needs dxc, slangc and SPIRV-Tools)" ON)
if(REPRO_SHADERS)
    find_program(DXC dxc HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
    find_program(SPIRV_VAL spirv-val HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
    find_program(SPIRV_DIS spirv-dis HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
    find_program(SPIRV_AS spirv-as HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
    find_program(SLANGC slangc HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
endif()

set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_DIR})

# repro_shader(<case target> <output name> <hlsl> [CONVERT_U_TO_PTR] <dxc arguments...>)
# Compiles HLSL to SPIR-V with DXC (Vulkan 1.3 target) and validates it with spirv-val.
# CONVERT_U_TO_PTR rewrites DXC's integer-to-pointer OpBitcasts into OpConvertUToPtr
# (common/convert_u_to_ptr.cmake), the good variant of case 04.
function(repro_shader target output hlsl)
    if(NOT REPRO_SHADERS)
        return()
    endif()
    set(out ${SHADER_DIR}/${output})
    set(args ${ARGN})
    set(dxc_out ${out})
    set(patch "")
    if(args AND "${ARGV3}" STREQUAL "CONVERT_U_TO_PTR")
        list(REMOVE_AT args 0)
        set(dxc_out ${out}.dxc)
        set(patch COMMAND ${CMAKE_COMMAND} -DIN=${dxc_out} -DOUT=${out}
                          -DSPIRV_DIS=${SPIRV_DIS} -DSPIRV_AS=${SPIRV_AS}
                          -P ${PROJECT_SOURCE_DIR}/common/convert_u_to_ptr.cmake)
    endif()
    add_custom_command(OUTPUT ${out}
        COMMAND ${DXC} -spirv -fspv-target-env=vulkan1.3 -HV 202x -enable-16bit-types
                ${args} ${CMAKE_CURRENT_SOURCE_DIR}/${hlsl} -Fo ${dxc_out}
        ${patch}
        COMMAND ${SPIRV_VAL} --target-env vulkan1.3 --scalar-block-layout ${out}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${hlsl} ${PROJECT_SOURCE_DIR}/common/convert_u_to_ptr.cmake
        COMMENT "${output}"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY SHADER_OUTPUTS ${out})
endfunction()

# repro_slang(<case target> <output name> <slang> <slangc arguments...>)
# Compiles Slang to SPIR-V 1.6 with scalar layout, no debug info, and validates it.
function(repro_slang target output slang)
    if(NOT REPRO_SHADERS)
        return()
    endif()
    set(out ${SHADER_DIR}/${output})
    add_custom_command(OUTPUT ${out}
        COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/${slang} -target spirv -profile sm_6_6+spirv_1_6
                -fvk-use-entrypoint-name -fvk-use-scalar-layout -g0 ${ARGN} -o ${out}
        COMMAND ${SPIRV_VAL} --target-env vulkan1.3 --scalar-block-layout ${out}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${slang}
        COMMENT "${output}"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY SHADER_OUTPUTS ${out})
endfunction()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
add_library(repro_common STATIC common/repro.c)
target_include_directories(repro_common PUBLIC common)
target_link_libraries(repro_common PUBLIC Vulkan::Vulkan $<$<NOT:$<PLATFORM_ID:Windows>>:m>)

# repro_case(<target> <source dir>): one executable per case; shaders added with repro_shader or repro_slang.
function(repro_case target dir)
    add_executable(${target} ${dir}/main.c)
    target_link_libraries(${target} PRIVATE repro_common)
endfunction()

function(repro_case_shaders target)
    get_property(outputs TARGET ${target} PROPERTY SHADER_OUTPUTS)
    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()


set(CS -T cs_6_6 -E cs_main)

# 01: struct with pointer members loaded by value.
repro_case(amd_struct_pointer_spill 01.amd_struct_pointer_spill)
set(H 01.amd_struct_pointer_spill/amd_struct_pointer_spill.hlsl)
repro_shader(amd_struct_pointer_spill amd_struct_pointer_spill.bad.spv    ${H} ${CS} -Od -DBY_VALUE=1)
repro_shader(amd_struct_pointer_spill amd_struct_pointer_spill.bad_o3.spv ${H} ${CS} -O3 -DBY_VALUE=1)
repro_shader(amd_struct_pointer_spill amd_struct_pointer_spill.good.spv   ${H} ${CS} -Od -DBY_VALUE=0)
repro_case_shaders(amd_struct_pointer_spill)

# 02: workgroup-scope fence reached by some subgroups only, then a workgroup control barrier.
repro_case(amd_divergent_group_fence 02.amd_divergent_group_fence)
set(H 02.amd_divergent_group_fence/amd_divergent_group_fence.hlsl)
repro_shader(amd_divergent_group_fence amd_divergent_group_fence.bad.spv  ${H} ${CS} -DFENCE_SCOPE=2)
repro_shader(amd_divergent_group_fence amd_divergent_group_fence.good.spv ${H} ${CS} -DFENCE_SCOPE=3)
repro_case_shaders(amd_divergent_group_fence)

# 03: 16-bit load through a uint16_t pointer held in push constants. Slang: DXC has no pointer
# types, and its u64-address form (variant u64) does not reproduce.
repro_case(amd_u16_push_pointer 03.amd_u16_push_pointer)
set(S 03.amd_u16_push_pointer/amd_u16_push_pointer.slang)
repro_slang(amd_u16_push_pointer amd_u16_push_pointer.bad.spv ${S} -O1)
repro_slang(amd_u16_push_pointer amd_u16_push_pointer.u64.spv ${S} -O1 -DU64)
repro_slang(amd_u16_push_pointer amd_u16_push_pointer.u32.spv ${S} -O1 -DU32)
repro_case_shaders(amd_u16_push_pointer)

# 04: 16-bit load through a pointer made by OpBitcast from a 64-bit integer.
repro_case(amd_bitcast_u16_pointer 04.amd_bitcast_u16_pointer)
set(H 04.amd_bitcast_u16_pointer/amd_bitcast_u16_pointer.hlsl)
repro_shader(amd_bitcast_u16_pointer amd_bitcast_u16_pointer.bad.spv  ${H} ${CS} -DWIDTH=16)
repro_shader(amd_bitcast_u16_pointer amd_bitcast_u16_pointer.u32.spv  ${H} ${CS} -DWIDTH=32)
repro_shader(amd_bitcast_u16_pointer amd_bitcast_u16_pointer.good.spv ${H} CONVERT_U_TO_PTR ${CS} -DWIDTH=16)
repro_case_shaders(amd_bitcast_u16_pointer)

# 05: a uint16_t pointer held in push constants cast to uint* (OpBitcast pointer to pointer)
# crashes pipeline creation. Slang, as for 03: DXC has no pointer-to-pointer cast.
repro_case(amd_pointer_cast_pipeline 05.amd_pointer_cast_pipeline)
set(S 05.amd_pointer_cast_pipeline/amd_pointer_cast_pipeline.slang)
repro_slang(amd_pointer_cast_pipeline amd_pointer_cast_pipeline.bad.spv  ${S} -O1)
repro_slang(amd_pointer_cast_pipeline amd_pointer_cast_pipeline.good.spv ${S} -O1 -DGOOD)
repro_case_shaders(amd_pointer_cast_pipeline)

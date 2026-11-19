# cmake -DIN=<spv> -DOUT=<spv> -DSPIRV_DIS=... -DSPIRV_AS=... -P convert_u_to_ptr.cmake
# Rewrites every OpBitcast of an integer to a PhysicalStorageBuffer pointer into OpConvertUToPtr,
# the form Slang emits. DXC emits OpBitcast for vk::RawBufferLoad/Store addresses, and on the
# RX 6800 a 16-bit load through such a pointer reads wrong data (case 04).
execute_process(COMMAND ${SPIRV_DIS} ${IN} OUTPUT_VARIABLE text COMMAND_ERROR_IS_FATAL ANY)
string(REGEX REPLACE "OpBitcast (%_ptr_PhysicalStorageBuffer[A-Za-z0-9_]*)" "OpConvertUToPtr \\1"
       text "${text}")
file(WRITE ${OUT}.spvasm "${text}")
execute_process(COMMAND ${SPIRV_AS} --target-env vulkan1.3 ${OUT}.spvasm -o ${OUT}
                COMMAND_ERROR_IS_FATAL ANY)

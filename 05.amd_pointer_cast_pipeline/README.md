# 05. Casting a `uint16_t` pointer from push constants to `uint*` crashes pipeline creation

The push constant block holds a `PhysicalStorageBuffer` pointer to `uint16_t`. The shader
casts it to a pointer to `uint` with `OpBitcast` and loads 32-bit words through it. On the
RX 6800, `vkCreateComputePipelines` crashes the process with an access violation
(0xC0000005), so the program stops after `creating compute pipeline cs_main ...` and prints no
`RESULT`. The GTX 1660 creates the pipeline in about 17 ms and gets the right values.
`spirv-val` and the validation layer are clean on both.

```
.builds\default\amd_pointer_cast_pipeline.exe --variant bad
```

Source: [amd_pointer_cast_pipeline.slang](amd_pointer_cast_pipeline.slang), compiled by `slangc -O1`.
The `bad` module ([spirv/amd_pointer_cast_pipeline.bad.spvasm](spirv/amd_pointer_cast_pipeline.bad.spvasm)):

```
%Push = OpTypeStruct %_ptr_PhysicalStorageBuffer_float %_ptr_PhysicalStorageBuffer_ushort   ; Block, offsets 0 and 8
...
   %24 = OpLoad %_ptr_PhysicalStorageBuffer_ushort %23
%words = OpBitcast %_ptr_PhysicalStorageBuffer_uint %24        ; pointer to pointer
...
   %33 = OpPtrAccessChain %_ptr_PhysicalStorageBuffer_uint %words %32
   %34 = OpLoad %uint %33 Aligned 4
```

| Variant | How the push constants hold `weights` | RX 6800, 26.10.1 and 26.11.2 | Vega 11, Linux proprietary 23.30 |
|---|---|---|---|
| `bad` | `uint16_t*`, cast to `uint*` at the load | **CRASH** in `vkCreateComputePipelines` (0xC0000005) | **CRASH**: SIGSEGV in `vkCreateComputePipelines` |
| `good` | `uint*`, no cast; the same loads | PASS | PASS |

- **Expected:** the pipeline is created and `output[i] = weights[i % 64]`, the values 1-64.
- **RX 6800:** the process crashes inside the driver's pipeline compile, before the validation
  layer can report anything.
- **Vega 11, proprietary 23.30:** the same crash, so there is no ISA to compare.

Every variant passes, with the validation layer clean, on the GTX 1660 SUPER (NVIDIA 621.01)
and, on the Vega 11, with Mesa RADV and AMDVLK 2023.Q3.3. RADV creates the `bad` pipeline in
0.8 ms.

## Notes

SPIR-V allows `OpBitcast` between two pointer types of the same storage class. Related to
[03](../03.amd_u16_push_pointer): there, a 16-bit load through the same uncast `uint16_t*`
member reads 0. HLSL can't express this: DXC has no pointer-to-pointer cast, so this case is
written in Slang.

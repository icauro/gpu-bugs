# 01. Struct of pointers in a Function variable: wrong results, ~1000x slower

A compute shader does a trilinear probe lookup through a `Volume` struct holding three
`vk::BufferPointer` members and some counts. 1M points, 64 threads per group.

```
.builds\default\amd_struct_pointer_spill.exe --variant bad
```

Source: [amd_struct_pointer_spill.hlsl](amd_struct_pointer_spill.hlsl), compiled by DXC. The `bad`
module ([spirv/amd_struct_pointer_spill.bad.spvasm](spirv/amd_struct_pointer_spill.bad.spvasm)):

```
   %v = OpVariable %_ptr_Function_Volume_0 Function        ; struct with 3 pointer members
...
  %84 = OpLoad %Volume %83 Aligned 16                       ; the whole struct, through push.volume
  %85 = OpCopyLogical %Volume_0 %84
        OpStore %v %85
...
 %152 = OpAccessChain %_ptr_Function__ptr_PhysicalStorageBuffer_uint %v %int_0
 %153 = OpLoad %_ptr_PhysicalStorageBuffer_uint %152       ; pointer read back from %v
```

| Variant | Shader | RX 6800, 26.10.1 and 26.11.2 | Vega 11, Linux proprietary 23.30 |
|---|---|---|---|
| `bad` | `Volume v = push.volume.Get();` then `v.states`, `v.irradiance`, ... DXC `-Od` keeps `%v = OpVariable %_ptr_Function_Volume Function` and loads the pointers back from it | **WRONG**: 4191515 of 4194304 values (mostly 0), 2.3 to 3.0 s | **CRASH**: SIGSEGV in the driver during `vkCreateShaderModule` |
| `bad_o3` | same source, DXC `-O3`: no Function variable | PASS | PASS |
| `good` | every member read through `push.volume.Get()`, `-Od`: no pointer-typed Function variable | PASS, 2 ms | PASS, 3 ms |

- **Expected:** the same values as `good`, as on the GTX 1660.
- **RX 6800:** wrong values on every run, and about 1000 times slower. The validation layer is
  clean.
- **Vega 11, proprietary 23.30:** the driver crashes on the same module, so there is no ISA
  to compare.

Every variant passes, with the validation layer clean, on the GTX 1660 SUPER (NVIDIA 621.01)
and, on the Vega 11, with Mesa RADV and AMDVLK 2023.Q3.3.

## Notes

In a larger application, the same pattern, compiled by Slang, hung the GPU with no fault
reported.

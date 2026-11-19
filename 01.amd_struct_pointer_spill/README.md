# 01. Struct of pointers in a Function variable: wrong results, ~1000x slower

When a struct of `PhysicalStorageBuffer` pointers is copied into a Function variable and the
pointers are read back from it, almost every result is wrong on the RX 6800, and the dispatch
takes about 1000 times longer. The shader does a trilinear probe lookup through a `Volume`
struct with three `vk::BufferPointer` members and some counts, over 1M points with 64 threads
per group.

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
| `bad` | `Volume v = push.volume.Get();` then `v.states`, `v.irradiance`, ... DXC `-Od` keeps `%v = OpVariable %_ptr_Function_Volume Function` and loads the pointers back from it | WRONG: 4191515 of 4194304 values (mostly 0), 2.3 to 3.0 s | CRASH: SIGSEGV in the driver during `vkCreateShaderModule` |
| `bad_o3` | same source, DXC `-O3`: no Function variable | PASS | PASS |
| `good` | every member read through `push.volume.Get()`, `-Od`: no pointer-typed Function variable | PASS, 2 ms | PASS, 3 ms |

`bad` should give the same values as `good`, as it does on the GTX 1660. On the RX 6800 it is
wrong on every run, with the validation layer clean. The Vega 11 proprietary driver crashes on
the module, so there is no ISA from it to compare.

Every variant passes with the validation layer clean on the GTX 1660 SUPER (NVIDIA 621.01),
and on the Vega 11 with Mesa RADV and AMDVLK 2023.Q3.3.

## Notes

In a larger application the same pattern, compiled by Slang, hung the GPU with no fault
reported.

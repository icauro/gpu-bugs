# 03. 16-bit load through a pointer held in push constants reads 0

On the RX 6800, a 16-bit load through a `uint16_t` pointer held in push constants always
returns 0. The push constant block holds a `PhysicalStorageBuffer` pointer to `uint16_t`, and
each invocation loads one value through it and stores it as a float. The GTX 1660 reads the
stored values. `spirv-val` and the validation layer are clean.

```
.builds\default\amd_u16_push_pointer.exe --variant bad
```

Source: [amd_u16_push_pointer.slang](amd_u16_push_pointer.slang), compiled by `slangc -O1`. The `bad`
module ([spirv/amd_u16_push_pointer.bad.spvasm](spirv/amd_u16_push_pointer.bad.spvasm)):

```
%Push = OpTypeStruct %_ptr_PhysicalStorageBuffer_float %_ptr_PhysicalStorageBuffer_ushort   ; Block, offsets 0 and 8
...
%23 = OpAccessChain %_ptr_PushConstant__ptr_PhysicalStorageBuffer_ushort %push %int_1
%24 = OpLoad %_ptr_PhysicalStorageBuffer_ushort %23
%25 = OpPtrAccessChain %_ptr_PhysicalStorageBuffer_ushort %24 %s       ; s = id % 64
%26 = OpLoad %ushort %25 Aligned 2                                      ; returns 0
 %w = OpConvertUToF %float %26
```

| Variant | How the push constants hold `weights` | RX 6800, 26.10.1 and 26.11.2 | Vega 11, Linux proprietary 23.30 |
|---|---|---|---|
| `bad` | `uint16_t*` | WRONG: 65536 of 65536 values are 0 | WRONG: 65536 of 65536 are 0; 2039 ms against 0.4 ms |
| `u64` | `uint64_t`, turned into a `uint16_t*` by `OpConvertUToPtr` at the load; the same 16-bit load | PASS | PASS |
| `u32` | `uint*`; the 32-bit word holding the value is loaded and shifted | PASS | PASS |

The expected output is `output[i] = weights[i % 64]`, the values 1 to 64. On the RX 6800 it
is all 0, with the validation layer clean for all three variants.

Every variant passes with the validation layer clean on the GTX 1660 SUPER (NVIDIA 621.01),
and on the Vega 11 with Mesa RADV and AMDVLK 2023.Q3.3.

## Cause in the generated ISA

AMD's proprietary shader compiler drops the high 32 bits of the pointer. The ISA below is from
the proprietary Linux driver 23.30 on a Vega 11 (GFX9), read through
`VK_KHR_pipeline_executable_properties` with `--dump-isa`. Push constants arrive in `s[2:5]`:
`output` in `s[2:3]`, `weights` in `s[4:5]`.

```
; bad (uint16_t* member)                         ; u64 (uint64_t member + OpConvertUToPtr)
v_lshl_add_u32  v0, s6, 6, v0                    v_lshl_add_u32  v0, s6, 6, v0
v_and_b32       v1, 63, v0                       v_and_b32       v1, 63, v0
v_lshlrev_b32   v1, 1, v1                        v_lshlrev_b32   v1, 1, v1
v_add_co_u32    v1, vcc, s4, v1                  v_add_co_u32    v1, vcc, s4, v1
                                                 v_mov_b32       v2, s5        ; high half
v_addc_co_u32   v2, vcc, v2, 0, vcc   ; v2 undef v_addc_co_u32   v2, vcc, v2, 0, vcc
global_load_ushort v1, v[1:2], off               global_load_ushort v1, v[1:2], off
```

In `bad`, `v_mov_b32 v2, s5` is missing. The high half of the address comes from `v2`, which
nothing has written, so the load reads from some other address. The compiler's own statistics
agree: `bHasUndefUsed = true` for `bad` and `false` for `u64` and `u32`. The bad address
probably also explains the 2 s run time of `bad` on the Vega 11.

The other drivers keep the high half. RADV (ACO) emits `v_mov_b32 v1, s5` before its
`v_addc_co_u32`. AMDVLK (LLPC) loads through the 64-bit SGPR pair, as
`global_load_ushort v2, v1, s[6:7]`.

The RX 6800 (RDNA2, GFX10.3, Windows driver 26.10.1) drops the high half the same way, and also
keeps only the low 16 bits of the low half:

```
s_and_b32           s0, s4, 0xffff               ; weights low dword, masked to 16 bits
v_lshlrev_b32       v0, 1, v0
v_add_co_u32        v0, vcc_lo, s0, v0
v_add_co_ci_u32_e64 v1, vcc_lo, v1, 0, vcc_lo    ; v1 never written; s5 never read
global_load_ushort  v0, v[0:1], off
```

Dumps: [isa/](isa/) (`rx6800_bad.txt`, `pro_bad.txt`, `pro_u64.txt`, `pro_u32.txt`,
`amdvlk_bad.txt`, `radv_bad.txt`). Make them with `--dump-isa <file>`.

## Notes

Only the pointer type of the push constant member matters. While reducing the original
shader, a loop around the load, loading the index from memory, and reading the pointer once
before the loop all made no difference. HLSL can't express this: DXC passes addresses as
`uint64_t`, the `u64` form, so this case is written in Slang.

## Kernel crashes while reducing

Not reproducible from this repo. Kept for the record.

- Bugcheck 0xD1 (DRIVER_IRQL_NOT_LESS_OR_EQUAL) in amdkmdag.sys, parameters
  `0xffff868ff4df0018, 0x8, 0x0, 0xfffff8077b6d5100`. It came moments after one run of
  [spirv/bugcheck/u16_load_loop.slang.spv](spirv/bugcheck/u16_load_loop.slang.spv), the
  original larger Slang module that this case was reduced from, with an edit to write out
  the loaded value. The run itself had printed all-zero results and exited. The program that
  ran it is in this repo's history (commit 5adceb6, `u16_load_loop.exe --variant
  slang_shape_only`). It was not run again. **Do not run it on a machine you need.**
- Bugcheck 0x13A (KERNEL_MODE_HEAP_CORRUPTION), parameters `0x12, 0xffffe58576000140,
  0xffffe585a9592000, 0x0`. It happened during a batch of four runs of intermediate
  reductions. Which run caused it is not known.

Kernel minidumps for both are available on request.

# 04. 16-bit load through a pointer made by OpBitcast reads wrong data

One thread per element copies `src[i]` to `dst[i]`, with `src` read through
`vk::RawBufferLoad<uint16_t>`.

```
.builds\default\amd_bitcast_u16_pointer.exe --variant bad
```

Source: [amd_bitcast_u16_pointer.hlsl](amd_bitcast_u16_pointer.hlsl), compiled by DXC. The `bad`
module ([spirv/amd_bitcast_u16_pointer.bad.spvasm](spirv/amd_bitcast_u16_pointer.bad.spvasm)):

```
%addr = OpIAdd %ulong %base %offset
%p    = OpBitcast %_ptr_PhysicalStorageBuffer_ushort %addr
%v    = OpLoad %ushort %p Aligned 2
```

`OpBitcast` from a 64-bit integer to a `PhysicalStorageBuffer` pointer is valid SPIR-V.
`spirv-val` passes, the validation layer is clean, and the device enables `shaderInt16`,
`storageBuffer16BitAccess` and `bufferDeviceAddress`.

| Variant | Load | Pointer | RX 6800, 26.10.1 and 26.11.2 | Vega 11, Linux proprietary 23.30 |
|---|---|---|---|---|
| `bad` | `uint16_t` | `OpBitcast` | **WRONG**: 65535 of 65536 values | **WRONG**: 65535 of 65536 |
| `u32` | `uint` | `OpBitcast` | PASS | PASS |
| `good` | `uint16_t` | `OpConvertUToPtr` (the DXC output patched by `common/convert_u_to_ptr.cmake`) | PASS | PASS |

- **Expected:** `dst` equals `src`, as on the GTX 1660.
- **RX 6800:** `dst` holds the same sequence (104, 0, 55272, 7411, 18436, ...) on every run.
  The sequence is the same with `src` in host-cached memory, in device-local memory, or a
  different buffer altogether, so the load is not reading the address it was given. The
  validation layer is clean for `bad`.

Every variant passes, with the validation layer clean, on the GTX 1660 SUPER (NVIDIA 621.01)
and, on the Vega 11, with Mesa RADV and AMDVLK 2023.Q3.3.

## Cause in the generated ISA

AMD's proprietary shader compiler builds the 64-bit address from the two 16-bit halves of the
low 32 bits. The ISA below is from the proprietary Linux driver 23.30 on a Vega 11 (GFX9), read
through `VK_KHR_pipeline_executable_properties` with `--dump-isa`. Push constants arrive in
`s[2:5]`: `src` in `s[2:3]`, `dst` in `s[4:5]`; `v2` is the element index.

```
; bad (OpBitcast)                                ; good (OpConvertUToPtr)
v_lshl_add_u32  v0, v2, 1, s2                    v_lshlrev_b32   v0, 1, v2
                                                 v_add_co_u32    v0, vcc, s2, v0
v_mov_b32       v3, v0 src0_sel: WORD_0          v_mov_b32       v1, s3
v_mov_b32       v4, v0 src0_sel: WORD_1          v_addc_co_u32   v1, vcc, v1, 0, vcc
global_load_ushort v0, v[3:4], off               global_load_ushort v0, v[0:1], off
```

In `bad` the 64-bit `OpIAdd` becomes a 32-bit add: `s3`, the high half of `src`, is never
read. Then the sum is split into two zero-extended 16-bit halves, low 16 bits into the address
low dword and high 16 bits into the address high dword, as if the 32-bit value were a vector of
two 16-bit elements to widen. The load address is `(lo >> 16) << 32 | (lo & 0xffff)` rather
than `src + 2 * i`, which matches what the RX 6800 shows: the same wrong values on every run,
whatever buffer `src` is. The compiler statistics report `bHasUndefUsed = false` for both
variants, so nothing undefined is read; the address is computed wrongly. The `u32` variant
uses the same `OpBitcast` with a 32-bit load, and its address is right.

The other drivers keep the full address. RADV (ACO) and AMDVLK (LLPC) both load through the
64-bit SGPR pair, as `global_load_ushort v1, v1, s[2:3]`.

The RX 6800 (RDNA2, GFX10.3, Windows driver 26.10.1) emits the same sequence: a 32-bit
`v_lshl_add_u32 v0, v2, 1, s2`, then `WORD_0` and `WORD_1` moves into `v[3:4]` as the load
address.

Dumps: [isa/](isa/) (`rx6800_bad.txt`, `pro_bad.txt`, `pro_u32.txt`, `pro_good.txt`,
`amdvlk_bad.txt`, `radv_bad.txt`). Make them with `--dump-isa <file>`.

## Notes

- A `uint16_t2` load through `OpBitcast` is wrong the same way.
- Declaring `StorageBuffer16BitAccess` and `SPV_KHR_16bit_storage` in the module (DXC leaves
  them out) changes nothing.

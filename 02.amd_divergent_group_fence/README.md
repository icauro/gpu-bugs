# 02. Workgroup-scope memory barrier in subgroup-divergent flow breaks a later control barrier

On the RX 6800, a workgroup-scope `OpMemoryBarrier` that only some subgroups reach breaks the
workgroup control barrier after it, and every result is wrong. Some subgroups run a loop with
`GroupMemoryBarrier()` in it and the others skip the loop. Then every invocation reaches
`GroupMemoryBarrierWithGroupSync()` and reads a value that another subgroup wrote before it.
The GTX 1660 matches the CPU reference. `spirv-val` and the validation layer are clean on
both.

```
.builds\default\amd_divergent_group_fence.exe --variant bad
```

Source: [amd_divergent_group_fence.hlsl](amd_divergent_group_fence.hlsl), compiled by DXC. The
`bad` module ([spirv/amd_divergent_group_fence.bad.spvasm](spirv/amd_divergent_group_fence.bad.spvasm)):

```
; only subgroups whose flag is set (alternating every 64 invocations)
OpMemoryBarrier %uint_2 %uint_264                  ; Workgroup, AcquireRelease | WorkgroupMemory
...
OpMemoryBarrier %uint_2 %uint_264
; every invocation
OpControlBarrier %uint_2 %uint_2 %uint_264
...                                                  ; reads g_shared[(local + 64) % 256]
OpControlBarrier %uint_2 %uint_2 %uint_264
```

SPIR-V allows `OpMemoryBarrier` in control flow that only some invocations reach. It orders
memory and doesn't wait for other invocations.

1024 workgroups of 256 invocations, 8 passes. In each pass, the subgroups whose flag is set
run 4 rounds of: write own slot, fence, read slot `local ^ 1` and add 1, fence. Then every
invocation writes its value, the workgroup meets at the control barrier, and each invocation
adds the value 64 invocations further on, which is in another subgroup.

| Variant | Fence | Subgroups that run the fenced rounds | RX 6800, 26.10.1 and 26.11.2 | Vega 11, Linux proprietary 23.30 |
|---|---|---|---|---|
| `bad` | `OpMemoryBarrier` Workgroup | half (subgroup size 64) | WRONG: 262144 of 262144 values differ | WRONG: 262144 of 262144 |
| `uniform` | same module | all | PASS | PASS |
| `good` | `OpMemoryBarrier` Subgroup | half | PASS | PASS |

The expected values come from the CPU reference in [main.c](main.c). On the RX 6800 every
value differs and they change from run to run. The validation layer is clean for `bad` there.

Every variant passes with the validation layer clean on the GTX 1660 SUPER (NVIDIA 621.01),
and on the Vega 11 with Mesa RADV and AMDVLK 2023.Q3.3.

## Cause in the generated ISA

AMD's proprietary shader compiler emits `s_barrier`, a workgroup execution barrier, for the
workgroup-scope `OpMemoryBarrier`, inside the branch that only some subgroups run. The ISA
below is from the proprietary Linux driver 23.30 on a Vega 11 (GFX9), read through
`VK_KHR_pipeline_executable_properties` with `--dump-isa`. It is one pass: the fenced rounds
under `s_and_saveexec` / `s_cbranch_execz`, then the control barrier section.

```
; bad (Workgroup-scope fence)                    ; good (Subgroup-scope fence)
  s_and_saveexec_b64  s[4:5], s[0:1]               s_and_saveexec_b64  s[4:5], s[0:1]
  s_cbranch_execz  label_0034  ; skip if no flag   s_cbranch_execz  label_0032
  ...                                              ...
label_0028:                    ; the 4 rounds    label_0028:
  ds_write_b32  v0, v3                             ds_write_b32  v0, v3
  s_barrier                    ; fence 1           ds_read_b32   v3, v1
  ds_read_b32   v3, v1                             s_waitcnt     lgkmcnt(0)
  s_waitcnt     lgkmcnt(0)                         v_add_u32     v3, 1, v3
  v_add_u32     v3, 1, v3                          s_cbranch_scc1  label_0028
  s_barrier                    ; fence 2         label_0032:
  s_cbranch_scc1  label_0028                       s_mov_b64     exec, s[4:5]
label_0034:                                        ds_write_b32  v0, v3 offset:1024
  s_mov_b64     exec, s[4:5]                       s_barrier
  ds_write_b32  v0, v3 offset:1024                 ds_read_b32   v5, v4 offset:1024
  s_barrier                    ; control barrier   ...
  ds_read_b32   v5, v4 offset:1024                 s_barrier
  ...
  s_barrier                    ; control barrier
```

`s_barrier` counts waves and has no idea which barrier in the program each wave is at. In
`bad`, a subgroup whose flag is clear skips the rounds, so its first `s_barrier` is the control
barrier while the flagged subgroups are at their first fence. Those meet and release each
other, and from there on every barrier pairs with the wrong one. The skipping subgroups read
`g_shared` before the others have written it. `uniform` runs the same module with every subgroup flagged, so all
waves reach the same barriers in the same order, and it passes. `good` has no barrier inside
the branch.

A workgroup-scope memory barrier only has to wait for the wave's own LDS accesses
(`s_waitcnt lgkmcnt(0)`), and it must not wait for other waves. RADV (ACO) and AMDVLK (LLPC) emit
two `s_barrier` instructions in `bad`, both for the control barriers after the branch.

The RX 6800 (RDNA2, GFX10.3, Windows driver 26.10.1) emits the same code: both fences are
`s_barrier` inside the `s_and_saveexec_b64` / `s_cbranch_execz` region, before the two control
barriers after it.

Dumps: [isa/](isa/) (`rx6800_bad.txt`, `pro_bad.txt`, `pro_uniform.txt`, `pro_good.txt`,
`amdvlk_bad.txt`, `radv_bad.txt`). Make them with `--dump-isa <file>`.

## Notes

Without the control barrier after the divergent section, every variant passes. In the
application this came from, the same pattern lost the device instead of giving wrong values.

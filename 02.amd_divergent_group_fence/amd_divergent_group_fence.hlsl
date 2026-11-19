// A workgroup-scope memory barrier (OpMemoryBarrier Workgroup) reached by only some of the
// workgroup's subgroups, followed by a control barrier the whole workgroup reaches.
//
// Each pass: subgroups whose flag is set run ROUNDS rounds of "write own slot, fence, read slot
// local ^ 1, fence"; the others skip them. Then every invocation publishes its value, the whole
// workgroup meets at GroupMemoryBarrierWithGroupSync, and adds the value of the invocation 64
// places on, which is in another subgroup. SPIR-V allows OpMemoryBarrier in control flow that
// only some subgroups reach: it is not a control barrier.
//
// FENCE_SCOPE 2 is Workgroup (GroupMemoryBarrier), 3 is Subgroup, the workaround.

struct Push {
    uint64_t input;  // uint[groups * 256]
    uint64_t flags;  // uint[groups * 256], read at the subgroup's first invocation
    uint64_t output; // uint2[groups * 256]: value, subgroup size
    uint     rounds;
    uint     passes;
};

[[vk::push_constant]] Push push;

groupshared uint g_slots[256];
groupshared uint g_shared[256];

// OpMemoryBarrier = 225. Scope and semantics are constant ids.
[[vk::ext_instruction(225)]] void spv_memory_barrier(uint scope, uint semantics);

void fence() {
#if FENCE_SCOPE == 3
    spv_memory_barrier(3 /* Subgroup */, 0x8 | 0x100 /* AcquireRelease | WorkgroupMemory */);
#else
    GroupMemoryBarrier(); // Workgroup scope, AcquireRelease | WorkgroupMemory
#endif
}

[numthreads(256, 1, 1)] void cs_main(uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    uint lanes = WaveGetLaneCount();
    uint first = local - WaveGetLaneIndex();
    uint base  = group.x * 256;
    uint value = vk::RawBufferLoad<uint>(push.input + (base + local) * 4, 4);
    uint flag  = vk::RawBufferLoad<uint>(push.flags + (base + first) * 4, 4);
    for (uint pass = 0; pass < push.passes; ++pass) {
        if (flag != 0) {
            for (uint round = 0; round < push.rounds; ++round) {
                g_slots[local] = value;
                fence();
                value = g_slots[local ^ 1] + 1;
                fence();
            }
        }
        g_shared[local] = value;
        GroupMemoryBarrierWithGroupSync();
        value += g_shared[(local + 64) % 256];
        GroupMemoryBarrierWithGroupSync();
    }
    vk::RawBufferStore<uint2>(push.output + (base + local) * 8, uint2(value, lanes), 8);
}

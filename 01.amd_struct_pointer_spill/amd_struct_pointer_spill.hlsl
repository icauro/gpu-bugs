// A struct holding buffer device addresses as pointer types (vk::BufferPointer), loaded by value
// into a local, then its pointer members dereferenced in a loop. Reduced from an
// application's probe lookup, where `Volume v = *ptr;` in Slang put the struct, pointer members and all,
// into a Function-storage variable; the RX 6800 hung ("no fault detected"). spirv-val passed.
// Reading each member through the push-constant pointer instead (BY_VALUE = 0) ran.
//
// DXC's optimiser removes the local; built with -Od the module keeps
// `OpVariable Function` of the struct type (with PhysicalStorageBuffer pointer members), which
// is the pattern Slang emitted with optimisation on.

struct Volume {
    vk::BufferPointer<uint>   states;     // uint per probe, 0 = dead
    vk::BufferPointer<float4> irradiance; // float4 per probe
    vk::BufferPointer<float4> offsets;    // float4 per probe, w = weight
    uint3                     counts;     // probes per axis
    float                     spacing;
};

struct Push {
    vk::BufferPointer<Volume> volume;
    uint64_t                  points; // float4[count], xyz = position
    uint64_t                  output; // float4[count]
    uint                      count;
};

[[vk::push_constant]] Push push;

// A macro, not a function: with -Od a function parameter of pointer type would be a Function
// variable too, in the good variant as well.
#define load4(base, index) vk::BufferPointer<float4>((uint64_t)(base) + (index) * 16).Get()

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.count) {
        return;
    }
#if BY_VALUE
    Volume v = push.volume.Get();
#define VOLUME v
#else
#define VOLUME push.volume.Get()
#endif
    float3 at    = vk::RawBufferLoad<float4>(push.points + id.x * 16, 16).xyz;
    float3 cell  = at / VOLUME.spacing;
    int3   base  = clamp(int3(floor(cell)), int3(0, 0, 0), int3(VOLUME.counts) - 2);
    float3 t     = saturate(cell - float3(base));
    float4 sum   = 0;
    float  total = 0;
    for (uint corner = 0; corner < 8; ++corner) {
        uint3 offset = uint3(corner & 1, (corner >> 1) & 1, corner >> 2);
        uint3 probe3 = uint3(base) + offset;
        uint  probe  = probe3.x + VOLUME.counts.x * (probe3.y + VOLUME.counts.y * probe3.z);
        uint  state  = vk::BufferPointer<uint>((uint64_t)VOLUME.states + probe * 4).Get();
        if (state == 0) {
            continue;
        }
        float3 w3     = lerp(1 - t, t, float3(offset));
        float  weight = w3.x * w3.y * w3.z * load4(VOLUME.offsets, probe).w;
        sum += load4(VOLUME.irradiance, probe) * weight;
        total += weight;
    }
    vk::RawBufferStore<float4>(push.output + id.x * 16, sum / max(total, 1e-4), 16);
}

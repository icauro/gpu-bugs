// output[id] = float(weights[id % 64]): one 16-bit load per invocation.
// The variants change only how the push constants hold weights:
//   (none)  uint16_t*                                   RX 6800: reads 0
//   U64     uint64_t, cast to uint16_t* at the load      RX 6800: right
//   U32     uint*, the weight taken from its 32-bit word RX 6800: right
struct Push
{
    float* output;
#if defined(U64)
    uint64_t weights;
#elif defined(U32)
    uint* weights;
#else
    uint16_t* weights;
#endif
};

[[vk::push_constant]] ConstantBuffer<Push> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 gid: SV_DispatchThreadID)
{
    uint id = gid.x;
    uint s  = id % 64;
#if defined(U64)
    float w = float(*(uint16_t*)(push.weights + s * 2));
#elif defined(U32)
    float w = float((push.weights[s >> 1] >> ((s & 1) * 16)) & 0xffff);
#else
    float w = float(push.weights[s]);
#endif
    push.output[id] = w;
}

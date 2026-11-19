// output[id] = float(weights[id % 64]), the 16-bit weight read from the 32-bit word holding it.
// The variants change only the type of the push constant member:
//   (none)  uint16_t*, cast to uint* at the load (OpBitcast between PhysicalStorageBuffer
//           pointer types)                       RX 6800: vkCreateComputePipelines crashes
//   GOOD    uint*, no cast                       RX 6800: right
struct Push
{
    float* output;
#if defined(GOOD)
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
#if defined(GOOD)
    uint* words = push.weights;
#else
    uint* words = (uint*)push.weights;
#endif
    push.output[id] = float((words[s >> 1] >> ((s & 1) * 16)) & 0xffff);
}

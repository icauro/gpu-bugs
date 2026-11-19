// One thread per element: dst[i] = src[i], where src is read through a buffer device address.
// DXC turns vk::RawBufferLoad<T>(address) into
//     %p = OpBitcast %_ptr_PhysicalStorageBuffer_T %address    ; address: 64-bit integer
//     %v = OpLoad %T %p Aligned <n>
// With T a 16-bit type the RX 6800 returns wrong values: the same values whatever buffer the
// address points at. With T 32-bit, or with OpConvertUToPtr in place of OpBitcast (the good
// variant, patched after DXC), the load is right.
//
// Defines: WIDTH = 16 | 32.

struct Push {
    uint64_t src; // uint16_t[count] (WIDTH 16) or uint[count] (WIDTH 32)
    uint64_t dst; // uint[count]
    uint     count;
};

[[vk::push_constant]] Push push;

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.count) {
        return;
    }
#if WIDTH == 16
    uint value = (uint)vk::RawBufferLoad<uint16_t>(push.src + id.x * 2, 2);
#else
    uint value = vk::RawBufferLoad<uint>(push.src + id.x * 4, 4);
#endif
    vk::RawBufferStore<uint>(push.dst + id.x * 4, value, 4);
}

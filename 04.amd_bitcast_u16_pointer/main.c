// Case 04: a 16-bit load through a PhysicalStorageBuffer pointer made by OpBitcast from a 64-bit
// integer. See README.md. Variants:
//   bad   uint16_t load, pointer from OpBitcast       (wrong values on the RX 6800)
//   u32   uint load, pointer from OpBitcast           (right)
//   good  uint16_t load, pointer from OpConvertUToPtr (right)
#include "repro.h"

#include <stdlib.h>

typedef struct Push {
    uint64_t src, dst;
    uint32_t count, pad;
} Push;

enum { COUNT = 65536 };

int main(int argc, char **argv) {
    Repro r;
    repro_init(&r, argc, argv, "04.amd_bitcast_u16_pointer");
    bool wide = repro_variant_is(&r, "u32");
    if (!repro_variant_is(&r, "bad") && !repro_variant_is(&r, "good") && !wide) {
        repro_log("variants: bad, u32, good");
        repro_finish(&r, REPRO_SETUP_FAILED);
    }

    ReproBuffer src = repro_buffer(&r, COUNT * 4);
    ReproBuffer dst = repro_buffer(&r, COUNT * 4);
    for (uint32_t i = 0; i < COUNT; ++i) {
        uint32_t value = (i * 2654435761u) >> 16; // 16-bit pattern, distinct per element
        if (wide) {
            ((uint32_t *)src.mapped)[i] = value;
        } else {
            ((uint16_t *)src.mapped)[i] = (uint16_t)value;
        }
    }

    const char *file = wide                          ? "amd_bitcast_u16_pointer.u32.spv"
                       : repro_variant_is(&r, "good") ? "amd_bitcast_u16_pointer.good.spv"
                                                      : "amd_bitcast_u16_pointer.bad.spv";
    VkShaderModule   module   = repro_shader(&r, file);
    VkPipelineLayout layout   = repro_layout(&r, sizeof(Push));
    VkPipeline       pipeline = repro_compute_pipeline(&r, module, "cs_main", layout);

    Push push = {.src = src.address, .dst = dst.address, .count = COUNT};
    for (int run = 0; run < r.repeat; ++run) {
        VkCommandBuffer cmd = repro_begin(&r);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, COUNT / 64, 1, 1);
        repro_submit_wait(&r, cmd);
    }

    const uint32_t *result = dst.mapped;
    uint32_t        wrong  = 0;
    for (uint32_t i = 0; i < COUNT; ++i) {
        uint32_t expected = (i * 2654435761u) >> 16;
        if (result[i] != expected && wrong++ < 8) {
            repro_log("element %u: gpu %u (0x%04x), expected %u (0x%04x)", i, result[i], result[i],
                      expected, expected);
        }
    }
    repro_log("%u of %u values differ", wrong, COUNT);
    repro_finish(&r, wrong ? REPRO_WRONG : REPRO_PASS);
}

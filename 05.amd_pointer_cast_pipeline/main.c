// Case 05: casting a uint16_t pointer held in push constants to uint* crashes
// vkCreateComputePipelines on the RX 6800. See README.md. --variant bad | good runs amd_pointer_cast_pipeline.<variant>.spv.
#include "repro.h"

#include <stdio.h>

enum { COUNT = 65536, WEIGHTS = 64 };

typedef struct Push {
    uint64_t output;  // float per invocation
    uint64_t weights; // uint16_t[WEIGHTS]
} Push;

int main(int argc, char **argv) {
    Repro r;
    repro_init(&r, argc, argv, "05.amd_pointer_cast_pipeline");

    ReproBuffer weights = repro_buffer(&r, WEIGHTS * 2);
    ReproBuffer output  = repro_buffer(&r, COUNT * 4);
    uint16_t   *weight  = weights.mapped;
    for (uint32_t s = 0; s < WEIGHTS; ++s) {
        weight[s] = (uint16_t)(1 + s); // every weight non-zero
    }

    char file[128];
    snprintf(file, sizeof(file), "amd_pointer_cast_pipeline.%s.spv", r.variant);
    VkShaderModule   module   = repro_shader(&r, file);
    VkPipelineLayout layout   = repro_layout(&r, sizeof(Push));
    VkPipeline       pipeline = repro_compute_pipeline(&r, module, "cs_main", layout);

    Push push = {output.address, weights.address};
    for (int run = 0; run < r.repeat; ++run) {
        VkCommandBuffer cmd = repro_begin(&r);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, COUNT / 64, 1, 1);
        repro_submit_wait(&r, cmd);
    }

    // Expected: output[i] = weights[i % 64].
    const float *result = output.mapped;
    uint32_t     wrong = 0, zero = 0;
    for (uint32_t i = 0; i < COUNT; ++i) {
        float expected = (float)weight[i % WEIGHTS];
        if (result[i] != expected) {
            zero += result[i] == 0;
            if (wrong++ < 4) {
                repro_log("output[%u] = %g, expected %g", i, result[i], expected);
            }
        }
    }
    repro_log("%u of %u values wrong (%u of them 0)", wrong, COUNT, zero);
    repro_finish(&r, wrong ? REPRO_WRONG : REPRO_PASS);
}

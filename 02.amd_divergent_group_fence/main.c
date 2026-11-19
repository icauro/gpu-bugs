// Case 02: OpMemoryBarrier with Workgroup scope reached by some subgroups only, then a
// control barrier for the whole workgroup. See README.md.
// Variants:
//   bad       workgroup-scope fence, every other 64 invocations reach it
//   uniform   same shader, every subgroup reaches it
//   good      subgroup-scope fence, every other 64 invocations reach it (the workaround)
#include "repro.h"
#include <stdlib.h>
#include <string.h>

typedef struct Push {
    uint64_t input, flags, output;
    uint32_t rounds, passes;
} Push;

enum { GROUPS = 1024, ROUNDS = 4, PASSES = 8 };

int main(int argc, char **argv) {
    Repro r;
    repro_init(&r, argc, argv, "02.amd_divergent_group_fence");
    bool uniform = repro_variant_is(&r, "uniform");
    if (!repro_variant_is(&r, "bad") && !uniform && !repro_variant_is(&r, "good")) {
        repro_log("variants: bad, uniform, good");
        repro_finish(&r, REPRO_SETUP_FAILED);
    }

    uint32_t    n      = GROUPS * 256;
    ReproBuffer input  = repro_buffer(&r, n * 4);
    ReproBuffer flags  = repro_buffer(&r, n * 4);
    ReproBuffer output = repro_buffer(&r, n * 8);
    uint32_t   *in     = input.mapped;
    uint32_t   *flag   = flags.mapped;
    for (uint32_t i = 0; i < n; ++i) {
        in[i] = i * 2654435761u >> 8;
        // A subgroup reads the flag at its first invocation. It alternates every 64
        // invocations, so with subgroups of 32 or 64 half the subgroups run the fenced rounds.
        flag[i] = uniform ? 1u : ((i % 256) / 64) & 1u;
    }

    VkShaderModule module = repro_shader(
        &r, repro_variant_is(&r, "good") ? "amd_divergent_group_fence.good.spv"
                                         : "amd_divergent_group_fence.bad.spv");
    VkPipelineLayout layout   = repro_layout(&r, sizeof(Push));
    VkPipeline       pipeline = repro_compute_pipeline(&r, module, "cs_main", layout);

    Push push = {input.address, flags.address, output.address, ROUNDS, PASSES};
    for (int run = 0; run < r.repeat; ++run) {
        VkCommandBuffer cmd = repro_begin(&r);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, GROUPS, 1, 1);
        repro_submit_wait(&r, cmd);
    }

    // The CPU reference, a workgroup at a time.
    const uint32_t *out   = output.mapped;
    uint32_t        lanes = out[1];
    uint32_t        wrong = 0;
    uint32_t        v[256], t[256];
    for (uint32_t g = 0; g < GROUPS; ++g) {
        const uint32_t base = g * 256;
        memcpy(v, in + base, sizeof(v));
        for (uint32_t pass = 0; pass < PASSES; ++pass) {
            for (uint32_t round = 0; round < ROUNDS; ++round) {
                for (uint32_t l = 0; l < 256; ++l) {
                    t[l] = flag[base + l - l % lanes] ? v[l ^ 1] + 1 : v[l];
                }
                memcpy(v, t, sizeof(v));
            }
            for (uint32_t l = 0; l < 256; ++l) {
                t[l] = v[l] + v[(l + 64) % 256];
            }
            memcpy(v, t, sizeof(v));
        }
        for (uint32_t l = 0; l < 256; ++l) {
            const uint32_t i = base + l;
            if (out[i * 2] != v[l] || out[i * 2 + 1] != lanes) {
                if (wrong++ < 4) {
                    repro_log("invocation %u: gpu %u (lanes %u), cpu %u", i, out[i * 2],
                              out[i * 2 + 1], v[l]);
                }
            }
        }
    }
    repro_log("subgroup size %u, %u passes of %u fenced rounds", lanes, PASSES, ROUNDS);
    repro_log("%u of %u values differ", wrong, n);
    repro_finish(&r, wrong ? REPRO_WRONG : REPRO_PASS);
}

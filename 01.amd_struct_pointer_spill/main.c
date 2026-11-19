// Case 01: a struct with pointer members loaded by value, its pointers then dereferenced.
// See README.md. Variants:
//   bad      struct copied to a local, DXC -Od (local stays a Function variable)   (hang)
//   bad_o3   struct copied to a local, DXC -O3 (optimiser removes the local)
//   good     members read through the push-constant pointer, DXC -Od              (runs)
#include "repro.h"

#include <math.h>

// DXC lays the pointee out by std430 rules: uint3 counts aligns to 16, at offset 32.
typedef struct Volume {
    uint64_t states, irradiance, offsets;
    uint32_t pad[2];
    uint32_t counts[3];
    float    spacing;
} Volume;

typedef struct Push {
    uint64_t volume, points, output;
    uint32_t count, pad;
} Push;

enum { NX = 32, NY = 16, NZ = 32, POINTS = 1 << 20 };

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int main(int argc, char **argv) {
    Repro r;
    repro_init(&r, argc, argv, "01.amd_struct_pointer_spill");
    const char *file = repro_variant_is(&r, "bad")      ? "amd_struct_pointer_spill.bad.spv"
                       : repro_variant_is(&r, "bad_o3") ? "amd_struct_pointer_spill.bad_o3.spv"
                       : repro_variant_is(&r, "good")   ? "amd_struct_pointer_spill.good.spv"
                                                        : NULL;
    if (!file) {
        repro_log("variants: bad, bad_o3, good");
        repro_finish(&r, REPRO_SETUP_FAILED);
    }
    uint32_t    probes     = NX * NY * NZ;
    ReproBuffer volume     = repro_buffer(&r, sizeof(Volume));
    ReproBuffer states     = repro_buffer(&r, probes * 4);
    ReproBuffer irradiance = repro_buffer(&r, probes * 16);
    ReproBuffer offsets    = repro_buffer(&r, probes * 16);
    ReproBuffer points     = repro_buffer(&r, POINTS * 16);
    ReproBuffer output     = repro_buffer(&r, POINTS * 16);
    Volume     *vol        = volume.mapped;
    *vol = (Volume){
        states.address, irradiance.address, offsets.address, {0, 0}, {NX, NY, NZ}, 0.5f};
    uint32_t *state = states.mapped;
    float    *irr = irradiance.mapped, *off = offsets.mapped, *point = points.mapped;
    for (uint32_t p = 0; p < probes; ++p) {
        state[p]       = p % 7 != 3;
        irr[p * 4 + 0] = (float)(p % 32) / 32.0f;
        irr[p * 4 + 1] = (float)(p % 17) / 17.0f;
        irr[p * 4 + 2] = (float)(p % 5) / 5.0f;
        irr[p * 4 + 3] = 1.0f;
        off[p * 4 + 3] = 0.5f + (float)(p % 3) * 0.25f;
    }
    for (uint32_t i = 0; i < POINTS; ++i) {
        point[i * 4 + 0] = (float)(i % 1021) / 1021.0f * NX * 0.5f;
        point[i * 4 + 1] = (float)(i % 509) / 509.0f * NY * 0.5f;
        point[i * 4 + 2] = (float)(i % 2039) / 2039.0f * NZ * 0.5f;
    }

    VkShaderModule   module   = repro_shader(&r, file);
    VkPipelineLayout layout   = repro_layout(&r, sizeof(Push));
    VkPipeline       pipeline = repro_compute_pipeline(&r, module, "cs_main", layout);
    Push             push     = {volume.address, points.address, output.address, POINTS, 0};
    for (int run = 0; run < r.repeat; ++run) {
        VkCommandBuffer cmd = repro_begin(&r);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, POINTS / 64, 1, 1);
        repro_submit_wait(&r, cmd);
    }

    const float *out   = output.mapped;
    uint32_t     wrong = 0;
    int          n[3]  = {NX, NY, NZ};
    for (uint32_t i = 0; i < POINTS; ++i) {
        int   base[3];
        float t[3];
        for (int c = 0; c < 3; ++c) {
            float cell = point[i * 4 + c] / vol->spacing;
            base[c]    = (int)clampf(floorf(cell), 0.0f, (float)(n[c] - 2));
            t[c]       = clampf(cell - (float)base[c], 0.0f, 1.0f);
        }
        float sum[4] = {0, 0, 0, 0}, total = 0;
        for (uint32_t corner = 0; corner < 8; ++corner) {
            uint32_t o[3]  = {corner & 1, (corner >> 1) & 1, corner >> 2};
            uint32_t probe = (base[0] + o[0]) + NX * ((base[1] + o[1]) + NY * (base[2] + o[2]));
            if (!state[probe]) {
                continue;
            }
            float weight = off[probe * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                weight *= o[c] ? t[c] : 1.0f - t[c];
            }
            for (int c = 0; c < 4; ++c) {
                sum[c] += irr[probe * 4 + c] * weight;
            }
            total += weight;
        }
        for (int c = 0; c < 4; ++c) {
            float expected = sum[c] / (total > 1e-4f ? total : 1e-4f);
            if (fabsf(out[i * 4 + c] - expected) > 1e-3f && wrong++ < 4) {
                repro_log("point %u.%d: gpu %f, cpu %f", i, c, out[i * 4 + c], expected);
            }
        }
    }
    repro_log("%u of %u values differ", wrong, POINTS * 4);
    repro_finish(&r, wrong ? REPRO_WRONG : REPRO_PASS);
}

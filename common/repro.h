// Minimal Vulkan harness shared by the repro cases: one device, one queue, buffers with
// device addresses, compute and graphics pipelines from SPIR-V files, and a submit that
// reports device loss (with VK_EXT_device_fault data when the driver has it).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// Process exit codes, also printed as "RESULT: <name>".
enum {
    REPRO_PASS          = 0, // ran, output matches the CPU reference
    REPRO_WRONG         = 1, // ran, output differs
    REPRO_DEVICE_LOST   = 2, // VK_ERROR_DEVICE_LOST from submit or wait
    REPRO_TIMEOUT       = 3, // fence not signalled within the timeout
    REPRO_SETUP_FAILED  = 4, // a Vulkan call before the dispatch failed
    REPRO_PIPELINE_FAIL = 5, // shader module or pipeline creation failed
};

typedef struct ReproBuffer {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    void           *mapped;
    VkDeviceAddress address;
    VkDeviceSize    size;
} ReproBuffer;

typedef struct ReproImage {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint32_t       width, height;
} ReproImage;

typedef struct Repro {
    VkInstance                 instance;
    VkPhysicalDevice           physical;
    VkDevice                   device;
    VkQueue                    queue;
    uint32_t                   queue_family;
    VkCommandPool              pool;
    VkPhysicalDeviceProperties properties;
    bool                       has_device_fault;
    const char                *variant;    // --variant, default "bad"
    const char                *spv_dir;    // directory of the .spv files
    uint64_t                   timeout_ns; // --timeout seconds, default 10
    int                        repeat;     // --repeat N dispatches, default 1
    const char                *dump_isa;   // --dump-isa file, or NULL
} Repro;

// Parses the common arguments, creates the device and prints the driver details a bug
// report needs. Arguments:
//   --device <index | name substring>   default: the first AMD GPU, else device 0
//   --variant <name>                    default: bad
//   --spv-dir <dir>                     default: <exe dir>/shaders
//   --timeout <seconds>                 default: 10
//   --repeat <n>                        default: 1
//   --validation                        enables VK_LAYER_KHRONOS_validation
//   --dump-isa <file>                   writes each pipeline's statistics and the driver's
//                                       internal representations (IR, ISA) to file, through
//                                       VK_KHR_pipeline_executable_properties
// Exits with REPRO_SETUP_FAILED on failure.
void repro_init(Repro *r, int argc, char **argv, const char *case_name);

// True when the --variant argument equals name.
bool repro_variant_is(const Repro *r, const char *name);

// Host-visible, coherent (cached when the device has it) buffer with a device address;
// contents start zeroed.
ReproBuffer repro_buffer(Repro *r, VkDeviceSize size);

// Loads <spv_dir>/<file>. Exits with REPRO_PIPELINE_FAIL on failure.
VkShaderModule repro_shader(Repro *r, const char *file);

// Pipeline layout with one push-constant range over all stages and no descriptor sets.
VkPipelineLayout repro_layout(Repro *r, uint32_t push_size);

// Exits with REPRO_PIPELINE_FAIL when creation fails (a driver compiler crash kills the
// process instead; the log line before it says which pipeline was being created).
VkPipeline repro_compute_pipeline(Repro *r, VkShaderModule module, const char *entry,
                                  VkPipelineLayout layout);

VkPipeline repro_graphics_pipeline(Repro *r, VkShaderModule vs_module, const char *vs_entry,
                                   VkShaderModule fs_module, const char *fs_entry,
                                   VkPipelineLayout layout, VkFormat color_format);

// RGBA8 color target usable as attachment and copy source.
ReproImage repro_image(Repro *r, uint32_t width, uint32_t height);

VkCommandBuffer repro_begin(Repro *r);

// Records dynamic rendering into image (cleared to 0) with one full-screen triangle draw.
void repro_draw_fullscreen(Repro *r, VkCommandBuffer cmd, ReproImage *image, VkPipeline pipeline,
                           VkPipelineLayout layout, const void *push, uint32_t push_size);

// Copies image into buffer (tightly packed RGBA8), after the rendering above.
void repro_copy_image(Repro *r, VkCommandBuffer cmd, ReproImage *image, ReproBuffer *buffer);

// Ends, submits and waits. On device loss or timeout prints the fault data and exits.
void repro_submit_wait(Repro *r, VkCommandBuffer cmd);

// Prints "RESULT: <name>" and exits with code.
_Noreturn void repro_finish(Repro *r, int code);

void repro_log(const char *format, ...);

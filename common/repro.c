#include "repro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#endif

static double repro_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
#endif
}

void repro_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

_Noreturn void repro_finish(Repro *r, int code) {
    static const char *names[] = {"PASS",        "WRONG",        "DEVICE_LOST",
                                  "TIMEOUT",     "SETUP_FAILED", "PIPELINE_FAILED"};
    (void)r;
    repro_log("RESULT: %s", names[code]);
    exit(code);
}

#define CHECK(call)                                                                                \
    do {                                                                                           \
        VkResult check_result = (call);                                                            \
        if (check_result != VK_SUCCESS) {                                                          \
            repro_log("%s failed: VkResult %d (%s:%d)", #call, (int)check_result, __FILE__,        \
                      __LINE__);                                                                   \
            repro_finish(r, check_result == VK_ERROR_DEVICE_LOST ? REPRO_DEVICE_LOST               \
                                                                 : REPRO_SETUP_FAILED);            \
        }                                                                                          \
    } while (0)

static void repro_print_os(void) {
#ifdef _WIN32
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn get_version =
        ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW info = {.dwOSVersionInfoSize = sizeof(info)};
    if (get_version && get_version(&info) == 0) {
        repro_log("os: Windows %lu.%lu build %lu", info.dwMajorVersion, info.dwMinorVersion,
                  info.dwBuildNumber);
    }
#else
    struct utsname info;
    if (uname(&info) == 0) {
        repro_log("os: %s %s", info.sysname, info.release);
    }
#endif
}

static void repro_default_spv_dir(Repro *r) {
    static char dir[1024] = "shaders";
#ifdef _WIN32
    char  exe[1024];
    DWORD length = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (length > 0 && length < sizeof(exe)) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = 0;
            snprintf(dir, sizeof(dir), "%s\\shaders", exe);
        }
    }
#else
    char    exe[1024];
    ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length > 0) {
        exe[length] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(dir, sizeof(dir), "%s/shaders", exe);
        }
    }
#endif
    r->spv_dir = dir;
}

static bool repro_has_extension(VkPhysicalDevice physical, const char *name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL);
    VkExtensionProperties *list = calloc(count, sizeof(*list));
    vkEnumerateDeviceExtensionProperties(physical, NULL, &count, list);
    bool found = false;
    for (uint32_t i = 0; i < count && !found; ++i) {
        found = strcmp(list[i].extensionName, name) == 0;
    }
    free(list);
    return found;
}

static VkPhysicalDevice repro_pick(Repro *r, const char *wanted) {
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(r->instance, &count, NULL));
    VkPhysicalDevice *list = calloc(count, sizeof(*list));
    CHECK(vkEnumeratePhysicalDevices(r->instance, &count, list));
    int chosen = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        repro_log("gpu %u: %s (vendor 0x%04x, type %d)", i, p.deviceName, p.vendorID,
                  (int)p.deviceType);
        if (chosen >= 0) {
            continue;
        }
        if (wanted) {
            char *end   = NULL;
            long  index = strtol(wanted, &end, 10);
            if ((end && *end == 0 && index == (long)i) || strstr(p.deviceName, wanted)) {
                chosen = (int)i;
            }
        } else if (p.vendorID == 0x1002 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            chosen = (int)i;
        }
    }
    if (chosen < 0 && !wanted && count > 0) {
        chosen = 0;
    }
    if (chosen < 0) {
        repro_log("no GPU matches --device %s", wanted);
        repro_finish(r, REPRO_SETUP_FAILED);
    }
    VkPhysicalDevice physical = list[chosen];
    free(list);
    return physical;
}

void repro_init(Repro *r, int argc, char **argv, const char *case_name) {
    memset(r, 0, sizeof(*r));
    r->variant    = "bad";
    r->timeout_ns = 10ull * 1000000000ull;
    r->repeat     = 1;
    repro_default_spv_dir(r);
    const char *wanted     = getenv("REPRO_DEVICE");
    bool        validation = false;
    for (int i = 1; i < argc; ++i) {
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--device") == 0 && next) {
            wanted = next, ++i;
        } else if (strcmp(argv[i], "--variant") == 0 && next) {
            r->variant = next, ++i;
        } else if (strcmp(argv[i], "--spv-dir") == 0 && next) {
            r->spv_dir = next, ++i;
        } else if (strcmp(argv[i], "--timeout") == 0 && next) {
            r->timeout_ns = (uint64_t)(atof(next) * 1e9), ++i;
        } else if (strcmp(argv[i], "--repeat") == 0 && next) {
            r->repeat = atoi(next), ++i;
        } else if (strcmp(argv[i], "--validation") == 0) {
            validation = true;
        } else if (strcmp(argv[i], "--dump-isa") == 0 && next) {
            r->dump_isa = next, ++i;
        } else {
            repro_log("unknown argument %s", argv[i]);
            repro_finish(r, REPRO_SETUP_FAILED);
        }
    }
    repro_log("case: %s, variant: %s", case_name, r->variant);
    repro_print_os();

    const char *layers[]  = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = case_name,
        .apiVersion       = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instance_info = {
        .sType               = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo    = &app,
        .enabledLayerCount   = validation ? 1u : 0u,
        .ppEnabledLayerNames = layers,
    };
    CHECK(vkCreateInstance(&instance_info, NULL, &r->instance));
    r->physical = repro_pick(r, wanted);

    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties subgroup = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &driver};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(r->physical, &properties);
    r->properties  = properties.properties;
    uint32_t dv    = r->properties.driverVersion;
    uint32_t api   = r->properties.apiVersion;
    repro_log("device: %s (0x%04x:0x%04x)", r->properties.deviceName, r->properties.vendorID,
              r->properties.deviceID);
    if (r->properties.vendorID == 0x10de) { // NVIDIA packs 10.8.8.6 bits
        repro_log("driver: %s / %s (driverVersion 0x%08x = %u.%u)", driver.driverName,
                  driver.driverInfo, dv, dv >> 22, (dv >> 14) & 0xff);
    } else {
        repro_log("driver: %s / %s (driverVersion 0x%08x = %u.%u.%u)", driver.driverName,
                  driver.driverInfo, dv, VK_API_VERSION_MAJOR(dv), VK_API_VERSION_MINOR(dv),
                  VK_API_VERSION_PATCH(dv));
    }
    repro_log("vulkan api: %u.%u.%u, conformance %u.%u.%u.%u, subgroup size %u",
              VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
              driver.conformanceVersion.major, driver.conformanceVersion.minor,
              driver.conformanceVersion.subminor, driver.conformanceVersion.patch,
              subgroup.subgroupSize);

    // Features the cases use; each is checked, then enabled alone.
    VkPhysicalDeviceFaultFeaturesEXT fault = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
    VkPhysicalDeviceVulkan11Features v11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
    r->has_device_fault = repro_has_extension(r->physical, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (r->has_device_fault) {
        v13.pNext = &fault;
    }
    VkPhysicalDeviceFeatures2 supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
    vkGetPhysicalDeviceFeatures2(r->physical, &supported);
    if (!supported.features.shaderInt64 || !supported.features.shaderInt16
        || !v11.storageBuffer16BitAccess || !v12.bufferDeviceAddress || !v12.scalarBlockLayout || !v12.shaderFloat16
        || !v13.synchronization2 || !v13.dynamicRendering) {
        repro_log("device lacks a required feature (int64, int16, 16-bit storage, buffer "
                  "device address, float16, synchronization2, dynamic rendering)");
        repro_finish(r, REPRO_SETUP_FAILED);
    }
    r->has_device_fault = r->has_device_fault && fault.deviceFault;
    if (r->dump_isa
        && !repro_has_extension(r->physical, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)) {
        repro_log("--dump-isa: the driver lacks %s",
                  VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        repro_finish(r, REPRO_SETUP_FAILED);
    }
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
        .pipelineExecutableInfo = VK_TRUE,
    };
    VkPhysicalDeviceFaultFeaturesEXT fault_on = {
        .sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
        .pNext       = r->dump_isa ? &executable_on : NULL,
        .deviceFault = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features v13_on = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = r->has_device_fault ? (void *)&fault_on
                            : r->dump_isa       ? (void *)&executable_on
                                                : NULL,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features v12_on = {
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &v13_on,
        .bufferDeviceAddress = VK_TRUE,
        .scalarBlockLayout   = VK_TRUE, // Slang modules (05) use scalar layout
        .shaderFloat16       = VK_TRUE,
    };
    VkPhysicalDeviceVulkan11Features v11_on = {
        .sType                    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext                    = &v12_on,
        .storageBuffer16BitAccess = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 enabled = {
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = &v11_on,
        .features = {.shaderInt64 = VK_TRUE, .shaderInt16 = VK_TRUE},
    };

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(r->physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(r->physical, &family_count, families);
    r->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count && r->queue_family == UINT32_MAX; ++i) {
        VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & want) == want) {
            r->queue_family = i;
        }
    }
    free(families);

    float                   priority   = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = r->queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[2];
    uint32_t    extension_count = 0;
    if (r->has_device_fault) {
        extensions[extension_count++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    }
    if (r->dump_isa) {
        extensions[extension_count++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    }
    VkDeviceCreateInfo device_info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &enabled,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queue_info,
        .enabledExtensionCount   = extension_count,
        .ppEnabledExtensionNames = extensions,
    };
    CHECK(vkCreateDevice(r->physical, &device_info, NULL, &r->device));
    vkGetDeviceQueue(r->device, r->queue_family, 0, &r->queue);
    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = r->queue_family,
    };
    CHECK(vkCreateCommandPool(r->device, &pool_info, NULL, &r->pool));
    repro_log("VK_EXT_device_fault: %s", r->has_device_fault ? "enabled" : "not supported");
}

bool repro_variant_is(const Repro *r, const char *name) {
    return strcmp(r->variant, name) == 0;
}

static uint32_t repro_memory_type(Repro *r, uint32_t bits, VkMemoryPropertyFlags want,
                                  VkMemoryPropertyFlags fallback) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(r->physical, &memory);
    for (int pass = 0; pass < 2; ++pass) {
        VkMemoryPropertyFlags flags = pass == 0 ? want : fallback;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
    }
    repro_log("no memory type for bits 0x%x", bits);
    repro_finish(r, REPRO_SETUP_FAILED);
}

ReproBuffer repro_buffer(Repro *r, VkDeviceSize size) {
    ReproBuffer        b    = {.size = size};
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    CHECK(vkCreateBuffer(r->device, &info, NULL, &b.buffer));
    VkMemoryRequirements need;
    vkGetBufferMemoryRequirements(r->device, b.buffer, &need);
    VkMemoryPropertyFlags host =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo allocate = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &flags,
        .allocationSize  = need.size,
        // Cached system memory: the CPU reference reads inputs and outputs back, and reads
        // from write-combined device-local memory crawl.
        .memoryTypeIndex = repro_memory_type(r, need.memoryTypeBits,
                                             host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, host),
    };
    CHECK(vkAllocateMemory(r->device, &allocate, NULL, &b.memory));
    CHECK(vkBindBufferMemory(r->device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(r->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    memset(b.mapped, 0, (size_t)size);
    VkBufferDeviceAddressInfo address = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = b.buffer,
    };
    b.address = vkGetBufferDeviceAddress(r->device, &address);
    return b;
}

VkShaderModule repro_shader(Repro *r, const char *file) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", r->spv_dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        repro_log("cannot open %s", path);
        repro_finish(r, REPRO_PIPELINE_FAIL);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)size);
    fread(code, 1, (size_t)size, f);
    fclose(f);
    VkShaderModuleCreateInfo info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size,
        .pCode    = code,
    };
    VkShaderModule module;
    VkResult       result = vkCreateShaderModule(r->device, &info, NULL, &module);
    free(code);
    if (result != VK_SUCCESS) {
        repro_log("vkCreateShaderModule(%s): VkResult %d", path, (int)result);
        repro_finish(r, REPRO_PIPELINE_FAIL);
    }
    repro_log("shader: %s (%ld bytes)", path, size);
    return module;
}

VkPipelineLayout repro_layout(Repro *r, uint32_t push_size) {
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_ALL,
        .size       = push_size,
    };
    VkPipelineLayoutCreateInfo info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(r->device, &info, NULL, &layout));
    return layout;
}

static VkPipelineCreateFlags repro_pipeline_flags(const Repro *r) {
    return r->dump_isa ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR
                             | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR
                       : 0;
}

// Appends every executable's statistics and text representations of pipeline to --dump-isa.
static void repro_dump_pipeline(Repro *r, VkPipeline pipeline) {
    if (!r->dump_isa) {
        return;
    }
    PFN_vkGetPipelineExecutablePropertiesKHR get_executables =
        (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(
            r->device, "vkGetPipelineExecutablePropertiesKHR");
    PFN_vkGetPipelineExecutableStatisticsKHR get_statistics =
        (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(
            r->device, "vkGetPipelineExecutableStatisticsKHR");
    PFN_vkGetPipelineExecutableInternalRepresentationsKHR get_representations =
        (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(
            r->device, "vkGetPipelineExecutableInternalRepresentationsKHR");
    FILE *out = fopen(r->dump_isa, "a");
    if (!out) {
        repro_log("--dump-isa: cannot open %s", r->dump_isa);
        return;
    }
    VkPipelineInfoKHR pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
    uint32_t executable_count = 0;
    get_executables(r->device, &pipeline_info, &executable_count, NULL);
    VkPipelineExecutablePropertiesKHR *executables =
        calloc(executable_count, sizeof(*executables));
    for (uint32_t i = 0; i < executable_count; ++i) {
        executables[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    }
    get_executables(r->device, &pipeline_info, &executable_count, executables);
    for (uint32_t e = 0; e < executable_count; ++e) {
        fprintf(out, "==== executable %u: %s (%s), subgroup size %u\n", e, executables[e].name,
                executables[e].description, executables[e].subgroupSize);
        VkPipelineExecutableInfoKHR executable = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
            .pipeline        = pipeline,
            .executableIndex = e,
        };
        uint32_t statistic_count = 0;
        get_statistics(r->device, &executable, &statistic_count, NULL);
        VkPipelineExecutableStatisticKHR *statistics =
            calloc(statistic_count, sizeof(*statistics));
        for (uint32_t i = 0; i < statistic_count; ++i) {
            statistics[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        }
        get_statistics(r->device, &executable, &statistic_count, statistics);
        for (uint32_t i = 0; i < statistic_count; ++i) {
            VkPipelineExecutableStatisticKHR *s = &statistics[i];
            switch (s->format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                fprintf(out, "%s: %s\n", s->name, s->value.b32 ? "true" : "false");
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                fprintf(out, "%s: %lld\n", s->name, (long long)s->value.i64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                fprintf(out, "%s: %llu\n", s->name, (unsigned long long)s->value.u64);
                break;
            default:
                fprintf(out, "%s: %g\n", s->name, s->value.f64);
                break;
            }
        }
        free(statistics);
        uint32_t representation_count = 0;
        get_representations(r->device, &executable, &representation_count, NULL);
        VkPipelineExecutableInternalRepresentationKHR *representations =
            calloc(representation_count, sizeof(*representations));
        for (uint32_t i = 0; i < representation_count; ++i) {
            representations[i].sType =
                VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
        }
        get_representations(r->device, &executable, &representation_count, representations);
        for (uint32_t i = 0; i < representation_count; ++i) {
            representations[i].pData = calloc(1, representations[i].dataSize + 1);
        }
        get_representations(r->device, &executable, &representation_count, representations);
        for (uint32_t i = 0; i < representation_count; ++i) {
            VkPipelineExecutableInternalRepresentationKHR *rep = &representations[i];
            fprintf(out, "---- %s (%s)\n", rep->name, rep->description);
            if (rep->isText) {
                fputs((const char *)rep->pData, out);
                fputc('\n', out);
            } else {
                fprintf(out, "<binary, %zu bytes>\n", rep->dataSize);
            }
            free(rep->pData);
        }
        free(representations);
    }
    free(executables);
    fclose(out);
    repro_log("dumped %u executables to %s", executable_count, r->dump_isa);
}

VkPipeline repro_compute_pipeline(Repro *r, VkShaderModule module, const char *entry,
                                  VkPipelineLayout layout) {
    repro_log("creating compute pipeline %s ...", entry);
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = repro_pipeline_flags(r),
        .stage =
            {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName  = entry,
            },
        .layout = layout,
    };
    VkPipeline pipeline;
    double     start  = repro_seconds();
    VkResult   result = vkCreateComputePipelines(r->device, NULL, 1, &info, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        repro_log("vkCreateComputePipelines: VkResult %d", (int)result);
        repro_finish(r, REPRO_PIPELINE_FAIL);
    }
    repro_log("created in %.1f ms", (repro_seconds() - start) * 1000.0);
    repro_dump_pipeline(r, pipeline);
    return pipeline;
}

VkPipeline repro_graphics_pipeline(Repro *r, VkShaderModule vs_module, const char *vs_entry,
                                   VkShaderModule fs_module, const char *fs_entry,
                                   VkPipelineLayout layout, VkFormat color_format) {
    repro_log("creating graphics pipeline %s + %s ...", vs_entry, fs_entry);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vs_module,
            .pName  = vs_entry,
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fs_module,
            .pName  = fs_entry,
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo blend            = {
                   .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                   .attachmentCount = 1,
                   .pAttachments    = &blend_attachment,
    };
    VkDynamicState                   dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                         VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic          = {
                 .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                 .dynamicStateCount = 2,
                 .pDynamicStates    = dynamic_states,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &color_format,
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .flags               = repro_pipeline_flags(r),
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = layout,
    };
    VkPipeline pipeline;
    double     start  = repro_seconds();
    VkResult   result = vkCreateGraphicsPipelines(r->device, NULL, 1, &info, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        repro_log("vkCreateGraphicsPipelines: VkResult %d", (int)result);
        repro_finish(r, REPRO_PIPELINE_FAIL);
    }
    repro_log("created in %.1f ms", (repro_seconds() - start) * 1000.0);
    repro_dump_pipeline(r, pipeline);
    return pipeline;
}

ReproImage repro_image(Repro *r, uint32_t width, uint32_t height) {
    ReproImage        image = {.width = width, .height = height};
    VkImageCreateInfo info  = {
         .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType   = VK_IMAGE_TYPE_2D,
         .format      = VK_FORMAT_R8G8B8A8_UNORM,
         .extent      = {width, height, 1},
         .mipLevels   = 1,
         .arrayLayers = 1,
         .samples     = VK_SAMPLE_COUNT_1_BIT,
         .tiling      = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    CHECK(vkCreateImage(r->device, &info, NULL, &image.image));
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(r->device, image.image, &need);
    VkMemoryAllocateInfo allocate = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = need.size,
        .memoryTypeIndex = repro_memory_type(r, need.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0),
    };
    CHECK(vkAllocateMemory(r->device, &allocate, NULL, &image.memory));
    CHECK(vkBindImageMemory(r->device, image.image, image.memory, 0));
    VkImageViewCreateInfo view = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    CHECK(vkCreateImageView(r->device, &view, NULL, &image.view));
    return image;
}

VkCommandBuffer repro_begin(Repro *r) {
    VkCommandBufferAllocateInfo allocate = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = r->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(r->device, &allocate, &cmd));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
}

static void repro_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                                VkImageLayout to, VkPipelineStageFlags2 src_stage,
                                VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                                VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask     = src_stage,
        .srcAccessMask    = src_access,
        .dstStageMask     = dst_stage,
        .dstAccessMask    = dst_access,
        .oldLayout        = from,
        .newLayout        = to,
        .image            = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dependency = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void repro_draw_fullscreen(Repro *r, VkCommandBuffer cmd, ReproImage *image, VkPipeline pipeline,
                           VkPipelineLayout layout, const void *push, uint32_t push_size) {
    (void)r;
    repro_image_barrier(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = image->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo rendering = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, {image->width, image->height}},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport = {0, 0, (float)image->width, (float)image->height, 0, 1};
    VkRect2D   scissor  = {{0, 0}, {image->width, image->height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, push_size, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void repro_copy_image(Repro *r, VkCommandBuffer cmd, ReproImage *image, ReproBuffer *buffer) {
    (void)r;
    repro_image_barrier(cmd, image->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent      = {image->width, image->height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer->buffer, 1, &region);
}

static void repro_print_fault(Repro *r) {
    if (!r->has_device_fault) {
        repro_log("(no VK_EXT_device_fault data)");
        return;
    }
    PFN_vkGetDeviceFaultInfoEXT get_fault =
        (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(r->device, "vkGetDeviceFaultInfoEXT");
    VkDeviceFaultCountsEXT counts = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (!get_fault || get_fault(r->device, &counts, NULL) != VK_SUCCESS) {
        repro_log("vkGetDeviceFaultInfoEXT failed");
        return;
    }
    VkDeviceFaultAddressInfoEXT *addresses =
        calloc(counts.addressInfoCount + 1, sizeof(*addresses));
    VkDeviceFaultVendorInfoEXT *vendors = calloc(counts.vendorInfoCount + 1, sizeof(*vendors));
    counts.vendorBinarySize             = 0;
    VkDeviceFaultInfoEXT info           = {
                  .sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                  .pAddressInfos     = addresses,
                  .pVendorInfos      = vendors,
    };
    VkResult result = get_fault(r->device, &counts, &info);
    repro_log("device fault (VkResult %d): %s", (int)result, info.description);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        repro_log("  address type %d: 0x%016llx (precision %llu)", (int)addresses[i].addressType,
                  (unsigned long long)addresses[i].reportedAddress,
                  (unsigned long long)addresses[i].addressPrecision);
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        repro_log("  vendor: %s (code 0x%llx, data 0x%llx)", vendors[i].description,
                  (unsigned long long)vendors[i].vendorFaultCode,
                  (unsigned long long)vendors[i].vendorFaultData);
    }
    free(addresses);
    free(vendors);
}

void repro_submit_wait(Repro *r, VkCommandBuffer cmd) {
    CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence           fence;
    CHECK(vkCreateFence(r->device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    double   start  = repro_seconds();
    VkResult result = vkQueueSubmit(r->queue, 1, &submit, fence);
    if (result == VK_SUCCESS) {
        result = vkWaitForFences(r->device, 1, &fence, VK_TRUE, r->timeout_ns);
    }
    double elapsed = (repro_seconds() - start) * 1000.0;
    if (result == VK_ERROR_DEVICE_LOST) {
        repro_log("device lost after %.1f ms", elapsed);
        repro_print_fault(r);
        repro_finish(r, REPRO_DEVICE_LOST);
    }
    if (result == VK_TIMEOUT) {
        repro_log("fence not signalled after %.1f ms", elapsed);
        repro_print_fault(r);
        repro_finish(r, REPRO_TIMEOUT);
    }
    if (result != VK_SUCCESS) {
        repro_log("submit/wait: VkResult %d", (int)result);
        repro_finish(r, REPRO_SETUP_FAILED);
    }
    repro_log("gpu work done in %.2f ms (submit to fence)", elapsed);
    vkDestroyFence(r->device, fence, NULL);
    vkFreeCommandBuffers(r->device, r->pool, 1, &cmd);
}

// The PC's FSR 3 frame generation (runtime/nvn/vulkan/framegen.cpp, AMD FidelityFX SDK) and FSR 3.1 / 4 API
// (fsr_api.cpp, AMD's signed DLLs) aren't built for the PS4: its GCN GPU has neither the time nor the DLLs for them.
// Their entry points say "not available", so the settings fall back as on a PC without them.
#include <stdbool.h>
#include <vulkan/vulkan.h>
#include "vulkan/framegen.h"
#include "vulkan/fsr_api.h"

bool fg_dispatch(VkCommandBuffer cb, const FgConfig *c, const FgFrame *f) { (void)cb, (void)c, (void)f; return false; }
void fg_destroy(void) {}
FgVk g_fg_vk;
bool fsr_api_present(void) { return false; }
void fsr_api_probe(VkDevice dev, VkPhysicalDevice phys) { (void)dev, (void)phys; }
bool fsr_api_has(int major) { (void)major; return false; }
bool fsr_api_usable(int major) { (void)major; return false; }
bool fsr_api_dispatch(const Fsr3Dispatch *d, int major) { (void)d, (void)major; return false; }
void fsr_api_destroy(void) {}
bool fsr4_available(void) { return false; }
bool fsr3_api_wanted(void) { return false; }

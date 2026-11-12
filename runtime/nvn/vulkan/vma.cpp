// Vulkan Memory Allocator implementation (header-only library, MSYS2 package vulkan-memory-allocator).
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#ifdef __ORBIS__
#include <volk.h>  // (PS4: the Vulkan functions are volk's pointers; vk.h)
#endif
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

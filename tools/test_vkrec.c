// Checks runtime/nvn/vulkan/vkrec.c's ring (the PS4's Vulkan recording thread) without a GPU: one thread puts millions
// of commands of random sizes (wraps, a full ring, syncs, plain submits) and overwrites what they pointed to right away;
// the "Vulkan" calls below, run on vkrec's thread, check each one arrives once, in order, with its own data.
//   clang -O1 -g -fsanitize=thread -Iruntime -Iruntime/nvn -Ithird_party -Ithird_party/vma tools/test_vkrec.c \
//       runtime/nvn/vulkan/vkrec.c -o build/test_vkrec && build/test_vkrec [millions] [seed]
// (also with -fsanitize=address,undefined instead of thread)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VKREC_IMPL  // (vk.h: the real vkCmd* names, defined below, not vkrec's)
#include "nvn/vulkan/vk.h"

static uint64_t produced;  // (producer thread)
static uint64_t consumed;  // (vkrec's thread; the producer reads it only after vr_sync)

#define CHECK(c)                                                                                                \
    do {                                                                                                        \
        if (!(c)) {                                                                                             \
            fprintf(stderr, "FAIL %s:%d at command %llu\n", __FILE__, __LINE__, (unsigned long long)consumed); \
            abort();                                                                                            \
        }                                                                                                       \
    } while (0)

static uint32_t pat(uint64_t seq, uint32_t i) { return (uint32_t)(seq * 2654435761u) ^ (i * 40503u); }
static VkCommandBuffer cb_of(uint64_t seq) { return (VkCommandBuffer)(uintptr_t)(seq * 16 + 16); }
// Push constant sizes: mostly small, 1 in 64 up to 64 KB (the ring is 8 MB: big entries fill it and wrap often)
static uint32_t pc_size(uint64_t seq) { return seq % 64 == 7 ? 4 * (1 + (uint32_t)(seq * 7919 % 16384)) : 4 * (1 + (uint32_t)(seq % 64)); }

// ---- the "Vulkan" calls vkrec makes (its thread)
static void slow_now_and_then(void) {  // (so the producer finds the ring full)
    if (consumed % 200000 == 199999) usleep(3000);
}
void vkCmdDraw(VkCommandBuffer cb, uint32_t nv, uint32_t ni, uint32_t fv, uint32_t fi) {
    CHECK(cb == cb_of(consumed) && nv == (uint32_t)consumed && ni == 1 && fv == pat(consumed, 0) && fi == 3);
    consumed++;
    slow_now_and_then();
}
void vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size,
                        const void *data) {
    (void)l;
    CHECK(cb == cb_of(consumed) && off == (uint32_t)consumed && st == 1 && size == pc_size(consumed));
    const uint32_t *w = data;
    for (uint32_t i = 0; i < size / 4; i++) CHECK(w[i] == pat(consumed, i));
    consumed++;
    slow_now_and_then();
}
void vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    CHECK(cb == cb_of(consumed) && first == (uint32_t)consumed && n == 1 + consumed % 16);
    for (uint32_t i = 0; i < n; i++) CHECK(v[i].x == (float)i && v[i].width == (float)(consumed % 1000));
    consumed++;
    slow_now_and_then();
}
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    CHECK(cb == cb_of(consumed) && bi->flags == (consumed & 7) && !bi->pNext && !bi->pInheritanceInfo);
    consumed++;
    return VK_SUCCESS;
}
VkResult vkEndCommandBuffer(VkCommandBuffer cb) {
    CHECK(cb == cb_of(consumed));
    consumed++;
    return VK_SUCCESS;
}
VkResult vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f) {
    CHECK(q == (VkQueue)(uintptr_t)cb_of(consumed) && n == 1 && !f && si->commandBufferInfoCount == 1 + consumed % 8);
    for (uint32_t i = 0; i < si->commandBufferInfoCount; i++) CHECK(si->pCommandBufferInfos[i].commandBuffer == cb_of(consumed + i));
    CHECK(si->signalSemaphoreInfoCount == consumed % 2);
    if (si->signalSemaphoreInfoCount) CHECK(si->pSignalSemaphoreInfos[0].value == consumed);
    consumed++;
    return VK_SUCCESS;
}
void rt_fatal_trace(void) { abort(); }
void vk_devlost_dump(void) { abort(); }
__attribute__((used)) void unused(void) {  // (used: only the aliases below name it)
    fprintf(stderr, "FAIL: vkrec called a Vulkan function this test never records\n");
    abort();
}
// The rest of what vkrec.c calls, never used here (ELF aliases: no C prototype to match)
#define ALIAS(f) __asm__(".globl " #f "\n.set " #f ", unused");
ALIAS(vkAllocateCommandBuffers) ALIAS(vkCmdBeginQuery) ALIAS(vkCmdBeginRendering) ALIAS(vkCmdBindDescriptorSets)
ALIAS(vkCmdBindIndexBuffer) ALIAS(vkCmdBindPipeline) ALIAS(vkCmdBindVertexBuffers) ALIAS(vkCmdBlitImage)
ALIAS(vkCmdClearAttachments) ALIAS(vkCmdClearColorImage) ALIAS(vkCmdClearDepthStencilImage) ALIAS(vkCmdCopyBuffer)
ALIAS(vkCmdCopyBufferToImage) ALIAS(vkCmdCopyImage) ALIAS(vkCmdCopyImageToBuffer) ALIAS(vkCmdDispatch)
ALIAS(vkCmdDrawIndexed) ALIAS(vkCmdEndQuery) ALIAS(vkCmdEndRendering) ALIAS(vkCmdPipelineBarrier2)
ALIAS(vkCmdResetQueryPool) ALIAS(vkCmdResolveImage) ALIAS(vkCmdSetBlendConstants) ALIAS(vkCmdSetCullMode)
ALIAS(vkCmdSetDepthBias) ALIAS(vkCmdSetDepthBiasEnable) ALIAS(vkCmdSetDepthCompareOp) ALIAS(vkCmdSetDepthTestEnable)
ALIAS(vkCmdSetDepthWriteEnable) ALIAS(vkCmdSetFrontFace) ALIAS(vkCmdSetLineWidth) ALIAS(vkCmdSetPrimitiveRestartEnable)
ALIAS(vkCmdSetPrimitiveTopology) ALIAS(vkCmdSetScissor) ALIAS(vkCmdSetStencilCompareMask) ALIAS(vkCmdSetStencilOp)
ALIAS(vkCmdSetStencilReference) ALIAS(vkCmdSetStencilTestEnable) ALIAS(vkCmdSetStencilWriteMask)
ALIAS(vkCmdWriteTimestamp2) ALIAS(vkDeviceWaitIdle) ALIAS(vkQueueWaitIdle) ALIAS(vkUpdateDescriptorSets)

// ---- the producer (the queue thread's side)
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}

int main(int argc, char **argv) {
    uint64_t n = (argc > 1 ? strtoull(argv[1], NULL, 10) : 4) * 1000000ull;
    if (argc > 2) rng = strtoull(argv[2], NULL, 0) | 1;
    setenv("SAVAGEWIND_VK_REC", "1", 1);
    if (!vr_enabled()) { fprintf(stderr, "FAIL: no recording thread\n"); return 1; }
    static uint32_t buf[65536];
    static VkViewport vp[16];
    uint64_t syncs = 0;
    while (produced < n) {
        uint64_t seq = produced, r = rnd() % 100;
        if (r < 40) {
            vr_CmdDraw(cb_of(seq), (uint32_t)seq, 1, pat(seq, 0), 3);
        } else if (r < 75) {
            uint32_t size = pc_size(seq);
            for (uint32_t i = 0; i < size / 4; i++) buf[i] = pat(seq, i);
            vr_CmdPushConstants(cb_of(seq), VK_NULL_HANDLE, 1, (uint32_t)seq, size, buf);
            memset(buf, 0xA5, size);  // (vkrec must have its own copy)
        } else if (r < 90) {
            uint32_t k = 1 + seq % 16;
            for (uint32_t i = 0; i < k; i++) vp[i] = (VkViewport){(float)i, 0, (float)(seq % 1000), 1, 0, 1};
            vr_CmdSetViewport(cb_of(seq), (uint32_t)seq, k, vp);
            memset(vp, 0x5A, sizeof vp);
        } else if (r < 94) {
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = (VkCommandBufferUsageFlags)(seq & 7)};
            CHECK(vr_BeginCommandBuffer(cb_of(seq), &bi) == VK_SUCCESS);
        } else if (r < 97) {
            CHECK(vr_EndCommandBuffer(cb_of(seq)) == VK_SUCCESS);
        } else {
            VkCommandBufferSubmitInfo cbs[8];
            uint32_t k = 1 + seq % 8;
            for (uint32_t i = 0; i < k; i++) cbs[i] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb_of(seq + i)};
            VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .value = seq};
            VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = k, .pCommandBufferInfos = cbs,
                                .signalSemaphoreInfoCount = (uint32_t)(seq % 2), .pSignalSemaphoreInfos = &sig};
            CHECK(vr_QueueSubmit2((VkQueue)(uintptr_t)cb_of(seq), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
            memset(cbs, 0, sizeof cbs);
            memset(&sig, 0, sizeof sig);
        }
        produced++;
        if (rnd() % 997 == 0) {
            vr_sync();
            CHECK(consumed == produced);
            syncs++;
        }
    }
    vr_sync();
    CHECK(consumed == produced);
    printf("PASS: %llu commands, %llu syncs\n", (unsigned long long)produced, (unsigned long long)syncs);
    return 0;
}

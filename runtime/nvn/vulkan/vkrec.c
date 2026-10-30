// Command recording on a thread of its own. On the PS4 the queue thread (objects.c) was the frame's bottleneck: ~80%
// busy at ~19 fps in the open world, a third of it in RADV's vkCmd* (state emission, draws, binds). Here the backend's
// vkCmd* calls (vk.h turns them into vr_Cmd*) go into a ring with copies of what they point to, and this thread makes
// the real calls in the same order, while the queue thread goes on translating. Everything that needs the commands
// recorded first (vkBeginCommandBuffer, vkEndCommandBuffer, submits, command buffer allocation: the pool is not
// thread-safe) waits for the ring to empty (vr_sync). A struct this doesn't know how to copy (an unknown pNext) also
// waits and makes the call itself.
// The objects named by the commands (pipelines, sets, views, buffers) outlive their recording as before: they are
// destroyed after the submission that used them completes, and submitting waits for the ring.
// PS4 by default; SAVAGEWIND_VK_REC=0 / 1 turns it off / on (the PC: off by default).
#define VKREC_IMPL
#include "vk.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ORBIS__
void ps4_profile_thread(const char *name);  // (platform/ps4.c) sampled as "vkrec"
#endif

#define RING (8u << 20)
enum {
    OP_WRAP, OP_BindPipeline, OP_SetViewport, OP_SetScissor, OP_BindDescriptorSets, OP_PipelineBarrier2,
    OP_CopyBufferToImage, OP_CopyImageToBuffer, OP_EndRendering, OP_Draw, OP_BeginRendering, OP_PushConstants,
    OP_WriteTimestamp2, OP_SetStencilWriteMask, OP_SetStencilTestEnable, OP_SetStencilReference, OP_SetStencilOp,
    OP_SetDepthWriteEnable, OP_SetDepthTestEnable, OP_ResetQueryPool, OP_BlitImage, OP_SetStencilCompareMask,
    OP_SetPrimitiveTopology, OP_SetPrimitiveRestartEnable, OP_SetLineWidth, OP_SetFrontFace, OP_SetDepthCompareOp,
    OP_SetDepthBiasEnable, OP_SetDepthBias, OP_SetCullMode, OP_SetBlendConstants, OP_CopyImage, OP_ClearColorImage,
    OP_ClearAttachments, OP_BindVertexBuffers, OP_ResolveImage, OP_EndQuery, OP_DrawIndexed, OP_Dispatch,
    OP_ClearDepthStencilImage, OP_BindIndexBuffer, OP_BeginQuery, OP_CopyBuffer, OP_BeginCommandBuffer,
    OP_EndCommandBuffer, OP_QueueSubmit2,
};
typedef struct { uint32_t op, size; } Hdr;  // size: the whole entry, a multiple of 16

static uint8_t *ring;
static _Atomic uint64_t r_head, r_tail;  // bytes put / run (never wrap: the position is % RING)
static uint64_t put_at;                  // (producer) the entry being written ends here once committed
static atomic_bool r_sleeping, r_waiting;
static pthread_mutex_t r_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_work = PTHREAD_COND_INITIALIZER, r_done = PTHREAD_COND_INITIALIZER;
static int r_on = -1;

static void *rec_thread(void *arg);
static _Atomic uint64_t r_slept_at;  // (consumer) where it caught up when it went to sleep
static void wake(void) {  // (the consumer sleeps once it has caught up and found nothing for a while)
    if (!atomic_load(&r_sleeping)) return;
    pthread_mutex_lock(&r_lock);
    pthread_cond_signal(&r_work);
    pthread_mutex_unlock(&r_lock);
}

bool vr_enabled(void) {
    if (r_on < 0) {
#ifdef __ORBIS__
        r_on = 1;
#else
        r_on = 0;
#endif
        if (getenv("SAVAGEWIND_VK_REC")) r_on = atoi(getenv("SAVAGEWIND_VK_REC")) != 0;
        if (r_on) {
            ring = aligned_alloc(64, RING);
            pthread_t t;
            if (!ring || pthread_create(&t, NULL, rec_thread, NULL) != 0) r_on = 0;
        }
        LOG("[vk] commands recorded %s\n", r_on ? "on a thread of their own" : "by the translating thread");
    }
    return r_on;
}

static inline void cpu_relax(void) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// Waits until every command put has been made (and returns at once when recording is not threaded)
void vr_sync(void) {
    if (r_on <= 0) return;
    uint64_t h = atomic_load(&r_head);
    wake();
    for (int i = 0; i < 2000; i++) {
        if (atomic_load_explicit(&r_tail, memory_order_acquire) == h) return;
        cpu_relax();
    }
    pthread_mutex_lock(&r_lock);
    atomic_store(&r_waiting, true);
    while (atomic_load(&r_tail) != h) pthread_cond_wait(&r_done, &r_lock);
    atomic_store(&r_waiting, false);
    pthread_mutex_unlock(&r_lock);
}

static void wait_room(uint64_t need) {  // until the consumer is no more than RING - need behind
    while (put_at + need - atomic_load_explicit(&r_tail, memory_order_acquire) > RING) {
        wake();
        pthread_mutex_lock(&r_lock);
        atomic_store(&r_waiting, true);
        if (put_at + need - atomic_load(&r_tail) > RING) pthread_cond_wait(&r_done, &r_lock);
        atomic_store(&r_waiting, false);
        pthread_mutex_unlock(&r_lock);
    }
}

// An entry of n bytes after its header (written by the caller, then rec_commit)
static void *rec_begin(uint32_t op, size_t n) {
    uint32_t size = (uint32_t)((sizeof(Hdr) + n + 15) & ~(size_t)15);
    put_at = atomic_load_explicit(&r_head, memory_order_relaxed);
    uint64_t pos = put_at % RING;
    if (pos + size > RING) {  // (no room before the end: a wrap marker, then from the start)
        wait_room(RING - pos);
        *(Hdr *)(ring + pos) = (Hdr){OP_WRAP, (uint32_t)(RING - pos)};
        put_at += RING - pos;
        pos = 0;
    }
    wait_room(size);
    Hdr *h = (Hdr *)(ring + pos);
    *h = (Hdr){op, size};
    put_at += size;
    return h + 1;
}
// A sleeping consumer is woken once WAKE_BYTES are waiting (or by vr_sync / a full ring): woken for each command,
// it spun between them, ~half of its time on the PS4 (a core the game's threads could use).
#define WAKE_BYTES (16u << 10)
static void rec_commit(void) {
    atomic_store(&r_head, put_at);  // (seq_cst: against the consumer's going to sleep)
    if (atomic_load(&r_sleeping) && put_at - atomic_load_explicit(&r_slept_at, memory_order_relaxed) >= WAKE_BYTES) wake();
}

// Copies of what the commands point to go after their arguments in the entry: a cursor over that space
typedef struct { uint8_t *p; } Cur;
static void *cur_put(Cur *c, const void *src, size_t n) {
    if (!src || !n) return NULL;
    void *d = c->p;
    memcpy(d, src, n);
    c->p += (n + 7) & ~(size_t)7;
    return d;
}
#define A8(n) (((size_t)(n) + 7) & ~(size_t)7)

// ---------------------------------------------------------------- the commands
#define CB VkCommandBuffer cb

typedef struct { CB; VkPipelineBindPoint bp; VkPipeline p; } BindPipelineA;
void vr_CmdBindPipeline(CB, VkPipelineBindPoint bp, VkPipeline p) {
    if (!vr_enabled()) { vkCmdBindPipeline(cb, bp, p); return; }
    BindPipelineA *a = rec_begin(OP_BindPipeline, sizeof *a);
    *a = (BindPipelineA){cb, bp, p};
    rec_commit();
}

typedef struct { CB; uint32_t first, n; const VkViewport *v; } SetViewportA;
void vr_CmdSetViewport(CB, uint32_t first, uint32_t n, const VkViewport *v) {
    if (!vr_enabled()) { vkCmdSetViewport(cb, first, n, v); return; }
    SetViewportA *a = rec_begin(OP_SetViewport, sizeof *a + A8(n * sizeof *v));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (SetViewportA){cb, first, n, cur_put(&c, v, n * sizeof *v)};
    rec_commit();
}

typedef struct { CB; uint32_t first, n; const VkRect2D *r; } SetScissorA;
void vr_CmdSetScissor(CB, uint32_t first, uint32_t n, const VkRect2D *r) {
    if (!vr_enabled()) { vkCmdSetScissor(cb, first, n, r); return; }
    SetScissorA *a = rec_begin(OP_SetScissor, sizeof *a + A8(n * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (SetScissorA){cb, first, n, cur_put(&c, r, n * sizeof *r)};
    rec_commit();
}

typedef struct { CB; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, ndyn; const VkDescriptorSet *s; const uint32_t *dyn; } BindDescriptorSetsA;
void vr_CmdBindDescriptorSets(CB, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s,
                              uint32_t ndyn, const uint32_t *dyn) {
    if (!vr_enabled()) { vkCmdBindDescriptorSets(cb, bp, l, first, n, s, ndyn, dyn); return; }
    BindDescriptorSetsA *a = rec_begin(OP_BindDescriptorSets, sizeof *a + A8(n * sizeof *s) + A8(ndyn * sizeof *dyn));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (BindDescriptorSetsA){cb, bp, l, first, n, ndyn, NULL, NULL};
    a->s = cur_put(&c, s, n * sizeof *s);
    a->dyn = cur_put(&c, dyn, ndyn * sizeof *dyn);
    rec_commit();
}

typedef struct { CB; VkDependencyInfo d; } PipelineBarrier2A;
void vr_CmdPipelineBarrier2(CB, const VkDependencyInfo *d) {
    bool plain = !d->pNext;
    for (uint32_t i = 0; plain && i < d->memoryBarrierCount; i++) plain = !d->pMemoryBarriers[i].pNext;
    for (uint32_t i = 0; plain && i < d->bufferMemoryBarrierCount; i++) plain = !d->pBufferMemoryBarriers[i].pNext;
    for (uint32_t i = 0; plain && i < d->imageMemoryBarrierCount; i++) plain = !d->pImageMemoryBarriers[i].pNext;
    if (!vr_enabled() || !plain) { vr_sync(); vkCmdPipelineBarrier2(cb, d); return; }
    size_t nm = d->memoryBarrierCount * sizeof *d->pMemoryBarriers, nb = d->bufferMemoryBarrierCount * sizeof *d->pBufferMemoryBarriers,
           ni = d->imageMemoryBarrierCount * sizeof *d->pImageMemoryBarriers;
    PipelineBarrier2A *a = rec_begin(OP_PipelineBarrier2, sizeof *a + A8(nm) + A8(nb) + A8(ni));
    Cur c = {(uint8_t *)(a + 1)};
    a->cb = cb;
    a->d = *d;
    a->d.pMemoryBarriers = cur_put(&c, d->pMemoryBarriers, nm);
    a->d.pBufferMemoryBarriers = cur_put(&c, d->pBufferMemoryBarriers, nb);
    a->d.pImageMemoryBarriers = cur_put(&c, d->pImageMemoryBarriers, ni);
    rec_commit();
}

typedef struct { CB; VkBuffer buf; VkImage img; VkImageLayout lay; uint32_t n; const VkBufferImageCopy *r; } BufImgA;
void vr_CmdCopyBufferToImage(CB, VkBuffer buf, VkImage img, VkImageLayout lay, uint32_t n, const VkBufferImageCopy *r) {
    if (!vr_enabled()) { vkCmdCopyBufferToImage(cb, buf, img, lay, n, r); return; }
    BufImgA *a = rec_begin(OP_CopyBufferToImage, sizeof *a + A8(n * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (BufImgA){cb, buf, img, lay, n, cur_put(&c, r, n * sizeof *r)};
    rec_commit();
}
typedef struct { CB; VkBuffer src, dst; uint32_t n; const VkBufferCopy *r; } BufBufA;
void vr_CmdCopyBuffer(CB, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    if (!vr_enabled()) { vkCmdCopyBuffer(cb, src, dst, n, r); return; }
    BufBufA *a = rec_begin(OP_CopyBuffer, sizeof *a + A8(n * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (BufBufA){cb, src, dst, n, cur_put(&c, r, n * sizeof *r)};
    rec_commit();
}
void vr_CmdCopyImageToBuffer(CB, VkImage img, VkImageLayout lay, VkBuffer buf, uint32_t n, const VkBufferImageCopy *r) {
    if (!vr_enabled()) { vkCmdCopyImageToBuffer(cb, img, lay, buf, n, r); return; }
    BufImgA *a = rec_begin(OP_CopyImageToBuffer, sizeof *a + A8(n * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (BufImgA){cb, buf, img, lay, n, cur_put(&c, r, n * sizeof *r)};
    rec_commit();
}

typedef struct { CB; } CbA;
void vr_CmdEndRendering(CB) {
    if (!vr_enabled()) { vkCmdEndRendering(cb); return; }
    CbA *a = rec_begin(OP_EndRendering, sizeof *a);
    a->cb = cb;
    rec_commit();
}

typedef struct { CB; uint32_t v[4]; } DrawA;
void vr_CmdDraw(CB, uint32_t nv, uint32_t ni, uint32_t fv, uint32_t fi) {
    if (!vr_enabled()) { vkCmdDraw(cb, nv, ni, fv, fi); return; }
    DrawA *a = rec_begin(OP_Draw, sizeof *a);
    *a = (DrawA){cb, {nv, ni, fv, fi}};
    rec_commit();
}

typedef struct { CB; uint32_t n, first, inst; int32_t vofs; uint32_t finst; } DrawIndexedA;
void vr_CmdDrawIndexed(CB, uint32_t n, uint32_t inst, uint32_t first, int32_t vofs, uint32_t finst) {
    if (!vr_enabled()) { vkCmdDrawIndexed(cb, n, inst, first, vofs, finst); return; }
    DrawIndexedA *a = rec_begin(OP_DrawIndexed, sizeof *a);
    *a = (DrawIndexedA){cb, n, first, inst, vofs, finst};
    rec_commit();
}

typedef struct { CB; VkRenderingInfo ri; } BeginRenderingA;
static bool att_plain(const VkRenderingAttachmentInfo *at) {
    if (!at || !at->pNext) return true;
    const VkBaseInStructure *x = at->pNext;
#ifdef VK_KHR_unified_image_layouts
    if (x->sType == VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT && !x->pNext) return true;
#endif
    (void)x;
    return false;
}
void vr_CmdBeginRendering(CB, const VkRenderingInfo *ri) {
    bool plain = !ri->pNext && (!ri->pDepthAttachment || !ri->pDepthAttachment->pNext) &&
                 (!ri->pStencilAttachment || !ri->pStencilAttachment->pNext);
    for (uint32_t i = 0; plain && i < ri->colorAttachmentCount; i++) plain = att_plain(&ri->pColorAttachments[i]);
    if (!vr_enabled() || !plain) { vr_sync(); vkCmdBeginRendering(cb, ri); return; }
    size_t na = ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo), nfb = 0;
#ifdef VK_KHR_unified_image_layouts
    nfb = ri->colorAttachmentCount * A8(sizeof(VkAttachmentFeedbackLoopInfoEXT));
#endif
    BeginRenderingA *a = rec_begin(OP_BeginRendering, sizeof *a + A8(na) + 2 * A8(sizeof(VkRenderingAttachmentInfo)) + nfb);
    Cur c = {(uint8_t *)(a + 1)};
    a->cb = cb;
    a->ri = *ri;
    VkRenderingAttachmentInfo *ca = cur_put(&c, ri->pColorAttachments, na);
    a->ri.pColorAttachments = ca;
#ifdef VK_KHR_unified_image_layouts
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++)
        if (ca[i].pNext) ca[i].pNext = cur_put(&c, ca[i].pNext, sizeof(VkAttachmentFeedbackLoopInfoEXT));
#endif
    a->ri.pDepthAttachment = cur_put(&c, ri->pDepthAttachment, ri->pDepthAttachment ? sizeof *ri->pDepthAttachment : 0);
    a->ri.pStencilAttachment = cur_put(&c, ri->pStencilAttachment, ri->pStencilAttachment ? sizeof *ri->pStencilAttachment : 0);
    rec_commit();
}

typedef struct { CB; VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; const void *data; } PushConstantsA;
void vr_CmdPushConstants(CB, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *data) {
    if (!vr_enabled()) { vkCmdPushConstants(cb, l, st, off, size, data); return; }
    PushConstantsA *a = rec_begin(OP_PushConstants, sizeof *a + A8(size));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (PushConstantsA){cb, l, st, off, size, cur_put(&c, data, size)};
    rec_commit();
}

typedef struct { CB; VkPipelineStageFlags2 st; VkQueryPool qp; uint32_t q; } WriteTimestamp2A;
void vr_CmdWriteTimestamp2(CB, VkPipelineStageFlags2 st, VkQueryPool qp, uint32_t q) {
    if (!vr_enabled()) { vkCmdWriteTimestamp2(cb, st, qp, q); return; }
    WriteTimestamp2A *a = rec_begin(OP_WriteTimestamp2, sizeof *a);
    *a = (WriteTimestamp2A){cb, st, qp, q};
    rec_commit();
}

// Commands of up to six 32-bit arguments (flags, enums, booleans, floats' bits)
typedef struct { CB; uint32_t v[6]; } U32A;
static void rec_u32(uint32_t op, VkCommandBuffer cb, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    U32A *a = rec_begin(op, sizeof *a);
    *a = (U32A){cb, {a0, a1, a2, a3, a4, 0}};
    rec_commit();
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

void vr_CmdSetStencilWriteMask(CB, VkStencilFaceFlags f, uint32_t m) {
    if (!vr_enabled()) { vkCmdSetStencilWriteMask(cb, f, m); return; }
    rec_u32(OP_SetStencilWriteMask, cb, f, m, 0, 0, 0);
}
void vr_CmdSetStencilCompareMask(CB, VkStencilFaceFlags f, uint32_t m) {
    if (!vr_enabled()) { vkCmdSetStencilCompareMask(cb, f, m); return; }
    rec_u32(OP_SetStencilCompareMask, cb, f, m, 0, 0, 0);
}
void vr_CmdSetStencilReference(CB, VkStencilFaceFlags f, uint32_t r) {
    if (!vr_enabled()) { vkCmdSetStencilReference(cb, f, r); return; }
    rec_u32(OP_SetStencilReference, cb, f, r, 0, 0, 0);
}
void vr_CmdSetStencilTestEnable(CB, VkBool32 b) {
    if (!vr_enabled()) { vkCmdSetStencilTestEnable(cb, b); return; }
    rec_u32(OP_SetStencilTestEnable, cb, b, 0, 0, 0, 0);
}
void vr_CmdSetStencilOp(CB, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp) {
    if (!vr_enabled()) { vkCmdSetStencilOp(cb, f, fail, pass, dfail, cmp); return; }
    rec_u32(OP_SetStencilOp, cb, f, fail, pass, dfail, cmp);
}
void vr_CmdSetDepthWriteEnable(CB, VkBool32 b) {
    if (!vr_enabled()) { vkCmdSetDepthWriteEnable(cb, b); return; }
    rec_u32(OP_SetDepthWriteEnable, cb, b, 0, 0, 0, 0);
}
void vr_CmdSetDepthTestEnable(CB, VkBool32 b) {
    if (!vr_enabled()) { vkCmdSetDepthTestEnable(cb, b); return; }
    rec_u32(OP_SetDepthTestEnable, cb, b, 0, 0, 0, 0);
}
void vr_CmdSetPrimitiveTopology(CB, VkPrimitiveTopology t) {
    if (!vr_enabled()) { vkCmdSetPrimitiveTopology(cb, t); return; }
    rec_u32(OP_SetPrimitiveTopology, cb, t, 0, 0, 0, 0);
}
void vr_CmdSetPrimitiveRestartEnable(CB, VkBool32 b) {
    if (!vr_enabled()) { vkCmdSetPrimitiveRestartEnable(cb, b); return; }
    rec_u32(OP_SetPrimitiveRestartEnable, cb, b, 0, 0, 0, 0);
}
void vr_CmdSetLineWidth(CB, float w) {
    if (!vr_enabled()) { vkCmdSetLineWidth(cb, w); return; }
    rec_u32(OP_SetLineWidth, cb, fbits(w), 0, 0, 0, 0);
}
void vr_CmdSetFrontFace(CB, VkFrontFace f) {
    if (!vr_enabled()) { vkCmdSetFrontFace(cb, f); return; }
    rec_u32(OP_SetFrontFace, cb, f, 0, 0, 0, 0);
}
void vr_CmdSetDepthCompareOp(CB, VkCompareOp op) {
    if (!vr_enabled()) { vkCmdSetDepthCompareOp(cb, op); return; }
    rec_u32(OP_SetDepthCompareOp, cb, op, 0, 0, 0, 0);
}
void vr_CmdSetDepthBiasEnable(CB, VkBool32 b) {
    if (!vr_enabled()) { vkCmdSetDepthBiasEnable(cb, b); return; }
    rec_u32(OP_SetDepthBiasEnable, cb, b, 0, 0, 0, 0);
}
void vr_CmdSetDepthBias(CB, float c, float cl, float s) {
    if (!vr_enabled()) { vkCmdSetDepthBias(cb, c, cl, s); return; }
    rec_u32(OP_SetDepthBias, cb, fbits(c), fbits(cl), fbits(s), 0, 0);
}
void vr_CmdSetCullMode(CB, VkCullModeFlags m) {
    if (!vr_enabled()) { vkCmdSetCullMode(cb, m); return; }
    rec_u32(OP_SetCullMode, cb, m, 0, 0, 0, 0);
}
void vr_CmdSetBlendConstants(CB, const float k[4]) {
    if (!vr_enabled()) { vkCmdSetBlendConstants(cb, k); return; }
    rec_u32(OP_SetBlendConstants, cb, fbits(k[0]), fbits(k[1]), fbits(k[2]), fbits(k[3]), 0);
}
void vr_CmdDispatch(CB, uint32_t x, uint32_t y, uint32_t z) {
    if (!vr_enabled()) { vkCmdDispatch(cb, x, y, z); return; }
    rec_u32(OP_Dispatch, cb, x, y, z, 0, 0);
}

typedef struct { CB; VkQueryPool qp; uint32_t a, b; } QueryA;
static void rec_query(uint32_t op, VkCommandBuffer cb, VkQueryPool qp, uint32_t x, uint32_t y) {
    QueryA *a = rec_begin(op, sizeof *a);
    *a = (QueryA){cb, qp, x, y};
    rec_commit();
}
void vr_CmdResetQueryPool(CB, VkQueryPool qp, uint32_t first, uint32_t n) {
    if (!vr_enabled()) { vkCmdResetQueryPool(cb, qp, first, n); return; }
    rec_query(OP_ResetQueryPool, cb, qp, first, n);
}
void vr_CmdBeginQuery(CB, VkQueryPool qp, uint32_t q, VkQueryControlFlags f) {
    if (!vr_enabled()) { vkCmdBeginQuery(cb, qp, q, f); return; }
    rec_query(OP_BeginQuery, cb, qp, q, f);
}
void vr_CmdEndQuery(CB, VkQueryPool qp, uint32_t q) {
    if (!vr_enabled()) { vkCmdEndQuery(cb, qp, q); return; }
    rec_query(OP_EndQuery, cb, qp, q, 0);
}

// Image to image (copy, blit, resolve): the regions differ in type only
typedef struct { CB; VkImage src; VkImageLayout sl; VkImage dst; VkImageLayout dl; uint32_t n; VkFilter filter; const void *r; } ImgImgA;
static void rec_imgimg(uint32_t op, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                       const void *r, size_t rsize, VkFilter filter) {
    ImgImgA *a = rec_begin(op, sizeof *a + A8(n * rsize));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (ImgImgA){cb, src, sl, dst, dl, n, filter, cur_put(&c, r, n * rsize)};
    rec_commit();
}
void vr_CmdCopyImage(CB, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    if (!vr_enabled()) { vkCmdCopyImage(cb, src, sl, dst, dl, n, r); return; }
    rec_imgimg(OP_CopyImage, cb, src, sl, dst, dl, n, r, sizeof *r, VK_FILTER_NEAREST);
}
void vr_CmdBlitImage(CB, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    if (!vr_enabled()) { vkCmdBlitImage(cb, src, sl, dst, dl, n, r, f); return; }
    rec_imgimg(OP_BlitImage, cb, src, sl, dst, dl, n, r, sizeof *r, f);
}
void vr_CmdResolveImage(CB, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    if (!vr_enabled()) { vkCmdResolveImage(cb, src, sl, dst, dl, n, r); return; }
    rec_imgimg(OP_ResolveImage, cb, src, sl, dst, dl, n, r, sizeof *r, VK_FILTER_NEAREST);
}

typedef struct { CB; VkImage img; VkImageLayout lay; VkClearValue v; uint32_t n; const VkImageSubresourceRange *r; } ClearImageA;
void vr_CmdClearColorImage(CB, VkImage img, VkImageLayout lay, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    if (!vr_enabled()) { vkCmdClearColorImage(cb, img, lay, v, n, r); return; }
    ClearImageA *a = rec_begin(OP_ClearColorImage, sizeof *a + A8(n * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (ClearImageA){cb, img, lay, {.color = *v}, n, cur_put(&c, r, n * sizeof *r)};
    rec_commit();
}
void vr_CmdClearDepthStencilImage(CB, VkImage img, VkImageLayout lay, const VkClearDepthStencilValue *v, uint32_t n,
                                  const VkImageSubresourceRange *r) {
    if (!vr_enabled()) { vkCmdClearDepthStencilImage(cb, img, lay, v, n, r); return; }
    ClearImageA *a = rec_begin(OP_ClearDepthStencilImage, sizeof *a + A8(n * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (ClearImageA){cb, img, lay, {.depthStencil = *v}, n, cur_put(&c, r, n * sizeof *r)};
    rec_commit();
}

typedef struct { CB; uint32_t na, nr; const VkClearAttachment *at; const VkClearRect *r; } ClearAttachmentsA;
void vr_CmdClearAttachments(CB, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r) {
    if (!vr_enabled()) { vkCmdClearAttachments(cb, na, at, nr, r); return; }
    ClearAttachmentsA *a = rec_begin(OP_ClearAttachments, sizeof *a + A8(na * sizeof *at) + A8(nr * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (ClearAttachmentsA){cb, na, nr, NULL, NULL};
    a->at = cur_put(&c, at, na * sizeof *at);
    a->r = cur_put(&c, r, nr * sizeof *r);
    rec_commit();
}

typedef struct { CB; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o; } BindVertexBuffersA;
void vr_CmdBindVertexBuffers(CB, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    if (!vr_enabled()) { vkCmdBindVertexBuffers(cb, first, n, b, o); return; }
    BindVertexBuffersA *a = rec_begin(OP_BindVertexBuffers, sizeof *a + A8(n * sizeof *b) + A8(n * sizeof *o));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (BindVertexBuffersA){cb, first, n, NULL, NULL};
    a->b = cur_put(&c, b, n * sizeof *b);
    a->o = cur_put(&c, o, n * sizeof *o);
    rec_commit();
}

typedef struct { CB; VkBuffer b; VkDeviceSize o; VkIndexType t; } BindIndexBufferA;
void vr_CmdBindIndexBuffer(CB, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (!vr_enabled()) { vkCmdBindIndexBuffer(cb, b, o, t); return; }
    BindIndexBufferA *a = rec_begin(OP_BindIndexBuffer, sizeof *a);
    *a = (BindIndexBufferA){cb, b, o, t};
    rec_commit();
}

// ---------------------------------------------------------------- command buffers and submits
// Beginning and ending a command buffer, and a submit that only signals the timeline (the submissions of a frame but
// its last), go into the ring too: each waited for the ring to empty, ~6% of the PS4's queue thread in the open world
// (~6 submits a frame). A submit waiting for or signalling a binary semaphore (the frame's last: the present waits for
// it, which must come after the real submit) still waits, as does anything with a pNext. Their errors are fatal here
// as they were for the callers. A wait for the timeline needs nothing more: the submit is put before it and the
// recording thread is woken for it at once.
typedef struct { CB; VkCommandBufferUsageFlags flags; } BeginCommandBufferA;
VkResult vr_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    if (!vr_enabled() || bi->pNext || bi->pInheritanceInfo) { vr_sync(); return vkBeginCommandBuffer(cb, bi); }
    BeginCommandBufferA *a = rec_begin(OP_BeginCommandBuffer, sizeof *a);
    *a = (BeginCommandBufferA){cb, bi->flags};
    rec_commit();
    return VK_SUCCESS;
}
typedef struct { CB; } EndCommandBufferA;
VkResult vr_EndCommandBuffer(VkCommandBuffer cb) {
    if (!vr_enabled()) return vkEndCommandBuffer(cb);
    EndCommandBufferA *a = rec_begin(OP_EndCommandBuffer, sizeof *a);
    *a = (EndCommandBufferA){cb};
    rec_commit();
    return VK_SUCCESS;
}
VkResult vr_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) {
    vr_sync();
    return vkAllocateCommandBuffers(d, ai, cb);
}
#define SUBMIT_CBS 8
typedef struct { VkQueue q; uint32_t ncb; VkCommandBufferSubmitInfo cbs[SUBMIT_CBS]; VkSemaphoreSubmitInfo sig; uint32_t nsig; } QueueSubmit2A;
VkResult vr_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f) {
    bool plain = vr_enabled() && n == 1 && !f && !si->pNext && !si->flags && !si->waitSemaphoreInfoCount &&
                 si->signalSemaphoreInfoCount <= 1 && si->commandBufferInfoCount <= SUBMIT_CBS;
    for (uint32_t i = 0; plain && i < si->commandBufferInfoCount; i++) plain = !si->pCommandBufferInfos[i].pNext;
    if (plain && si->signalSemaphoreInfoCount) plain = !si->pSignalSemaphoreInfos[0].pNext;
    if (!plain) { vr_sync(); return vkQueueSubmit2(q, n, si, f); }
    QueueSubmit2A *a = rec_begin(OP_QueueSubmit2, sizeof *a);
    a->q = q, a->ncb = si->commandBufferInfoCount, a->nsig = si->signalSemaphoreInfoCount;
    memcpy(a->cbs, si->pCommandBufferInfos, a->ncb * sizeof a->cbs[0]);
    if (a->nsig) a->sig = si->pSignalSemaphoreInfos[0];
    rec_commit();
    wake();
    return VK_SUCCESS;
}
VkResult vr_QueueWaitIdle(VkQueue q) { vr_sync(); return vkQueueWaitIdle(q); }
VkResult vr_DeviceWaitIdle(VkDevice d) { vr_sync(); return vkDeviceWaitIdle(d); }

// ---------------------------------------------------------------- the recording thread
static void run(uint32_t op, void *p) {
    switch (op) {
#define ARG(T) T *a = p
    case OP_BindPipeline: { ARG(BindPipelineA); vkCmdBindPipeline(a->cb, a->bp, a->p); break; }
    case OP_SetViewport: { ARG(SetViewportA); vkCmdSetViewport(a->cb, a->first, a->n, a->v); break; }
    case OP_SetScissor: { ARG(SetScissorA); vkCmdSetScissor(a->cb, a->first, a->n, a->r); break; }
    case OP_BindDescriptorSets: {
        ARG(BindDescriptorSetsA);
        vkCmdBindDescriptorSets(a->cb, a->bp, a->l, a->first, a->n, a->s, a->ndyn, a->dyn);
        break;
    }
    case OP_PipelineBarrier2: { ARG(PipelineBarrier2A); vkCmdPipelineBarrier2(a->cb, &a->d); break; }
    case OP_CopyBufferToImage: { ARG(BufImgA); vkCmdCopyBufferToImage(a->cb, a->buf, a->img, a->lay, a->n, a->r); break; }
    case OP_CopyBuffer: { ARG(BufBufA); vkCmdCopyBuffer(a->cb, a->src, a->dst, a->n, a->r); break; }
    case OP_BeginCommandBuffer: {
        ARG(BeginCommandBufferA);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = a->flags};
        VkResult r = vkBeginCommandBuffer(a->cb, &bi);
        if (r != VK_SUCCESS) FATAL("[vk] vkrec: vkBeginCommandBuffer: %d\n", r);
        break;
    }
    case OP_EndCommandBuffer: {
        ARG(EndCommandBufferA);
        VkResult r = vkEndCommandBuffer(a->cb);
        if (r != VK_SUCCESS) FATAL("[vk] vkrec: vkEndCommandBuffer: %d\n", r);
        break;
    }
    case OP_QueueSubmit2: {
        ARG(QueueSubmit2A);
        VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = a->ncb, .pCommandBufferInfos = a->cbs,
                            .signalSemaphoreInfoCount = a->nsig, .pSignalSemaphoreInfos = &a->sig};
        VkResult r = vkQueueSubmit2(a->q, 1, &si, VK_NULL_HANDLE);
        if (r == VK_ERROR_DEVICE_LOST) { extern void vk_devlost_dump(void); vk_devlost_dump(); }
        if (r != VK_SUCCESS) FATAL("[vk] vkrec: vkQueueSubmit2: %d\n", r);
        break;
    }
    case OP_CopyImageToBuffer: { ARG(BufImgA); vkCmdCopyImageToBuffer(a->cb, a->img, a->lay, a->buf, a->n, a->r); break; }
    case OP_EndRendering: { ARG(CbA); vkCmdEndRendering(a->cb); break; }
    case OP_Draw: { ARG(DrawA); vkCmdDraw(a->cb, a->v[0], a->v[1], a->v[2], a->v[3]); break; }
    case OP_DrawIndexed: { ARG(DrawIndexedA); vkCmdDrawIndexed(a->cb, a->n, a->inst, a->first, a->vofs, a->finst); break; }
    case OP_BeginRendering: { ARG(BeginRenderingA); vkCmdBeginRendering(a->cb, &a->ri); break; }
    case OP_PushConstants: { ARG(PushConstantsA); vkCmdPushConstants(a->cb, a->l, a->st, a->off, a->size, a->data); break; }
    case OP_WriteTimestamp2: { ARG(WriteTimestamp2A); vkCmdWriteTimestamp2(a->cb, a->st, a->qp, a->q); break; }
    case OP_SetStencilWriteMask: { ARG(U32A); vkCmdSetStencilWriteMask(a->cb, a->v[0], a->v[1]); break; }
    case OP_SetStencilCompareMask: { ARG(U32A); vkCmdSetStencilCompareMask(a->cb, a->v[0], a->v[1]); break; }
    case OP_SetStencilReference: { ARG(U32A); vkCmdSetStencilReference(a->cb, a->v[0], a->v[1]); break; }
    case OP_SetStencilTestEnable: { ARG(U32A); vkCmdSetStencilTestEnable(a->cb, a->v[0]); break; }
    case OP_SetStencilOp: {
        ARG(U32A);
        vkCmdSetStencilOp(a->cb, a->v[0], (VkStencilOp)a->v[1], (VkStencilOp)a->v[2], (VkStencilOp)a->v[3], (VkCompareOp)a->v[4]);
        break;
    }
    case OP_SetDepthWriteEnable: { ARG(U32A); vkCmdSetDepthWriteEnable(a->cb, a->v[0]); break; }
    case OP_SetDepthTestEnable: { ARG(U32A); vkCmdSetDepthTestEnable(a->cb, a->v[0]); break; }
    case OP_SetPrimitiveTopology: { ARG(U32A); vkCmdSetPrimitiveTopology(a->cb, (VkPrimitiveTopology)a->v[0]); break; }
    case OP_SetPrimitiveRestartEnable: { ARG(U32A); vkCmdSetPrimitiveRestartEnable(a->cb, a->v[0]); break; }
    case OP_SetLineWidth: { ARG(U32A); vkCmdSetLineWidth(a->cb, bitsf(a->v[0])); break; }
    case OP_SetFrontFace: { ARG(U32A); vkCmdSetFrontFace(a->cb, (VkFrontFace)a->v[0]); break; }
    case OP_SetDepthCompareOp: { ARG(U32A); vkCmdSetDepthCompareOp(a->cb, (VkCompareOp)a->v[0]); break; }
    case OP_SetDepthBiasEnable: { ARG(U32A); vkCmdSetDepthBiasEnable(a->cb, a->v[0]); break; }
    case OP_SetDepthBias: { ARG(U32A); vkCmdSetDepthBias(a->cb, bitsf(a->v[0]), bitsf(a->v[1]), bitsf(a->v[2])); break; }
    case OP_SetCullMode: { ARG(U32A); vkCmdSetCullMode(a->cb, a->v[0]); break; }
    case OP_SetBlendConstants: {
        ARG(U32A);
        const float k[4] = {bitsf(a->v[0]), bitsf(a->v[1]), bitsf(a->v[2]), bitsf(a->v[3])};
        vkCmdSetBlendConstants(a->cb, k);
        break;
    }
    case OP_Dispatch: { ARG(U32A); vkCmdDispatch(a->cb, a->v[0], a->v[1], a->v[2]); break; }
    case OP_ResetQueryPool: { ARG(QueryA); vkCmdResetQueryPool(a->cb, a->qp, a->a, a->b); break; }
    case OP_BeginQuery: { ARG(QueryA); vkCmdBeginQuery(a->cb, a->qp, a->a, a->b); break; }
    case OP_EndQuery: { ARG(QueryA); vkCmdEndQuery(a->cb, a->qp, a->a); break; }
    case OP_CopyImage: { ARG(ImgImgA); vkCmdCopyImage(a->cb, a->src, a->sl, a->dst, a->dl, a->n, a->r); break; }
    case OP_BlitImage: { ARG(ImgImgA); vkCmdBlitImage(a->cb, a->src, a->sl, a->dst, a->dl, a->n, a->r, a->filter); break; }
    case OP_ResolveImage: { ARG(ImgImgA); vkCmdResolveImage(a->cb, a->src, a->sl, a->dst, a->dl, a->n, a->r); break; }
    case OP_ClearColorImage: { ARG(ClearImageA); vkCmdClearColorImage(a->cb, a->img, a->lay, &a->v.color, a->n, a->r); break; }
    case OP_ClearDepthStencilImage: {
        ARG(ClearImageA);
        vkCmdClearDepthStencilImage(a->cb, a->img, a->lay, &a->v.depthStencil, a->n, a->r);
        break;
    }
    case OP_ClearAttachments: { ARG(ClearAttachmentsA); vkCmdClearAttachments(a->cb, a->na, a->at, a->nr, a->r); break; }
    case OP_BindVertexBuffers: { ARG(BindVertexBuffersA); vkCmdBindVertexBuffers(a->cb, a->first, a->n, a->b, a->o); break; }
    case OP_BindIndexBuffer: { ARG(BindIndexBufferA); vkCmdBindIndexBuffer(a->cb, a->b, a->o, a->t); break; }
#undef ARG
    default: FATAL("[vk] vkrec: unknown command %u\n", op);
    }
}

static void *rec_thread(void *arg) {
    (void)arg;
#ifdef __ORBIS__
    ps4_profile_thread("vkrec");
#endif
    uint64_t t = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&r_head, memory_order_acquire);
        if (t == h) {
            if (atomic_load(&r_waiting)) {  // (vr_sync / wait_room: caught up)
                pthread_mutex_lock(&r_lock);
                pthread_cond_broadcast(&r_done);
                pthread_mutex_unlock(&r_lock);
            }
            for (int i = 0; i < 200 && t == h; i++) {
                cpu_relax();
                h = atomic_load_explicit(&r_head, memory_order_acquire);
            }
            if (t == h) {
                pthread_mutex_lock(&r_lock);
                atomic_store_explicit(&r_slept_at, t, memory_order_relaxed);
                atomic_store(&r_sleeping, true);
                while ((h = atomic_load(&r_head)) == t) pthread_cond_wait(&r_work, &r_lock);
                atomic_store(&r_sleeping, false);
                pthread_mutex_unlock(&r_lock);
            }
        }
        while (t != h) {
            Hdr *e = (Hdr *)(ring + t % RING);
            if (e->op != OP_WRAP) run(e->op, e + 1);
            t += e->size;
            if ((t & 0xFFFF) < e->size || t == h) {  // (now and then, and at the end: room for the producer)
                atomic_store(&r_tail, t);  // (seq_cst: against vr_sync's r_waiting, as rec_commit's r_head)
                if (atomic_load(&r_waiting)) {
                    pthread_mutex_lock(&r_lock);
                    pthread_cond_broadcast(&r_done);
                    pthread_mutex_unlock(&r_lock);
                }
            }
        }
    }
    return NULL;
}

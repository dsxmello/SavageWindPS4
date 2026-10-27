// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __ORBIS__
#include <volk.h>  // (no Vulkan loader on the PS4: RADV is linked in, its entry points come through volk; device.c)
#else
#include <vulkan/vulkan.h>
#endif
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"
#ifdef __ORBIS__
// SDL's clock is clock_gettime, a system call on the PS4: the backend's timings (SAVAGEWIND_VK_PROF, GPU waits) read
// the platform's TSC clock instead (platform/ps4.c plat_now_ns; differences only, never compared with SDL's)
#include <SDL3/SDL_timer.h>
uint64_t plat_now_ns(void);
#define SDL_GetTicksNS() plat_now_ns()
#endif

void vk_device_fault_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) { vk_device_fault_report(); \
    FATAL("Vulkan device lost: the GPU stopped responding (%s, %s:%d)\n", #x, __FILE__, __LINE__); } \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    VkQueue present_queue;     // a second queue of the same family for presentation (device.c), or queue
    uint32_t qfam, qfam_count;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkPipelineCache pcache_pre;  // the precompiled shaders' libraries (cache/vk_precompiled.bin; read only while playing)
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, wide_lines;
    bool feat_aniso;  // samplerAnisotropy (Video > Anisotropic filtering)
    bool depth_clamp;          // depthClamp feature (nvnCommandBufferSetDepthClamp: shadow maps keep occluders before the near plane)
    bool occlusion_precise;    // occlusionQueryPrecise: SAMPLES_PASSED counts samples, not just "any"
    bool feedback_loop;        // attachment feedback loops + barriers inside dynamic rendering (exec.c)
    bool feedback_layout;      // ... in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL layout (no VK_KHR_unified_image_layouts: GENERAL otherwise)
    bool gpl;                  // graphics pipeline libraries with fast linking (exec.c)
    bool dmabuf;               // memory exported as dma-bufs (Linux: the guest heap, exec.c backend_heap_set)
    bool radv;                 // the driver is Mesa's RADV
    bool present_wait;         // VK_KHR_present_id + present_wait on (SAVAGEWIND_LOG_PRESENT: when frames are shown)
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    VkPresentModeKHR sc_mode;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_wait_ready(void);  // until the main thread has made the window and device (device.c)
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)
void vk_present_idle(void);  // device.c: the presentation thread has no present queued or in progress
void vk_dyn_forget(void);    // exec.c: graphics pipeline / dynamic state of the main command buffer changed elsewhere

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
// Render-target scale groups (textures.c tex_scale_group): none, the 3D scene's (internal resolution), the final
// image's (the window's resolution), shadow maps (Video > Shadow resolution)
enum { RS_NONE = 0, RS_SCENE = 1, RS_OUT = 2, RS_SHADOW = 3 };

typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    bool slices_renderable;  // 3D image made 2D-array compatible: its slices are render targets
    bool feedback_ok;        // made with VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT (sampled while it is rendered to)
    bool feedback_seen;      // a draw sampled it while writing it: its passes begin with the feedback loop enabled
    bool in_feedback_layout; // in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL now (g_vk.feedback_layout: a pass that loops, exec.c)
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    bool scalable, presented;
    uint8_t rs_group;            // RS_*: which output scale its allocation follows (textures.c)
    uint32_t guest_w;            // (RS_SHADOW: the guest width its scale is worked out from)
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    uint32_t glyph_gen;          // a button glyph sheet (runtime/nvn/glyphs.c): the icons it was uploaded with
    // textures.c: one image per memory and shape, shared by every texture made there
    uint64_t key_addr;           // 0: not shared
    uint32_t key[7];             // target, format, width, height, depth, levels, samples
    struct TexBackend *key_next;
    int refs;                    // textures holding the image
    int all, idle;               // slots + 1 in textures.c's lists of all images / images no texture holds
    uint64_t idle_frame, used_frame;  // when it was let go / last asked for by a finalized texture (zombies)
    uint64_t guest_size;         // bytes of guest storage its last texture covered (textures.c: kept images)
    uint64_t kept_hash, kept_bytes;
    bool kept;                   // parked past PARK_FRAMES with its contents (textures.c idle_expire)
    uint64_t drawn_frame, prev_drawn;  // last two frames a pass drew into it (exec.c: one-shot draws)
    uint64_t serial;             // which image this is: every image made gets a new one (exec.c's linear targets)
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkComponentMapping vk_sample_swizzle(NvnTexture *t, const NvnTextureView *v);  // (a colour texture's, sampled)
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
void vk_make_scalable_like(NvnTexture *t, NvnTexture *like);  // a pass's attachments share one scale
uint8_t vk_texture_scale_group(NvnTexture *t);
void renderer_get_scene_scale(float *sx, float *sy);
void renderer_set_scene_scale(float sx, float sy);
void renderer_set_shadow_size(uint32_t n);  // shadow maps' width (RS_SHADOW; 0: the game's own)
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
bool vk_replace_file(const char *tmp, const char *path);  // rename over an existing file, atomically

// ---- command recording on a thread of its own (vkrec.c): the backend's vkCmd* calls go through vr_Cmd*
bool vr_enabled(void);
void vr_sync(void);  // until every command put has been recorded
#define CB_ VkCommandBuffer cb
void vr_CmdBindPipeline(CB_, VkPipelineBindPoint bp, VkPipeline p);
void vr_CmdSetViewport(CB_, uint32_t first, uint32_t n, const VkViewport *v);
void vr_CmdSetScissor(CB_, uint32_t first, uint32_t n, const VkRect2D *r);
void vr_CmdBindDescriptorSets(CB_, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s,
                              uint32_t ndyn, const uint32_t *dyn);
void vr_CmdPipelineBarrier2(CB_, const VkDependencyInfo *d);
void vr_CmdCopyBufferToImage(CB_, VkBuffer buf, VkImage img, VkImageLayout lay, uint32_t n, const VkBufferImageCopy *r);
void vr_CmdCopyImageToBuffer(CB_, VkImage img, VkImageLayout lay, VkBuffer buf, uint32_t n, const VkBufferImageCopy *r);
void vr_CmdEndRendering(CB_);
void vr_CmdDraw(CB_, uint32_t nv, uint32_t ni, uint32_t fv, uint32_t fi);
void vr_CmdDrawIndexed(CB_, uint32_t n, uint32_t inst, uint32_t first, int32_t vofs, uint32_t finst);
void vr_CmdBeginRendering(CB_, const VkRenderingInfo *ri);
void vr_CmdPushConstants(CB_, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *data);
void vr_CmdWriteTimestamp2(CB_, VkPipelineStageFlags2 st, VkQueryPool qp, uint32_t q);
void vr_CmdSetStencilWriteMask(CB_, VkStencilFaceFlags f, uint32_t m);
void vr_CmdSetStencilCompareMask(CB_, VkStencilFaceFlags f, uint32_t m);
void vr_CmdSetStencilReference(CB_, VkStencilFaceFlags f, uint32_t r);
void vr_CmdSetStencilTestEnable(CB_, VkBool32 b);
void vr_CmdSetStencilOp(CB_, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp);
void vr_CmdSetDepthWriteEnable(CB_, VkBool32 b);
void vr_CmdSetDepthTestEnable(CB_, VkBool32 b);
void vr_CmdSetPrimitiveTopology(CB_, VkPrimitiveTopology t);
void vr_CmdSetPrimitiveRestartEnable(CB_, VkBool32 b);
void vr_CmdSetLineWidth(CB_, float w);
void vr_CmdSetFrontFace(CB_, VkFrontFace f);
void vr_CmdSetDepthCompareOp(CB_, VkCompareOp op);
void vr_CmdSetDepthBiasEnable(CB_, VkBool32 b);
void vr_CmdSetDepthBias(CB_, float c, float cl, float s);
void vr_CmdSetCullMode(CB_, VkCullModeFlags m);
void vr_CmdSetBlendConstants(CB_, const float k[4]);
void vr_CmdDispatch(CB_, uint32_t x, uint32_t y, uint32_t z);
void vr_CmdResetQueryPool(CB_, VkQueryPool qp, uint32_t first, uint32_t n);
void vr_CmdBeginQuery(CB_, VkQueryPool qp, uint32_t q, VkQueryControlFlags f);
void vr_CmdEndQuery(CB_, VkQueryPool qp, uint32_t q);
void vr_CmdCopyImage(CB_, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r);
void vr_CmdBlitImage(CB_, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f);
void vr_CmdResolveImage(CB_, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r);
void vr_CmdClearColorImage(CB_, VkImage img, VkImageLayout lay, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r);
void vr_CmdClearDepthStencilImage(CB_, VkImage img, VkImageLayout lay, const VkClearDepthStencilValue *v, uint32_t n,
                                  const VkImageSubresourceRange *r);
void vr_CmdClearAttachments(CB_, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r);
void vr_CmdBindVertexBuffers(CB_, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o);
void vr_CmdBindIndexBuffer(CB_, VkBuffer b, VkDeviceSize o, VkIndexType t);
#undef CB_
VkResult vr_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi);
VkResult vr_EndCommandBuffer(VkCommandBuffer cb);
VkResult vr_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb);
VkResult vr_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f);
VkResult vr_QueueWaitIdle(VkQueue q);
VkResult vr_DeviceWaitIdle(VkDevice d);
#ifndef VKREC_IMPL
#define vkCmdBindPipeline vr_CmdBindPipeline
#define vkCmdSetViewport vr_CmdSetViewport
#define vkCmdSetScissor vr_CmdSetScissor
#define vkCmdBindDescriptorSets vr_CmdBindDescriptorSets
#define vkCmdPipelineBarrier2 vr_CmdPipelineBarrier2
#define vkCmdCopyBufferToImage vr_CmdCopyBufferToImage
#define vkCmdCopyImageToBuffer vr_CmdCopyImageToBuffer
#define vkCmdEndRendering vr_CmdEndRendering
#define vkCmdDraw vr_CmdDraw
#define vkCmdDrawIndexed vr_CmdDrawIndexed
#define vkCmdBeginRendering vr_CmdBeginRendering
#define vkCmdPushConstants vr_CmdPushConstants
#define vkCmdWriteTimestamp2 vr_CmdWriteTimestamp2
#define vkCmdSetStencilWriteMask vr_CmdSetStencilWriteMask
#define vkCmdSetStencilCompareMask vr_CmdSetStencilCompareMask
#define vkCmdSetStencilReference vr_CmdSetStencilReference
#define vkCmdSetStencilTestEnable vr_CmdSetStencilTestEnable
#define vkCmdSetStencilOp vr_CmdSetStencilOp
#define vkCmdSetDepthWriteEnable vr_CmdSetDepthWriteEnable
#define vkCmdSetDepthTestEnable vr_CmdSetDepthTestEnable
#define vkCmdSetPrimitiveTopology vr_CmdSetPrimitiveTopology
#define vkCmdSetPrimitiveRestartEnable vr_CmdSetPrimitiveRestartEnable
#define vkCmdSetLineWidth vr_CmdSetLineWidth
#define vkCmdSetFrontFace vr_CmdSetFrontFace
#define vkCmdSetDepthCompareOp vr_CmdSetDepthCompareOp
#define vkCmdSetDepthBiasEnable vr_CmdSetDepthBiasEnable
#define vkCmdSetDepthBias vr_CmdSetDepthBias
#define vkCmdSetCullMode vr_CmdSetCullMode
#define vkCmdSetBlendConstants vr_CmdSetBlendConstants
#define vkCmdDispatch vr_CmdDispatch
#define vkCmdResetQueryPool vr_CmdResetQueryPool
#define vkCmdBeginQuery vr_CmdBeginQuery
#define vkCmdEndQuery vr_CmdEndQuery
#define vkCmdCopyImage vr_CmdCopyImage
#define vkCmdBlitImage vr_CmdBlitImage
#define vkCmdResolveImage vr_CmdResolveImage
#define vkCmdClearColorImage vr_CmdClearColorImage
#define vkCmdClearDepthStencilImage vr_CmdClearDepthStencilImage
#define vkCmdClearAttachments vr_CmdClearAttachments
#define vkCmdBindVertexBuffers vr_CmdBindVertexBuffers
#define vkCmdBindIndexBuffer vr_CmdBindIndexBuffer
#define vkBeginCommandBuffer vr_BeginCommandBuffer
#define vkEndCommandBuffer vr_EndCommandBuffer
#define vkAllocateCommandBuffers vr_AllocateCommandBuffers
#define vkQueueSubmit2 vr_QueueSubmit2
#define vkQueueWaitIdle vr_QueueWaitIdle
#define vkDeviceWaitIdle vr_DeviceWaitIdle
#endif

// The part of shaderc's C API that runtime/nvn/vulkan/shaders.c uses, implemented over glslang's C interface
// (ps4/shaderc_glslang.c): the PS4 build has glslang (GLSL -> SPIR-V on the console) but not shaderc and SPIRV-Tools.
// No SPIR-V optimizer: every optimization level compiles as shaderc_optimization_level_zero does.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    shaderc_vertex_shader = 0,
    shaderc_fragment_shader = 1,
    shaderc_compute_shader = 2,
} shaderc_shader_kind;
typedef enum { shaderc_target_env_vulkan = 0 } shaderc_target_env;
typedef enum { shaderc_env_version_vulkan_1_3 = (1u << 22) | (3 << 12) } shaderc_env_version;
typedef enum {
    shaderc_optimization_level_zero = 0,
    shaderc_optimization_level_size = 1,
    shaderc_optimization_level_performance = 2,
} shaderc_optimization_level;
typedef enum {
    shaderc_compilation_status_success = 0,
    shaderc_compilation_status_compilation_error = 2,
    shaderc_compilation_status_internal_error = 3,
} shaderc_compilation_status;

typedef struct shaderc_compiler *shaderc_compiler_t;
typedef struct shaderc_compile_options *shaderc_compile_options_t;
typedef struct shaderc_compilation_result *shaderc_compilation_result_t;

shaderc_compiler_t shaderc_compiler_initialize(void);
void shaderc_compiler_release(shaderc_compiler_t c);
shaderc_compile_options_t shaderc_compile_options_initialize(void);
void shaderc_compile_options_release(shaderc_compile_options_t o);
void shaderc_compile_options_set_target_env(shaderc_compile_options_t o, shaderc_target_env env, unsigned version);
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t o, shaderc_optimization_level level);
shaderc_compilation_result_t shaderc_compile_into_spv(shaderc_compiler_t c, const char *src, size_t size,
                                                      shaderc_shader_kind kind, const char *name, const char *entry,
                                                      shaderc_compile_options_t o);
shaderc_compilation_status shaderc_result_get_compilation_status(shaderc_compilation_result_t r);
const char *shaderc_result_get_error_message(shaderc_compilation_result_t r);
size_t shaderc_result_get_length(shaderc_compilation_result_t r);
const char *shaderc_result_get_bytes(shaderc_compilation_result_t r);
void shaderc_result_release(shaderc_compilation_result_t r);

#ifdef __cplusplus
}
#endif

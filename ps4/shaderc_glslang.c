// shaderc's C API (ps4/shaderc/shaderc/shaderc.h) over glslang's: GLSL -> SPIR-V for Vulkan 1.3 (SPIR-V 1.6).
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include "shaderc/shaderc.h"

struct shaderc_compiler { int unused; };
struct shaderc_compile_options { int unused; };
struct shaderc_compilation_result {
    shaderc_compilation_status status;
    char *error;
    char *bytes;
    size_t length;
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init_process(void) { glslang_initialize_process(); }

shaderc_compiler_t shaderc_compiler_initialize(void) {
    pthread_once(&g_once, init_process);
    return calloc(1, sizeof(struct shaderc_compiler));
}
void shaderc_compiler_release(shaderc_compiler_t c) { free(c); }
shaderc_compile_options_t shaderc_compile_options_initialize(void) { return calloc(1, sizeof(struct shaderc_compile_options)); }
void shaderc_compile_options_release(shaderc_compile_options_t o) { free(o); }
void shaderc_compile_options_set_target_env(shaderc_compile_options_t o, shaderc_target_env env, unsigned version) {
    (void)o, (void)env, (void)version;  // (always Vulkan 1.3)
}
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t o, shaderc_optimization_level level) {
    (void)o, (void)level;  // (no SPIR-V optimizer: ACO optimizes the shader anyway)
}

static shaderc_compilation_result_t fail(shaderc_compilation_result_t r, const char *what, const char *log) {
    r->status = shaderc_compilation_status_compilation_error;
    size_t n = strlen(what) + (log ? strlen(log) : 0) + 2;
    r->error = malloc(n);
    if (r->error) {
        strcpy(r->error, what);
        strcat(r->error, "\n");
        if (log) strcat(r->error, log);
    }
    return r;
}

shaderc_compilation_result_t shaderc_compile_into_spv(shaderc_compiler_t c, const char *src, size_t size,
                                                      shaderc_shader_kind kind, const char *name, const char *entry,
                                                      shaderc_compile_options_t o) {
    (void)c, (void)name, (void)entry, (void)o;
    shaderc_compilation_result_t r = calloc(1, sizeof *r);
    if (!r) return NULL;
    char *code = malloc(size + 1);  // (glslang wants it NUL-terminated)
    if (!code) { r->status = shaderc_compilation_status_internal_error; return r; }
    memcpy(code, src, size);
    code[size] = 0;
    glslang_stage_t stage = kind == shaderc_vertex_shader     ? GLSLANG_STAGE_VERTEX
                            : kind == shaderc_fragment_shader ? GLSLANG_STAGE_FRAGMENT
                                                              : GLSLANG_STAGE_COMPUTE;
    const glslang_input_t in = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_3,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_6,
        .code = code,
        .default_version = 100,
        .default_profile = GLSLANG_NO_PROFILE,
        .messages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT,
        .resource = glslang_default_resource(),
    };
    glslang_shader_t *sh = glslang_shader_create(&in);
    glslang_program_t *prog = NULL;
    if (!glslang_shader_preprocess(sh, &in)) { fail(r, "preprocess failed", glslang_shader_get_info_log(sh)); goto out; }
    if (!glslang_shader_parse(sh, &in)) { fail(r, "parse failed", glslang_shader_get_info_log(sh)); goto out; }
    prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fail(r, "link failed", glslang_program_get_info_log(prog));
        goto out;
    }
    glslang_program_SPIRV_generate(prog, stage);
    size_t words = glslang_program_SPIRV_get_size(prog);
    r->length = words * 4;
    r->bytes = malloc(r->length);
    if (!r->bytes) { r->status = shaderc_compilation_status_internal_error; goto out; }
    glslang_program_SPIRV_get(prog, (unsigned *)r->bytes);
    r->status = shaderc_compilation_status_success;
out:
    if (prog) glslang_program_delete(prog);
    glslang_shader_delete(sh);
    free(code);
    return r;
}

shaderc_compilation_status shaderc_result_get_compilation_status(shaderc_compilation_result_t r) {
    return r ? r->status : shaderc_compilation_status_internal_error;
}
const char *shaderc_result_get_error_message(shaderc_compilation_result_t r) { return r && r->error ? r->error : ""; }
size_t shaderc_result_get_length(shaderc_compilation_result_t r) { return r ? r->length : 0; }
const char *shaderc_result_get_bytes(shaderc_compilation_result_t r) { return r ? r->bytes : NULL; }
void shaderc_result_release(shaderc_compilation_result_t r) {
    if (!r) return;
    free(r->error);
    free(r->bytes);
    free(r);
}

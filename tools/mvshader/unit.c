// (tools/mvshader) The array-twin texture forms the recorded corpus doesn't reach (no fragment shader there fetches a
// 2D texel, none samples with a register offset or a bias on a twin): every TEXS / TLDS 2D form and the full TEX form's
// lod / bias / offset / depth compare combinations, translated by the translator's own functions on a fragment Ctx
// with every slot an array twin (ShaderKey.mv, tex_amask), finished by glsl_finish, then compiled and validated as
// the harness does. Also the same forms without mv, for the control. Usage: unit (run.sh builds and runs it).
#include "../../runtime/nvn/maxwell/translate.c"

#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>

static int fails;

static Ctx *mk(const ShaderKey *k, ShaderInfo **info) {
    *info = calloc(1, sizeof **info);
    Ctx *c = calloc(1, sizeof *c);
    c->info = *info, c->key = k, c->stage = STAGE_FRAGMENT, c->ipa_mode = -1, c->glsl = 1;
    c->omap_target = 0xF;  // (colour target 0: the results summed into it)
    return c;
}

static void check(const char *name, ShaderInfo *si, bool mv) {
    if (!si->msl) { printf("FAIL %s: translate: %s\n", name, si->error); fails++; return; }
    if (mv && !strstr(si->msl, "sampler2DArray")) { printf("FAIL %s: no array sampler\n", name); fails++; }
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, si->msl, strlen(si->msl), shaderc_fragment_shader, name, "main", o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        printf("FAIL %s: compile:\n%s\n%s\n", name, shaderc_result_get_error_message(r), si->msl);
        fails++;
    } else {
        spv_context vc = spvContextCreate(SPV_ENV_VULKAN_1_3);
        spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(r), shaderc_result_get_length(r) / 4};
        spv_diagnostic d = NULL;
        if (spvValidate(vc, &bin, &d) != SPV_SUCCESS) { printf("FAIL %s: validate: %s\n", name, d ? d->error : "?"); fails++; }
        spvDiagnosticDestroy(d);
        spvContextDestroy(vc);
    }
    if (getenv("UNIT_SHOW")) printf("---- %s\n%s", name, si->msl);
    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    shaderc_compiler_release(comp);
}

int main(void) {
    for (int mv = 0; mv < 2; mv++) {
        ShaderKey k = {0};
        k.glsl = 1, k.mv = (uint8_t)mv, k.vs_out_mask = 0xFFFFFFFF;
        if (mv) k.tex_amask[0] = k.tex_amask[1] = k.tex_amask[2] = 0xFF;
        // TEXS 2D (info 1-6: implicit, lz, ll, depth compare, compare + ll, compare + lz) and TLDS 2D (2, 4, 5, 12:
        // lz, offset, level, offset + level); one texture index (slot) each
        ShaderInfo *si;
        Ctx *c = mk(&k, &si);
        static const int samp[] = {1, 2, 3, 4, 5, 6}, fetch[] = {2, 4, 5, 12};
        int idx = 8;
        for (int f = 0; f < 2; f++)
            for (int i = 0; i < (f ? 4 : 6); i++) {
                int info = f ? fetch[i] : samp[i];
                uint64_t w = ((uint64_t)info << 53) | ((uint64_t)idx++ << 36);
                char call[4096];
                int slot;
                if (!glsl_texs(c, w, f, &slot, call, "r0", "r1", "r2", "r3")) { printf("FAIL texs %d %d\n", f, info); fails++; continue; }
                c->reg_used[0] = c->reg_used[1] = c->reg_used[2] = c->reg_used[3] = 1;
                write_tex_result(c, slot, call, 0xF, 4, RZ, false, false);
                sp(&c->body, "    r0 = r0 + r4;\n");  // (each result counted)
            }
        c->reg_used[4] = c->reg_used[5] = c->reg_used[6] = c->reg_used[7] = 1;
        check(mv ? "texs/tlds mv" : "texs/tlds", glsl_finish(c, false, false), mv);
        maxwell_free(si);
        // TEX 2D: lod none / lz / lb / ll, with and without offset (aoffi), with and without depth compare
        c = mk(&k, &si);
        for (int lod = 0; lod < 4; lod++)
            for (int off = 0; off < 2; off++)
                for (int dc = 0; dc < 2; dc++) {
                    static const int lm[] = {0, 1, 2, 3};
                    uint64_t w = 0xc038000000000000ull | (uint64_t)1 << 29 /* 2D */ | (uint64_t)0xF << 31 |
                                 (uint64_t)idx++ << 36 | (uint64_t)dc << 50 | (uint64_t)off << 54 | (uint64_t)lm[lod] << 55 |
                                 (uint64_t)8 << 0 /* d = r8 */ | (uint64_t)0 << 8 /* a = r0 */ | (uint64_t)2 << 20 /* b = r2 */;
                    emit_tex(c, w, false);
                    sp(&c->body, "    r0 = r0 + r8;\n");
                }
        check(mv ? "tex mv" : "tex", glsl_finish(c, false, false), mv);
        maxwell_free(si);
    }
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

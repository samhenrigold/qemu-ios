/* ES2 coherent framebuffer fetch on the legacy desktop renderer. Included
 * after GLESHost and the memory-budget helpers; no guest shader assets. */
/* Inject helper declarations after the shader's extension directives. GLSL
 * requires directives to precede ordinary declarations, including ours. */
static size_t gles_extension_prefix_end(const char *source)
{
    const char *p = source;
    size_t last = 0;
    while (*p) {
        const char *end = strchr(p, '\n');
        end = end ? end + 1 : p + strlen(p);
        const char *t = p;
        while (t < end && (*t == ' ' || *t == '\t'))
            t++;
        if (!strncmp(t, "#extension", 10) && g_ascii_isspace(t[10])) {
            last = end - source;
        }
        p = end;
    }
    return last;
}

typedef struct {
    GLint sampler, scale, clip[2];
    GArray *uniforms;
    GLint max_name;
} GLESFetchProgram;

static GHashTable **gles_fetch_table(void)
{
    return gh.group ? &gh.group->fetch_programs : &gh.fetch_programs;
}

static void gles_fetch_program_free(gpointer opaque)
{
    GLESFetchProgram *p = opaque;
    g_array_free(p->uniforms, true);
    g_free(p);
}

static GLESFetchProgram *gles_fetch_program(GLuint program)
{
    GHashTable *table = *gles_fetch_table();
    return table ? g_hash_table_lookup(table, GUINT_TO_POINTER(program)) : NULL;
}

static bool gles_fetch_private_name(const char *name)
{
    return !strcmp(name, "qemu_fetch_sampler") ||
           !strcmp(name, "qemu_fetch_scale") ||
           !strcmp(name, "qemu_clip_enabled0") ||
           !strcmp(name, "qemu_clip_enabled1");
}

static void gles_fetch_forget(GLuint program)
{
    if (*gles_fetch_table()) {
        g_hash_table_remove(*gles_fetch_table(), GUINT_TO_POINTER(program));
    }
}

static void gles_fetch_link(GLuint program)
{
    gles_fetch_forget(program);
    GLint sampler = glGetUniformLocation(program, "qemu_fetch_sampler");
    GLint clip = glGetUniformLocation(program, "qemu_clip_enabled0");
    if (sampler < 0 && clip < 0) {
        return;
    }
    GLESFetchProgram *p = g_new0(GLESFetchProgram, 1);
    p->sampler = sampler;
    p->clip[0] = clip;
    p->clip[1] = glGetUniformLocation(program, "qemu_clip_enabled1");
    p->scale = glGetUniformLocation(program, "qemu_fetch_scale");
    p->uniforms = g_array_new(false, false, sizeof(GLuint));
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (GLuint i = 0; i < count; i++) {
        char name[1024];
        GLsizei length;
        GLint size;
        GLenum type;
        glGetActiveUniform(program, i, sizeof(name), &length, &size, &type,
                           name);
        if (gles_fetch_private_name(name)) {
            continue;
        }
        g_array_append_val(p->uniforms, i);
        p->max_name = MAX(p->max_name, length + 1);
    }
    if (!*gles_fetch_table()) {
        *gles_fetch_table() = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL, gles_fetch_program_free);
    }
    g_hash_table_insert(*gles_fetch_table(), GUINT_TO_POINTER(program), p);
}

static unsigned gles_fetch_capabilities(void)
{
#if defined(GLES_HOST_EAGL) || defined(GLES_HOST_ANGLE)
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    const char *name = "GL_EXT_shader_framebuffer_fetch", *p = extensions;
    while (p && (p = strstr(p, name))) {
        if ((p == extensions || p[-1] == ' ') &&
            (!p[strlen(name)] || p[strlen(name)] == ' '))
            return 1;
        p += strlen(name);
    }
    return 0;
#else
    GLint units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
    return units > GLES_MAX_TEXUNITS ? 1 : 0;
#endif
}

#if !defined(GLES_HOST_EAGL) && !defined(GLES_HOST_ANGLE)
/* Rewrite identifiers, not substrings. A wrapper initializes the read-only ES2
 * input before the application's main, including calls to its helper functions. */
static char *gles_fetch_glsl(const char *source)
{
    bool fetch = strstr(source, "gl_LastFragData") != NULL;
    if (!fetch && !strstr(source, "GL_EXT_shader_framebuffer_fetch")) {
        return g_strdup(source);
    }
    /* Shared shader headers can require the extension even in vertex shaders
     * or fragments that never read it. Normalize the directive and capability
     * macro there too, without adding a framebuffer sampler or main wrapper. */
    GString *out =
        g_string_new("#version 120\n"
                     "#define QEMU_EXT_shader_framebuffer_fetch 1\n");
    const char *declarations =
        "uniform sampler2D qemu_fetch_sampler;\n"
        "uniform vec2 qemu_fetch_scale;\nvec4 qemu_fetch_colors[1];\n";
    /* The extension permits a precision redeclaration of the input. Its
     * desktop declaration is supplied above, after ES precision removal. */
    GRegex *declaration = g_regex_new(
        "(?m)^[ \\t]*(?:(?:lowp|mediump|highp)[ \\t]+)?vec4[ \\t]+gl_LastFragData[ \\t]*\\[[^\\]]+\\][ \\t]*;[ \\t]*$",
        0, 0, NULL);
    g_autofree char *clean =
        g_regex_replace_literal(declaration, source, -1, 0, "", 0, NULL);
    g_regex_unref(declaration);
    size_t prefix_end = gles_extension_prefix_end(clean);
    bool declared = !fetch;
    const char *p = clean;
    while (*p) {
        if (!declared && (size_t)(p - clean) >= prefix_end) {
            g_string_append(out, declarations);
            declared = true;
        }
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p + 1) : strlen(p);
        const char *t = p;
        while (t < p + n && g_ascii_isspace(*t)) {
            t++;
        }
        if (!strncmp(t, "#version", 8) ||
            (!strncmp(t, "#extension", 10) &&
             g_strstr_len(t, p + n - t, "GL_EXT_shader_framebuffer_fetch"))) {
            g_string_append_c(out, '\n');
            p += n;
            continue;
        }
        const char *limit = p + n;
        while (p < limit) {
            if (g_ascii_isalpha(*p) || *p == '_') {
                const char *word = p++;
                while (p < limit && (g_ascii_isalnum(*p) || *p == '_')) {
                    p++;
                }
                size_t length = p - word;
                const char *replacement = NULL;
                if (fetch && length == 4 && !memcmp(word, "main", 4))
                    replacement = "qemu_fetch_main";
                if (length == 15 && !memcmp(word, "gl_LastFragData", 15))
                    replacement = "qemu_fetch_colors";
                if (fetch && length == 17 &&
                    !memcmp(word, "gl_MaxDrawBuffers", 17))
                    replacement = "1";
                if (length == strlen("GL_EXT_shader_framebuffer_fetch") &&
                    !memcmp(word, "GL_EXT_shader_framebuffer_fetch", length))
                    replacement = "QEMU_EXT_shader_framebuffer_fetch";
                if (replacement)
                    g_string_append(out, replacement);
                else
                    g_string_append_len(out, word, length);
            } else {
                g_string_append_c(out, *p++);
            }
        }
    }
    if (!declared)
        g_string_append(out, declarations);
    if (fetch)
        g_string_append(
            out,
            "\nvoid main(){qemu_fetch_colors[0]=texture2D(qemu_fetch_sampler,gl_FragCoord.xy*qemu_fetch_scale);qemu_fetch_main();}\n");
    return g_string_free(out, false);
}

typedef struct {
    GLint active, binding;
} GLESFetchBinding;

static bool gles_fetch_copy(GLESFetchProgram *program)
{
    GLint kind, object, width = 0, height = 0, format = GL_RGBA8;
    glGetFramebufferAttachmentParameterivEXT(
        GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &kind);
    glGetFramebufferAttachmentParameterivEXT(
        GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT, &object);
    if (kind == GL_TEXTURE) {
        GLint level, face, saved;
        glGetFramebufferAttachmentParameterivEXT(
            GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_EXT, &level);
        glGetFramebufferAttachmentParameterivEXT(
            GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE_EXT, &face);
        GLenum target = face ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        glGetIntegerv(
            face ? GL_TEXTURE_BINDING_CUBE_MAP : GL_TEXTURE_BINDING_2D, &saved);
        glBindTexture(target, object);
        glGetTexLevelParameteriv(face ? face : target, level, GL_TEXTURE_WIDTH,
                                 &width);
        glGetTexLevelParameteriv(face ? face : target, level, GL_TEXTURE_HEIGHT,
                                 &height);
        glGetTexLevelParameteriv(face ? face : target, level,
                                 GL_TEXTURE_INTERNAL_FORMAT, &format);
        glBindTexture(target, saved);
    } else if (kind == GL_RENDERBUFFER_EXT) {
        GLint saved;
        glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &saved);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, object);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT,
                                        GL_RENDERBUFFER_WIDTH_EXT, &width);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT,
                                        GL_RENDERBUFFER_HEIGHT_EXT, &height);
        glGetRenderbufferParameterivEXT(
            GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_INTERNAL_FORMAT_EXT, &format);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, saved);
    }
    unsigned bpp = format == GL_RGBA32F_ARB                          ? 16
                   : format == GL_RGBA16F_ARB || format == GL_RGBA16 ? 8
                                                                     : 4;
    uint64_t bytes = (uint64_t)MAX(width, 0) * MAX(height, 0) * bpp;
    if (!bytes || bytes > GLES_MAX_TEX_BYTES ||
        !gles_charge(gles_charge_table(true),
                     gles_charge_key(GLES_CHARGE_FEEDBACK, 0, 0, 0), bytes,
                     NULL))
        return false;
    if (!gh.feedback_tex)
        gh.feedback_tex = gles_private_name(glIsTexture);
    glBindTexture(GL_TEXTURE_2D, gh.feedback_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint read_fb, draw_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &draw_fb);
    gles_texture_begin();
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, draw_fb);
    GLint allocated_width = 0, allocated_height = 0, allocated_format = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &allocated_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &allocated_height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &allocated_format);
    if (allocated_width == width && allocated_height == height &&
        allocated_format == format) {
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    } else {
        glCopyTexImage2D(GL_TEXTURE_2D, 0, format, 0, 0, width, height, 0);
    }
    GLenum error = glGetError();
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, read_fb);
    if (error != GL_NO_ERROR) {
        gles_reject(error);
        return false;
    }
    glUniform1i(program->sampler, GLES_MAX_TEXUNITS);
    glUniform2f(program->scale, 1.0f / width, 1.0f / height);
    return true;
}

/* TextureBarrierNV permits one same-pixel read/write per primitive. Keep the
 * guest's depth/stencil attachments, render into the private fetch texture,
 * then resolve its colour back once for the entire guest draw. This avoids a
 * full framebuffer copy for every triangle while retaining primitive order. */
static bool gles_fetch_barriers_supported(void)
{
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    const char *name = "GL_NV_texture_barrier", *p = extensions;
    while (p && (p = strstr(p, name))) {
        size_t n = strlen(name);
        if ((p == extensions || p[-1] == ' ') && (!p[n] || p[n] == ' '))
            return true;
        p += n;
    }
    return false;
}

typedef struct {
    GLint read, draw, kind, object, face, level, width, height;
} GLESFetchTarget;

static void gles_fetch_restore_attachment(const GLESFetchTarget *t)
{
    if (t->kind == GL_RENDERBUFFER_EXT) {
        glFramebufferRenderbufferEXT(GL_DRAW_FRAMEBUFFER_EXT,
                                     GL_COLOR_ATTACHMENT0_EXT,
                                     GL_RENDERBUFFER_EXT, t->object);
    } else {
        glFramebufferTexture2DEXT(
            GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
            t->face ? t->face : GL_TEXTURE_2D, t->object, t->level);
    }
}

/* -1: initial copy failed; 0: use the general copy path; 1: barrier path ready. */
static int gles_fetch_barrier_begin(GLESFetchProgram *program,
                                    GLESFetchTarget *t)
{
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (samples > 1)
        return 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &t->read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &t->draw);
    if (!t->draw)
        return 0;
    glGetFramebufferAttachmentParameterivEXT(
        GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &t->kind);
    if (t->kind != GL_TEXTURE && t->kind != GL_RENDERBUFFER_EXT)
        return 0;
    glGetFramebufferAttachmentParameterivEXT(
        GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT, &t->object);
    if (t->kind == GL_TEXTURE) {
        glGetFramebufferAttachmentParameterivEXT(
            GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_EXT, &t->level);
        glGetFramebufferAttachmentParameterivEXT(
            GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE_EXT, &t->face);
    }
    if (!gles_fetch_copy(program))
        return -1;
    GLint format;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &t->width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &t->height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &format);
    /* Preserve the established conversion path for sRGB targets. */
    if (format == GL_SRGB8 || format == GL_SRGB8_ALPHA8)
        return 0;
    glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, gh.feedback_tex, 0);
    GLenum status = glCheckFramebufferStatusEXT(GL_DRAW_FRAMEBUFFER_EXT),
           error = glGetError();
    if (error || status != GL_FRAMEBUFFER_COMPLETE_EXT) {
        gles_fetch_restore_attachment(t);
        return 0;
    }
    return 1;
}

static bool gles_fetch_barrier_end(const GLESFetchTarget *t)
{
    gles_fetch_restore_attachment(t);
    if (!gh.fetch_barrier_fbo)
        gh.fetch_barrier_fbo = gles_private_name(glIsFramebufferEXT);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, gh.fetch_barrier_fbo);
    glFramebufferTexture2DEXT(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, gh.feedback_tex, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glTextureBarrierNV();
    glBlitFramebufferEXT(0, 0, t->width, t->height, 0, 0, t->width, t->height,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLenum error = glGetError();
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, t->read);
    if (error) {
        gles_reject(error);
        return false;
    }
    return true;
}

#include "gles-fetch-batch.h"

/* Each primitive sees all prior primitives, including overlapping geometry in
 * one draw. A single snapshot per draw would not implement the coherent EXT. */
static bool gles_fetch_draw_impl(GLESFetchProgram *program, GLenum mode,
                                 uint32_t count, const uint8_t *indices,
                                 unsigned index_bytes, bool barriers)
{
    unsigned step = mode == GL_TRIANGLES ? 3 : mode == GL_LINES ? 2 : 1;
    unsigned size = mode == GL_POINTS ? 1 : mode <= GL_LINE_STRIP ? 2 : 3;
    if (mode > GL_TRIANGLE_FAN) {
        glDrawArrays(mode, 0, 0);
        return true;
    }
    GLESFetchBinding saved;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved.active);
    glActiveTexture(GL_TEXTURE0 + GLES_MAX_TEXUNITS);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved.binding);
    bool ok = true;
    unsigned end = count < size           ? 0
                   : mode == GL_LINE_LOOP ? count
                                          : count - size + 1;
    GLESFetchTarget target = {0};
    int barrier =
        barriers && end ? gles_fetch_barrier_begin(program, &target) : 0;
    if (barrier < 0)
        ok = false;
    /* This path uses only host-owned indices/attributes and native GL. The
     * executor gate retains the context and scratch state while I/O and other
     * vCPUs use the BQL. Preparation, fallback copies and resolve stay locked. */
    bool batched =
        barrier > 0 && gles_fetch_batch_draw(mode, count, indices, index_bytes);
    bool unlocked = !batched && barrier > 0 && gles_native_begin();
    for (unsigned i = 0; !batched && ok && i < end; i += step) {
        GLuint v[3] = {i, i + 1, i + 2};
        if (mode == GL_LINE_LOOP)
            v[1] %= count;
        if (mode == GL_TRIANGLE_FAN) {
            v[0] = 0;
            v[1] = i + 1;
            v[2] = i + 2;
        }
        if (mode == GL_TRIANGLE_STRIP && (i & 1)) {
            GLuint t = v[0];
            v[0] = v[1];
            v[1] = t;
        }
        if (indices) {
            for (unsigned k = 0; k < size; k++)
                v[k] = index_bytes == 1 ? indices[v[k]]
                                        : lduw_le_p(indices + v[k] * 2);
        }
        if (barrier)
            glTextureBarrierNV();
        else if (!gles_fetch_copy(program)) {
            ok = false;
            break;
        }
        glDrawElements(size == 1   ? GL_POINTS
                       : size == 2 ? GL_LINES
                                   : GL_TRIANGLES,
                       size, GL_UNSIGNED_INT, v);
    }
    gles_native_end(unlocked);
    if (barrier > 0 && !gles_fetch_barrier_end(&target))
        ok = false;
    glBindTexture(GL_TEXTURE_2D, saved.binding);
    glActiveTexture(saved.active);
    return ok;
}

static bool gles_fetch_draw(GLESFetchProgram *program, GLenum mode,
                            uint32_t count, const uint8_t *indices,
                            unsigned index_bytes)
{
    return gles_fetch_draw_impl(program, mode, count, indices, index_bytes,
                                gles_fetch_barriers_supported());
}
#endif

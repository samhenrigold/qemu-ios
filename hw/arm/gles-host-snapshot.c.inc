/*
 * Saving and restoring live GL state (#included by gles-host.c, CGL backend).
 *
 * The state lives in host CGL contexts, which QEMU cannot serialize, so it is
 * read back through GL at save time and rebuilt through GL at load time:
 *
 *   - share groups and contexts keep their handles (the guest holds them);
 *   - per namespace (a share group, or a context with none): emulated buffer
 *     objects, textures with their pixels, renderbuffers (storage only: their
 *     contents come back cleared), framebuffer attachments, shaders and
 *     programs with attribute bindings and uniform values;
 *   - per context: the GLESHost fields that are guest state (client and
 *     attribute arrays, bindings, pixel store, drawable size, ...) and the host
 *     GL context's own state, fixed function included (see GLESCtxGL).
 *
 * Texture, buffer, framebuffer and renderbuffer names are bound back
 * directly, which legacy GL allows; shader and program names are the guest ids
 * of gles_glsl_* and get new host ids. The host-private drawable objects
 * (gh.fbo/tex/depth) are ordinary GL objects in the namespace and travel the
 * same way.
 *
 * Everything is one versioned blob in the "gles-host" section.
 */

#define SNAP_MAGIC   0x534c4731u     /* "1GLS" */
#define SNAP_VERSION 3
#define SNAP_SCAN_GAP 256            /* names probed past the last live one */

static uint32_t gles_group_handle(GLESGroup *group);

typedef struct { GByteArray *b; } SnapW;
typedef struct { const uint8_t *p; size_t n, off; bool bad; } SnapR;

static void sw32(SnapW *w, uint32_t v) { g_byte_array_append(w->b, (uint8_t *)&v, 4); }
static void swbytes(SnapW *w, const void *p, uint32_t n)
{
    sw32(w, n);
    if (n) g_byte_array_append(w->b, p, n);
}
static uint32_t sr32(SnapR *r)
{
    uint32_t v = 0;
    if (r->off + 4 > r->n) { r->bad = true; return 0; }
    memcpy(&v, r->p + r->off, 4);
    r->off += 4;
    return v;
}
/* A length-prefixed run; NULL (and r->bad) if it overruns the blob. */
static const uint8_t *srbytes(SnapR *r, uint32_t *n)
{
    const uint8_t *p;
    *n = sr32(r);
    if (r->bad || r->off + *n > r->n) { r->bad = true; return NULL; }
    p = r->p + r->off;
    r->off += *n;
    return p;
}

/* ---- per-context GL state ------------------------------------------------ */

static const GLenum snap_enables[] = {
    GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
    GL_DITHER, GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE,
    GL_SAMPLE_COVERAGE, GL_ALPHA_TEST, GL_FOG, GL_LIGHTING, GL_LIGHT0, GL_LIGHT1,
    GL_LIGHT2, GL_LIGHT3, GL_LIGHT4, GL_LIGHT5, GL_LIGHT6, GL_LIGHT7,
    GL_COLOR_MATERIAL, GL_NORMALIZE, GL_RESCALE_NORMAL, GL_POINT_SMOOTH,
    GL_LINE_SMOOTH, GL_MULTISAMPLE, GL_COLOR_LOGIC_OP, GL_CLIP_PLANE0,
    GL_CLIP_PLANE1, GL_CLIP_PLANE2, GL_CLIP_PLANE3, GL_CLIP_PLANE4,
    GL_CLIP_PLANE5, GL_POINT_SPRITE,
};
static const GLenum snap_hints[] = {
    GL_PERSPECTIVE_CORRECTION_HINT, GL_POINT_SMOOTH_HINT, GL_LINE_SMOOTH_HINT,
    GL_FOG_HINT, GL_GENERATE_MIPMAP_HINT,
};

#define SNAP_MV_DEPTH  32
#define SNAP_AUX_DEPTH 4

typedef struct {
    GLfloat amb[4], dif[4], spec[4], pos[4], spot_dir[4];
    GLfloat spot_exp, spot_cut, att[3];
} SnapLight;

typedef struct {
    GLint mode, combine_rgb, combine_a, src_rgb[3], src_a[3], op_rgb[3], op_a[3];
    GLfloat color[4], rgb_scale, a_scale, cur_tex[4];
    GLint coord_replace, tex2d, rect, bind2d, bindrect;
    GLint mat_depth;
    GLfloat mat[SNAP_AUX_DEPTH][16];
} SnapUnit;

/* The host context's own state. Written as raw bytes: the layout is this
 * build's, and SNAP_VERSION plus sizeof() guard a mismatch. */
typedef struct {
    GLint viewport[4], scissor[4];
    GLfloat depth_range[2], clear_color[4], clear_depth;
    GLint clear_stencil;
    GLboolean color_mask[4], depth_mask, sample_inv, pad0[2];
    GLint stencil_wm, stencil_back_wm;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint blend_eq_rgb, blend_eq_a;
    GLfloat blend_color[4];
    GLint depth_func, cull_mode, front_face;
    GLint st[2][6];     /* front/back: func, ref, value mask, fail, zfail, zpass */
    GLfloat poly_factor, poly_units, line_width, point_size, alpha_ref, sample_cov;
    GLint alpha_func, shade_model, logic_op;
    GLint hints[ARRAY_SIZE(snap_hints)];
    GLboolean enables[ARRAY_SIZE(snap_enables)];
    GLint mv_depth, proj_depth;
    GLfloat mv[SNAP_MV_DEPTH][16], proj[SNAP_AUX_DEPTH][16];
    SnapLight light[8];
    GLfloat lm_amb[4];
    GLint lm_two;
    GLfloat mat_amb[4], mat_dif[4], mat_spec[4], mat_emi[4], mat_shin;
    GLint fog_mode;
    GLfloat fog_density, fog_start, fog_end, fog_color[4];
    GLfloat cur_color[4], cur_normal[4];
    GLfloat pt_min, pt_max, pt_att[3], pt_fade;
    GLfloat clip[6][4];
    GLint units;
    SnapUnit unit[GLES_MAX_TEXUNITS];
    GLint active_tex, fbo, rb, program;
    GLfloat vattr[GLES_MAX_ATTRIBS][4];
} GLESCtxGL;

/* Every matrix of the stack for `mode`, bottom first. Only the top of a stack
 * can be read, so it is popped down to the bottom and pushed back. */
static void snap_get_stack(GLenum mode, GLenum depth_pname, GLenum mat_pname,
                           GLint *depth, GLfloat (*m)[16], int cap)
{
    GLint d = 1;
    glMatrixMode(mode);
    glGetIntegerv(depth_pname, &d);
    d = MIN(MAX(d, 1), cap);
    *depth = d;
    for (int i = d - 1; i >= 0; i--) {
        glGetFloatv(mat_pname, m[i]);
        if (i) glPopMatrix();
    }
    for (int i = 1; i < d; i++) {
        glPushMatrix();
        glLoadMatrixf(m[i]);
    }
}

static void snap_set_stack(GLenum mode, GLenum depth_pname, GLint depth, GLfloat (*m)[16])
{
    GLint d = 1;
    glMatrixMode(mode);
    glGetIntegerv(depth_pname, &d);
    while (d-- > 1) glPopMatrix();
    glLoadMatrixf(m[0]);
    for (int i = 1; i < depth; i++) {
        glPushMatrix();
        glLoadMatrixf(m[i]);
    }
}

static void snap_capture_gl(GLESCtxGL *s)
{
    GLint mode = GL_MODELVIEW;
    memset(s, 0, sizeof(*s));
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
    glGetFloatv(GL_DEPTH_RANGE, s->depth_range);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clear_depth);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &s->clear_stencil);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_SAMPLE_COVERAGE_INVERT, &s->sample_inv);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s->stencil_wm);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &s->stencil_back_wm);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_a);
    glGetFloatv(GL_BLEND_COLOR, s->blend_color);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    glGetIntegerv(GL_CULL_FACE_MODE, &s->cull_mode);
    glGetIntegerv(GL_FRONT_FACE, &s->front_face);
    {
        static const GLenum f[6] = { GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK,
                                     GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS };
        static const GLenum b[6] = { GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK,
                                     GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                                     GL_STENCIL_BACK_PASS_DEPTH_PASS };
        for (int i = 0; i < 6; i++) {
            glGetIntegerv(f[i], &s->st[0][i]);
            glGetIntegerv(b[i], &s->st[1][i]);
        }
    }
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s->poly_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s->poly_units);
    glGetFloatv(GL_LINE_WIDTH, &s->line_width);
    glGetFloatv(GL_POINT_SIZE, &s->point_size);
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &s->alpha_func);
    glGetFloatv(GL_ALPHA_TEST_REF, &s->alpha_ref);
    glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, &s->sample_cov);
    glGetIntegerv(GL_SHADE_MODEL, &s->shade_model);
    glGetIntegerv(GL_LOGIC_OP_MODE, &s->logic_op);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_hints); i++) glGetIntegerv(snap_hints[i], &s->hints[i]);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_enables); i++) s->enables[i] = glIsEnabled(snap_enables[i]);

    for (int i = 0; i < 8; i++) {
        SnapLight *l = &s->light[i];
        GLenum n = GL_LIGHT0 + i;
        glGetLightfv(n, GL_AMBIENT, l->amb);
        glGetLightfv(n, GL_DIFFUSE, l->dif);
        glGetLightfv(n, GL_SPECULAR, l->spec);
        glGetLightfv(n, GL_POSITION, l->pos);
        glGetLightfv(n, GL_SPOT_DIRECTION, l->spot_dir);
        glGetLightfv(n, GL_SPOT_EXPONENT, &l->spot_exp);
        glGetLightfv(n, GL_SPOT_CUTOFF, &l->spot_cut);
        glGetLightfv(n, GL_CONSTANT_ATTENUATION, &l->att[0]);
        glGetLightfv(n, GL_LINEAR_ATTENUATION, &l->att[1]);
        glGetLightfv(n, GL_QUADRATIC_ATTENUATION, &l->att[2]);
    }
    glGetFloatv(GL_LIGHT_MODEL_AMBIENT, s->lm_amb);
    glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &s->lm_two);
    glGetMaterialfv(GL_FRONT, GL_AMBIENT, s->mat_amb);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, s->mat_dif);
    glGetMaterialfv(GL_FRONT, GL_SPECULAR, s->mat_spec);
    glGetMaterialfv(GL_FRONT, GL_EMISSION, s->mat_emi);
    glGetMaterialfv(GL_FRONT, GL_SHININESS, &s->mat_shin);
    glGetIntegerv(GL_FOG_MODE, &s->fog_mode);
    glGetFloatv(GL_FOG_DENSITY, &s->fog_density);
    glGetFloatv(GL_FOG_START, &s->fog_start);
    glGetFloatv(GL_FOG_END, &s->fog_end);
    glGetFloatv(GL_FOG_COLOR, s->fog_color);
    glGetFloatv(GL_CURRENT_COLOR, s->cur_color);
    glGetFloatv(GL_CURRENT_NORMAL, s->cur_normal);
    glGetFloatv(GL_POINT_SIZE_MIN, &s->pt_min);
    glGetFloatv(GL_POINT_SIZE_MAX, &s->pt_max);
    glGetFloatv(GL_POINT_DISTANCE_ATTENUATION, s->pt_att);
    glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, &s->pt_fade);
    for (int i = 0; i < 6; i++) {
        GLdouble eq[4];
        glGetClipPlane(GL_CLIP_PLANE0 + i, eq);
        for (int k = 0; k < 4; k++) s->clip[i][k] = eq[k];
    }

    glGetIntegerv(GL_MATRIX_MODE, &mode);
    snap_get_stack(GL_MODELVIEW, GL_MODELVIEW_STACK_DEPTH, GL_MODELVIEW_MATRIX,
                   &s->mv_depth, s->mv, SNAP_MV_DEPTH);
    snap_get_stack(GL_PROJECTION, GL_PROJECTION_STACK_DEPTH, GL_PROJECTION_MATRIX,
                   &s->proj_depth, s->proj, SNAP_AUX_DEPTH);

    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_tex);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &s->units);
    s->units = MIN(s->units, (GLint)GLES_MAX_TEXUNITS);
    for (int u = 0; u < s->units; u++) {
        SnapUnit *t = &s->unit[u];
        glActiveTexture(GL_TEXTURE0 + u);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &t->mode);
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, t->color);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &t->combine_rgb);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, &t->combine_a);
        for (int k = 0; k < 3; k++) {
            glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE0_RGB + k, &t->src_rgb[k]);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + k, &t->src_a[k]);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_RGB + k, &t->op_rgb[k]);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + k, &t->op_a[k]);
        }
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, &t->rgb_scale);
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, &t->a_scale);
        glGetTexEnviv(GL_POINT_SPRITE, GL_COORD_REPLACE, &t->coord_replace);
        glGetFloatv(GL_CURRENT_TEXTURE_COORDS, t->cur_tex);
        t->tex2d = glIsEnabled(GL_TEXTURE_2D);
        t->rect = glIsEnabled(GL_TEXTURE_RECTANGLE_ARB);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &t->bind2d);
        glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &t->bindrect);
        snap_get_stack(GL_TEXTURE, GL_TEXTURE_STACK_DEPTH, GL_TEXTURE_MATRIX,
                       &t->mat_depth, t->mat, SNAP_AUX_DEPTH);
    }
    glActiveTexture(s->active_tex);
    glMatrixMode(mode);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &s->fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &s->rb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    for (unsigned i = 1; i < GLES_MAX_ATTRIBS; i++) {
        glGetVertexAttribfv(i, GL_CURRENT_VERTEX_ATTRIB, s->vattr[i]);
    }
    while (glGetError() != GL_NO_ERROR) {
        /* pnames a context does not know leave their zero */
    }
}

static void snap_set_enable(GLenum cap, GLboolean on)
{
    if (on) glEnable(cap); else glDisable(cap);
}

static void snap_apply_gl(const GLESCtxGL *s)
{
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
    glDepthRange(s->depth_range[0], s->depth_range[1]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    glClearDepth(s->clear_depth);
    glClearStencil(s->clear_stencil);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glDepthMask(s->depth_mask);
    glStencilMaskSeparate(GL_FRONT, s->stencil_wm);
    glStencilMaskSeparate(GL_BACK, s->stencil_back_wm);
    glBlendFuncSeparate(s->blend_src_rgb, s->blend_dst_rgb, s->blend_src_a, s->blend_dst_a);
    glBlendEquationSeparate(s->blend_eq_rgb, s->blend_eq_a);
    glBlendColor(s->blend_color[0], s->blend_color[1], s->blend_color[2], s->blend_color[3]);
    glDepthFunc(s->depth_func);
    glCullFace(s->cull_mode);
    glFrontFace(s->front_face);
    for (int f = 0; f < 2; f++) {
        GLenum face = f ? GL_BACK : GL_FRONT;
        glStencilFuncSeparate(face, s->st[f][0], s->st[f][1], s->st[f][2]);
        glStencilOpSeparate(face, s->st[f][3], s->st[f][4], s->st[f][5]);
    }
    glPolygonOffset(s->poly_factor, s->poly_units);
    glLineWidth(s->line_width);
    glPointSize(s->point_size);
    glAlphaFunc(s->alpha_func, s->alpha_ref);
    glSampleCoverage(s->sample_cov, s->sample_inv);
    glShadeModel(s->shade_model);
    glLogicOp(s->logic_op);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_hints); i++) glHint(snap_hints[i], s->hints[i]);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_enables); i++) snap_set_enable(snap_enables[i], s->enables[i]);

    /* Positions, spot directions and clip planes were read in eye space; set
     * them under an identity modelview so they come back unchanged. */
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    for (int i = 0; i < 8; i++) {
        const SnapLight *l = &s->light[i];
        GLenum n = GL_LIGHT0 + i;
        glLightfv(n, GL_AMBIENT, l->amb);
        glLightfv(n, GL_DIFFUSE, l->dif);
        glLightfv(n, GL_SPECULAR, l->spec);
        glLightfv(n, GL_POSITION, l->pos);
        glLightfv(n, GL_SPOT_DIRECTION, l->spot_dir);
        glLightf(n, GL_SPOT_EXPONENT, l->spot_exp);
        glLightf(n, GL_SPOT_CUTOFF, l->spot_cut);
        glLightf(n, GL_CONSTANT_ATTENUATION, l->att[0]);
        glLightf(n, GL_LINEAR_ATTENUATION, l->att[1]);
        glLightf(n, GL_QUADRATIC_ATTENUATION, l->att[2]);
    }
    for (int i = 0; i < 6; i++) {
        GLdouble eq[4] = { s->clip[i][0], s->clip[i][1], s->clip[i][2], s->clip[i][3] };
        glClipPlane(GL_CLIP_PLANE0 + i, eq);
    }
    glPopMatrix();
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, s->lm_amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, s->lm_two);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, s->mat_amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, s->mat_dif);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, s->mat_spec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, s->mat_emi);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, s->mat_shin);
    glFogi(GL_FOG_MODE, s->fog_mode);
    glFogf(GL_FOG_DENSITY, s->fog_density);
    glFogf(GL_FOG_START, s->fog_start);
    glFogf(GL_FOG_END, s->fog_end);
    glFogfv(GL_FOG_COLOR, s->fog_color);
    glColor4fv(s->cur_color);
    glNormal3fv(s->cur_normal);
    glPointParameterf(GL_POINT_SIZE_MIN, s->pt_min);
    glPointParameterf(GL_POINT_SIZE_MAX, s->pt_max);
    glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, s->pt_att);
    glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, s->pt_fade);

    for (int u = 0; u < s->units; u++) {
        const SnapUnit *t = &s->unit[u];
        glActiveTexture(GL_TEXTURE0 + u);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, t->mode);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, t->color);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, t->combine_rgb);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, t->combine_a);
        for (int k = 0; k < 3; k++) {
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + k, t->src_rgb[k]);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + k, t->src_a[k]);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + k, t->op_rgb[k]);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + k, t->op_a[k]);
        }
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, t->rgb_scale);
        glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, t->a_scale);
        glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, t->coord_replace);
        glMultiTexCoord4fv(GL_TEXTURE0 + u, t->cur_tex);
        snap_set_enable(GL_TEXTURE_2D, t->tex2d);
        snap_set_enable(GL_TEXTURE_RECTANGLE_ARB, t->rect);
        glBindTexture(GL_TEXTURE_2D, t->bind2d);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, t->bindrect);
        snap_set_stack(GL_TEXTURE, GL_TEXTURE_STACK_DEPTH, t->mat_depth, (GLfloat (*)[16])t->mat);
    }
    glActiveTexture(s->active_tex ? s->active_tex : GL_TEXTURE0);
    snap_set_stack(GL_PROJECTION, GL_PROJECTION_STACK_DEPTH, s->proj_depth, (GLfloat (*)[16])s->proj);
    snap_set_stack(GL_MODELVIEW, GL_MODELVIEW_STACK_DEPTH, s->mv_depth, (GLfloat (*)[16])s->mv);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, s->fbo);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, s->rb);
    glUseProgram(s->program);
    for (unsigned i = 1; i < GLES_MAX_ATTRIBS; i++) glVertexAttrib4fv(i, s->vattr[i]);
    while (glGetError() != GL_NO_ERROR) {
    }
}

/* ---- per-namespace objects ------------------------------------------------ */

static void snap_put_map(SnapW *w, GHashTable *t, size_t value_size)
{
    GHashTableIter it;
    gpointer k, v;
    sw32(w, t ? g_hash_table_size(t) : 0);
    if (!t) return;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        sw32(w, GPOINTER_TO_UINT(k));
        if (value_size) swbytes(w, v, value_size); else sw32(w, GPOINTER_TO_UINT(v));
    }
}

/* value_size 0: the value is the pointer-sized integer itself. */
static GHashTable *snap_get_map(SnapR *r, GHashTable *t, size_t value_size)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t k = sr32(r), len;
        if (value_size) {
            const uint8_t *p = srbytes(r, &len);
            if (p && len == value_size) g_hash_table_replace(t, GUINT_TO_POINTER(k), g_memdup2(p, len));
        } else {
            g_hash_table_replace(t, GUINT_TO_POINTER(k), GUINT_TO_POINTER(sr32(r)));
        }
    }
    return t;
}

static void snap_put_buffers(SnapW *w, GHashTable *t)
{
    GHashTableIter it;
    gpointer k, v;
    sw32(w, t ? g_hash_table_size(t) : 0);
    if (!t) return;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESBuffer *b = v;
        sw32(w, GPOINTER_TO_UINT(k));
        swbytes(w, b->data, b->size);
    }
}

static void snap_get_buffers(SnapR *r, GHashTable *t)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t name = sr32(r), len;
        const uint8_t *p = srbytes(r, &len);
        GLESBuffer *b = g_new0(GLESBuffer, 1);
        b->name = name;
        b->refs = 1;
        b->size = len;
        b->data = p && len ? g_memdup2(p, len) : NULL;
        g_hash_table_replace(t, GUINT_TO_POINTER(name), b);
    }
}

static GLESBuffer *snap_buffer(GHashTable *t, uint32_t name)
{
    GLESBuffer *b = name && t ? g_hash_table_lookup(t, GUINT_TO_POINTER(name)) : NULL;
    if (b) b->refs++;
    return b;
}

static void snap_put_textures(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW tw = { body };
    uint32_t count = 0;
    GLint unpack = 4;

    for (GLuint name = 1, gap = 0; gap < SNAP_SCAN_GAP; name++) {
        GLenum target = GL_TEXTURE_2D;
        if (!glIsTexture(name)) { gap++; continue; }
        gap = 0;
        while (glGetError() != GL_NO_ERROR) {
        }
        glBindTexture(GL_TEXTURE_2D, name);
        if (glGetError() != GL_NO_ERROR) {
            target = GL_TEXTURE_RECTANGLE_ARB;
            glBindTexture(target, name);
            if (glGetError() != GL_NO_ERROR) {
                fprintf(stderr, "[gles] snapshot: texture %u has an unsupported target; dropped\n", name);
                continue;
            }
        }
        count++;
        sw32(&tw, name);
        sw32(&tw, target);
        {
            static const GLenum p[] = { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
                                        GL_TEXTURE_WRAP_T, GL_GENERATE_MIPMAP, GL_TEXTURE_MAX_LEVEL };
            for (unsigned i = 0; i < ARRAY_SIZE(p); i++) {
                GLint v = 0;
                glGetTexParameteriv(target, p[i], &v);
                sw32(&tw, v);
            }
        }
        for (GLint lvl = 0; lvl < 13; lvl++) {
            GLint wd = 0, ht = 0, ifmt = 0;
            glGetTexLevelParameteriv(target, lvl, GL_TEXTURE_WIDTH, &wd);
            glGetTexLevelParameteriv(target, lvl, GL_TEXTURE_HEIGHT, &ht);
            glGetTexLevelParameteriv(target, lvl, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            if (wd <= 0 || ht <= 0) break;
            g_autofree uint8_t *px = g_malloc((size_t)wd * ht * 4);
            glGetIntegerv(GL_PACK_ALIGNMENT, &unpack);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(target, lvl, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glPixelStorei(GL_PACK_ALIGNMENT, unpack);
            sw32(&tw, lvl + 1);
            sw32(&tw, wd);
            sw32(&tw, ht);
            sw32(&tw, ifmt);
            swbytes(&tw, px, (uint32_t)wd * ht * 4);
        }
        sw32(&tw, 0);
    }
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

/* Internal formats that can be recreated from RGBA8 bytes as they are;
 * anything else (compressed, host-private) comes back as RGBA. */
static GLint snap_texture_ifmt(GLint ifmt)
{
    switch (ifmt) {
    case GL_RGBA: case GL_RGB: case GL_ALPHA: case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA: case GL_RGBA8: case GL_RGB8: case GL_ALPHA8:
    case GL_LUMINANCE8: case GL_LUMINANCE8_ALPHA8: case GL_RGB5_A1: case GL_RGBA4:
        return ifmt;
    default:
        return GL_RGBA;
    }
}

static void snap_get_textures(SnapR *r)
{
    uint32_t n = sr32(r);
    GLint unpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        GLenum target = sr32(r);
        GLint p[6];
        for (int k = 0; k < 6; k++) p[k] = sr32(r);
        glBindTexture(target, name);
        for (uint32_t lvl; (lvl = sr32(r)) && !r->bad;) {
            GLint wd = sr32(r), ht = sr32(r), ifmt = sr32(r);
            uint32_t len;
            const uint8_t *px = srbytes(r, &len);
            if (px && len == (uint32_t)wd * ht * 4) {
                glTexImage2D(target, lvl - 1, snap_texture_ifmt(ifmt), wd, ht, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
        }
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, p[0]);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, p[1]);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, p[2]);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, p[3]);
        if (target == GL_TEXTURE_2D) {
            glTexParameteri(target, GL_GENERATE_MIPMAP, p[4]);
        }
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, p[5]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
}

static void snap_put_renderbuffers(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW bw = { body };
    uint32_t count = 0;
    for (GLuint name = 1, gap = 0; gap < SNAP_SCAN_GAP; name++) {
        GLint wd = 0, ht = 0, ifmt = 0;
        if (!glIsRenderbufferEXT(name)) { gap++; continue; }
        gap = 0;
        count++;
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, name);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_WIDTH_EXT, &wd);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_HEIGHT_EXT, &ht);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_INTERNAL_FORMAT_EXT, &ifmt);
        sw32(&bw, name);
        sw32(&bw, wd);
        sw32(&bw, ht);
        sw32(&bw, ifmt);
    }
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

static void snap_get_renderbuffers(SnapR *r)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        GLint wd = sr32(r), ht = sr32(r), ifmt = sr32(r);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, name);
        if (wd > 0 && ht > 0) {
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, ifmt, wd, ht);
        }
    }
}

static const GLenum snap_attachments[] = {
    GL_COLOR_ATTACHMENT0_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_STENCIL_ATTACHMENT_EXT,
};

static void snap_put_framebuffers(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW fw = { body };
    uint32_t count = 0;
    for (GLuint name = 1, gap = 0; gap < SNAP_SCAN_GAP; name++) {
        if (!glIsFramebufferEXT(name)) { gap++; continue; }
        gap = 0;
        count++;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, name);
        sw32(&fw, name);
        for (unsigned i = 0; i < ARRAY_SIZE(snap_attachments); i++) {
            GLint type = 0, obj = 0, lvl = 0, target = GL_TEXTURE_2D;
            glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, snap_attachments[i],
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &type);
            if (type == GL_TEXTURE || type == GL_RENDERBUFFER_EXT) {
                glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, snap_attachments[i],
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT, &obj);
            }
            if (type == GL_TEXTURE) {
                GLint rect = 0;
                glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, snap_attachments[i],
                    GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_EXT, &lvl);
                /* The texture target is not an attachment query in GL 2.1:
                 * a rectangle texture refuses a 2D bind. */
                glBindTexture(GL_TEXTURE_RECTANGLE_ARB, obj);
                rect = glGetError() == GL_NO_ERROR;
                glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
                target = rect ? GL_TEXTURE_RECTANGLE_ARB : GL_TEXTURE_2D;
            }
            sw32(&fw, type);
            sw32(&fw, obj);
            sw32(&fw, lvl);
            sw32(&fw, target);
        }
    }
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

static void snap_get_framebuffers(SnapR *r)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, name);
        for (unsigned k = 0; k < ARRAY_SIZE(snap_attachments); k++) {
            GLint type = sr32(r), obj = sr32(r), lvl = sr32(r), target = sr32(r);
            if (type == GL_TEXTURE) {
                glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, snap_attachments[k], target, obj, lvl);
            } else if (type == GL_RENDERBUFFER_EXT) {
                glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, snap_attachments[k],
                                             GL_RENDERBUFFER_EXT, obj);
            }
        }
    }
}

/* Shaders and programs: saved under their guest ids, recreated with new host
 * ids. Programs link against their shaders, so shaders go first. */
static void snap_put_glsl(SnapW *w)
{
    uint32_t *next;
    GHashTable **t = gles_glsl_table(&next);
    GHashTableIter it;
    gpointer k, v;

    sw32(w, *next);
    sw32(w, *t ? g_hash_table_size(*t) : 0);
    if (!*t) return;
    for (int pass = 0; pass < 2; pass++) {
        g_hash_table_iter_init(&it, *t);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            GLuint host = GPOINTER_TO_UINT(v);
            bool prog = glIsProgram(host);
            if (prog != (pass == 1)) continue;
            sw32(w, GPOINTER_TO_UINT(k));
            sw32(w, prog);
            if (!prog) {
                GLint type = 0, len = 0;
                glGetShaderiv(host, GL_SHADER_TYPE, &type);
                glGetShaderiv(host, GL_SHADER_SOURCE_LENGTH, &len);
                g_autofree char *src = g_malloc0(len + 1);
                if (len) glGetShaderSource(host, len + 1, NULL, src);
                sw32(w, type);
                swbytes(w, src, strlen(src));
                continue;
            }
            GLuint sh[16];
            GLsizei nsh = 0;
            GLint linked = 0, nattr = 0, nuni = 0;
            /* Attached shaders travel inline: apps delete their shaders
             * right after linking, so they are often gone from the name map
             * while the program still uses them. */
            glGetAttachedShaders(host, 16, &nsh, sh);
            sw32(w, nsh);
            for (GLsizei i = 0; i < nsh; i++) {
                GLint type = 0, len = 0;
                glGetShaderiv(sh[i], GL_SHADER_TYPE, &type);
                glGetShaderiv(sh[i], GL_SHADER_SOURCE_LENGTH, &len);
                g_autofree char *src = g_malloc0(len + 1);
                if (len) glGetShaderSource(sh[i], len + 1, NULL, src);
                sw32(w, type);
                swbytes(w, src, strlen(src));
            }
            glGetProgramiv(host, GL_LINK_STATUS, &linked);
            sw32(w, linked);
            glGetProgramiv(host, GL_ACTIVE_ATTRIBUTES, &nattr);
            sw32(w, linked ? nattr : 0);
            for (GLint i = 0; linked && i < nattr; i++) {
                char name[256];
                GLint size;
                GLenum type;
                glGetActiveAttrib(host, i, sizeof(name), NULL, &size, &type, name);
                swbytes(w, name, strlen(name));
                sw32(w, glGetAttribLocation(host, name));
            }
            /* Uniform values, one record per element, with the location the
             * guest was given (it caches locations; see snap_get_glsl). */
            GByteArray *body = g_byte_array_new();
            SnapW uw = { body };
            uint32_t count = 0;
            glGetProgramiv(host, GL_ACTIVE_UNIFORMS, &nuni);
            glUseProgram(host);
            for (GLint i = 0; linked && i < nuni; i++) {
                char name[256], el[272];
                GLint size;
                GLenum type;
                glGetActiveUniform(host, i, sizeof(name), NULL, &size, &type, name);
                char *br = strchr(name, '[');
                if (br) *br = 0;
                for (GLint e = 0; e < size; e++) {
                    snprintf(el, sizeof(el), size > 1 ? "%s[%d]" : "%s", name, e);
                    GLint loc = glGetUniformLocation(host, el);
                    GLfloat val[16] = { 0 };
                    if (loc < 0) continue;
                    if (type == GL_INT || type == GL_BOOL || type == GL_SAMPLER_2D ||
                        type == GL_SAMPLER_CUBE || type == GL_INT_VEC2 || type == GL_INT_VEC3 ||
                        type == GL_INT_VEC4 || type == GL_BOOL_VEC2 || type == GL_BOOL_VEC3 ||
                        type == GL_BOOL_VEC4) {
                        glGetUniformiv(host, loc, (GLint *)val);
                    } else {
                        glGetUniformfv(host, loc, val);
                    }
                    count++;
                    swbytes(&uw, el, strlen(el));
                    sw32(&uw, type);
                    sw32(&uw, gles_uloc_guest(GPOINTER_TO_UINT(k), loc));
                    swbytes(&uw, val, sizeof(val));
                }
            }
            sw32(w, count);
            g_byte_array_append(w->b, body->data, body->len);
            g_byte_array_free(body, true);
        }
    }
}

static void snap_set_uniform(GLint loc, GLenum type, const GLfloat *v)
{
    const GLint *iv = (const GLint *)v;
    switch (type) {
    case GL_FLOAT: glUniform1fv(loc, 1, v); break;
    case GL_FLOAT_VEC2: glUniform2fv(loc, 1, v); break;
    case GL_FLOAT_VEC3: glUniform3fv(loc, 1, v); break;
    case GL_FLOAT_VEC4: glUniform4fv(loc, 1, v); break;
    case GL_FLOAT_MAT2: glUniformMatrix2fv(loc, 1, GL_FALSE, v); break;
    case GL_FLOAT_MAT3: glUniformMatrix3fv(loc, 1, GL_FALSE, v); break;
    case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, 1, GL_FALSE, v); break;
    case GL_INT: case GL_BOOL: case GL_SAMPLER_2D: case GL_SAMPLER_CUBE:
        glUniform1iv(loc, 1, iv); break;
    case GL_INT_VEC2: case GL_BOOL_VEC2: glUniform2iv(loc, 1, iv); break;
    case GL_INT_VEC3: case GL_BOOL_VEC3: glUniform3iv(loc, 1, iv); break;
    case GL_INT_VEC4: case GL_BOOL_VEC4: glUniform4iv(loc, 1, iv); break;
    }
}

static void snap_get_glsl(SnapR *r)
{
    uint32_t *next;
    GHashTable **t = gles_glsl_table(&next);
    uint32_t n;

    *next = sr32(r);
    n = sr32(r);
    if (!*t) *t = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_remove_all(*t);
    if (*gles_uloc_table()) g_hash_table_remove_all(*gles_uloc_table());
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t guest = sr32(r), prog = sr32(r), len;
        if (!prog) {
            GLenum type = sr32(r);
            const uint8_t *src = srbytes(r, &len);
            g_autofree char *s = g_strndup((const char *)src, src ? len : 0);
            const char *p = s;
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &p, NULL);
            glCompileShader(sh);
            g_hash_table_insert(*t, GUINT_TO_POINTER(guest), GUINT_TO_POINTER(sh));
            continue;
        }
        GLuint pr = glCreateProgram(), shs[16];
        uint32_t nsh = sr32(r);
        for (uint32_t k = 0; k < nsh && !r->bad; k++) {
            GLenum type = sr32(r);
            const uint8_t *src = srbytes(r, &len);
            g_autofree char *s = g_strndup((const char *)src, src ? len : 0);
            const char *p = s;
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &p, NULL);
            glCompileShader(sh);
            glAttachShader(pr, sh);
            if (k < 16) shs[k] = sh;
        }
        uint32_t linked = sr32(r), nattr = sr32(r);
        for (uint32_t k = 0; k < nattr && !r->bad; k++) {
            const uint8_t *nm = srbytes(r, &len);
            g_autofree char *name = g_strndup((const char *)nm, nm ? len : 0);
            GLint loc = (GLint)sr32(r);
            if (loc >= 0) glBindAttribLocation(pr, loc, name);
        }
        if (linked) glLinkProgram(pr);
        for (uint32_t k = 0; k < MIN(nsh, 16); k++) glDeleteShader(shs[k]);  /* freed with the program */
        g_hash_table_insert(*t, GUINT_TO_POINTER(guest), GUINT_TO_POINTER(pr));
        uint32_t nuni = sr32(r);
        if (linked) glUseProgram(pr);
        for (uint32_t k = 0; k < nuni && !r->bad; k++) {
            const uint8_t *nm = srbytes(r, &len);
            g_autofree char *name = g_strndup((const char *)nm, nm ? len : 0);
            GLenum type = sr32(r);
            GLint old_loc = (GLint)sr32(r);
            const uint8_t *val = srbytes(r, &len);
            GLint loc = linked ? glGetUniformLocation(pr, name) : -1;
            if (loc >= 0 && val && len == 16 * sizeof(GLfloat)) {
                snap_set_uniform(loc, type, (const GLfloat *)val);
            }
            if (loc != old_loc && old_loc >= 0) {
                /* The guest keeps the location it was given. */
                g_hash_table_insert(gles_uloc_map(guest, true), GINT_TO_POINTER(old_loc + 1),
                                    GINT_TO_POINTER(loc + 1));
            }
        }
    }
}

/* All objects of the namespace current in the bound context. */
static void snap_put_objects(SnapW *w)
{
    snap_put_textures(w);
    snap_put_renderbuffers(w);
    snap_put_framebuffers(w);
    snap_put_glsl(w);
}

static void snap_get_objects(SnapR *r)
{
    snap_get_textures(r);
    snap_get_renderbuffers(r);
    snap_get_framebuffers(r);
    snap_get_glsl(r);
}

/* ---- GLESHost fields ------------------------------------------------------ */

static void snap_put_array(SnapW *w, const GLESArray *a)
{
    sw32(w, a->enabled);
    sw32(w, a->size);
    sw32(w, a->type);
    sw32(w, a->stride);
    sw32(w, a->ptr);
    sw32(w, a->vbo ? a->vbo->name : 0);
    sw32(w, a->client_state);
}

static void snap_get_array(SnapR *r, GLESArray *a, GHashTable *buffers)
{
    a->enabled = sr32(r);
    a->size = sr32(r);
    a->type = sr32(r);
    a->stride = sr32(r);
    a->ptr = sr32(r);
    a->vbo = snap_buffer(buffers, sr32(r));
    a->client_state = sr32(r);
}

static void snap_put_host(SnapW *w, GLESHost *h)
{
    sw32(w, h->inited);
    sw32(w, h->fbo);
    sw32(w, h->tex);
    sw32(w, h->depth);
    sw32(w, h->drawable_width);
    sw32(w, h->drawable_height);
    sw32(w, h->drawable_announced);
    snap_put_array(w, &h->vertex);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) snap_put_array(w, &h->texcoord[i]);
    sw32(w, h->client_active_unit);
    snap_put_array(w, &h->color);
    snap_put_array(w, &h->normal);
    snap_put_array(w, &h->pointsize);
    sw32(w, h->array_buffer ? h->array_buffer->name : 0);
    sw32(w, h->element_buffer ? h->element_buffer->name : 0);
    sw32(w, h->bound_renderbuffer);
    sw32(w, h->bound_framebuffer);
    snap_put_map(w, h->fbo_drawable, 0);
    sw32(w, h->unpack_alignment);
    sw32(w, h->unpack_row_bytes);
    sw32(w, h->pack_alignment);
    sw32(w, h->error);
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        snap_put_array(w, &h->attr[i]);
        sw32(w, h->attr_normalized[i]);
    }
    sw32(w, h->program);
    swbytes(w, &h->default_pvrtc, sizeof(h->default_pvrtc));
}

static void snap_get_host(SnapR *r, GLESHost *h)
{
    uint32_t len;
    h->inited = sr32(r);
    h->fbo = sr32(r);
    h->tex = sr32(r);
    h->depth = sr32(r);
    h->drawable_width = sr32(r);
    h->drawable_height = sr32(r);
    h->drawable_announced = sr32(r);
    snap_get_array(r, &h->vertex, h->buffers);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) snap_get_array(r, &h->texcoord[i], h->buffers);
    h->client_active_unit = sr32(r);
    snap_get_array(r, &h->color, h->buffers);
    snap_get_array(r, &h->normal, h->buffers);
    snap_get_array(r, &h->pointsize, h->buffers);
    h->array_buffer = snap_buffer(h->buffers, sr32(r));
    h->element_buffer = snap_buffer(h->buffers, sr32(r));
    h->bound_renderbuffer = sr32(r);
    h->bound_framebuffer = sr32(r);
    if (!h->fbo_drawable) h->fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    snap_get_map(r, h->fbo_drawable, 0);
    h->unpack_alignment = sr32(r);
    h->unpack_row_bytes = sr32(r);
    h->pack_alignment = sr32(r);
    h->error = sr32(r);
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        snap_get_array(r, &h->attr[i], h->buffers);
        h->attr_normalized[i] = sr32(r);
    }
    h->program = sr32(r);
    {
        const uint8_t *p = srbytes(r, &len);
        if (p && len == sizeof(h->default_pvrtc)) memcpy(&h->default_pvrtc, p, len);
    }
}

/* ---- sections ------------------------------------------------------------- */

/* The namespace-wide tables of a group, or of a context with no group. */
static void snap_put_tables(SnapW *w, GHashTable *buffers, GHashTable *surfaces,
                            GHashTable *rb_sized, GHashTable *pvrtc, uint32_t next_buffer)
{
    sw32(w, next_buffer);
    snap_put_buffers(w, buffers);
    snap_put_map(w, surfaces, sizeof(GLESSurface));
    snap_put_map(w, rb_sized, 0);
    snap_put_map(w, pvrtc, sizeof(GLESPVRTC));
}

static uint32_t snap_get_tables(SnapR *r, GHashTable *buffers, GHashTable *surfaces,
                                GHashTable *rb_sized, GHashTable *pvrtc)
{
    uint32_t next = sr32(r);
    snap_get_buffers(r, buffers);
    snap_get_map(r, surfaces, sizeof(GLESSurface));
    snap_get_map(r, rb_sized, 0);
    snap_get_map(r, pvrtc, sizeof(GLESPVRTC));
    return next;
}

/* Save one context: its own fields, and when it is the context that carries
 * its namespace's objects, those too. Leaves its GL state as it found it. */
static void snap_put_context(SnapW *w, uint32_t handle, GLESHost *h, bool objects)
{
    GLESCtxGL s;
    sw32(w, handle);
    sw32(w, h->group ? gles_group_handle(h->group) : 0);
    if (!h->group) {
        snap_put_tables(w, h->buffers, h->surfaces, h->rb_sized, h->pvrtc, h->next_buffer_name);
    }
    snap_put_host(w, h);
    sw32(w, h->inited && objects);
    if (!h->inited) return;
    gh_current = h;
    CGLSetCurrentContext(h->cgl);
    snap_capture_gl(&s);
    swbytes(w, &s, sizeof(s));
    if (objects) snap_put_objects(w);
    snap_apply_gl(&s);               /* the object scan moved bindings */
}

static uint32_t gles_group_handle(GLESGroup *group)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, gles_groups);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (v == group) return GPOINTER_TO_UINT(k);
    }
    return 0;
}

static GHashTable *snap_new_buffers(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, gles_buffer_destroy);
}
static GHashTable *snap_new_owned(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
}

static GByteArray *gles_snapshot_save(void)
{
    SnapW w = { g_byte_array_new() };
    GLESHost *was = gh_current;
    GHashTable *done = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter it;
    gpointer k, v;

    if (!gles_contexts) {
        gles_contexts = g_hash_table_new(g_direct_hash, g_direct_equal);
        gles_groups = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    sw32(&w, SNAP_MAGIC);
    sw32(&w, SNAP_VERSION);
    sw32(&w, sizeof(GLESCtxGL));
    sw32(&w, gles_handle);
    sw32(&w, g_hash_table_size(gles_groups));
    g_hash_table_iter_init(&it, gles_groups);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESGroup *g = v;
        sw32(&w, GPOINTER_TO_UINT(k));
        snap_put_tables(&w, g->buffers, g->surfaces, g->rb_sized, g->pvrtc, g->next_buffer_name);
    }
    sw32(&w, g_hash_table_size(gles_contexts) + 1);
    snap_put_context(&w, 0, &gh_legacy, true);
    g_hash_table_iter_init(&it, gles_contexts);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESHost *h = v;
        bool objects = h->inited && !g_hash_table_contains(done, h->group);
        if (objects) g_hash_table_add(done, h->group);
        snap_put_context(&w, GPOINTER_TO_UINT(k), h, objects);
    }
    g_hash_table_destroy(done);
    gh_current = was;
    CGLSetCurrentContext(NULL);     /* the vCPU thread makes its own current */
    return w.b;
}

static bool gles_snapshot_load(const uint8_t *p, size_t n)
{
    SnapR r = { p, n, 0, false };
    uint32_t ngroups, ncontexts;

    if (sr32(&r) != SNAP_MAGIC || sr32(&r) != SNAP_VERSION || sr32(&r) != sizeof(GLESCtxGL)) {
        fprintf(stderr, "[gles] snapshot: GL state from an incompatible build; not restored\n");
        return false;
    }
    gles_drop_all();
    if (!gles_contexts) {
        gles_contexts = g_hash_table_new(g_direct_hash, g_direct_equal);
        gles_groups = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    gles_handle = sr32(&r);
    ngroups = sr32(&r);
    for (uint32_t i = 0; i < ngroups && !r.bad; i++) {
        uint32_t handle = sr32(&r);
        GLESGroup *g = g_new0(GLESGroup, 1);
        g->refs = 1;
        g->buffers = snap_new_buffers();
        g->surfaces = snap_new_owned();
        g->rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
        g->pvrtc = snap_new_owned();
        g->next_buffer_name = snap_get_tables(&r, g->buffers, g->surfaces, g->rb_sized, g->pvrtc);
        g_hash_table_insert(gles_groups, GUINT_TO_POINTER(handle), g);
    }
    ncontexts = sr32(&r);
    for (uint32_t i = 0; i < ncontexts && !r.bad; i++) {
        uint32_t handle = sr32(&r), group = sr32(&r), len;
        GLESHost *h = handle ? g_new0(GLESHost, 1) : &gh_legacy;
        GLESCtxGL s;
        bool objects;

        if (group) {
            h->group = g_hash_table_lookup(gles_groups, GUINT_TO_POINTER(group));
            if (!h->group) { r.bad = true; break; }
            h->group->refs++;
            h->buffers = h->group->buffers;
            h->surfaces = h->group->surfaces;
            h->rb_sized = h->group->rb_sized;
            h->pvrtc = h->group->pvrtc;
        } else {
            h->buffers = snap_new_buffers();
            h->surfaces = snap_new_owned();
            h->rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
            h->pvrtc = snap_new_owned();
            h->next_buffer_name = snap_get_tables(&r, h->buffers, h->surfaces, h->rb_sized, h->pvrtc);
        }
        snap_get_host(&r, h);
        objects = sr32(&r);
        if (handle) {
            g_hash_table_insert(gles_contexts, GUINT_TO_POINTER(handle), h);
            qatomic_inc(&gles_live_contexts);
        }
        if (!h->inited) continue;
        const uint8_t *sp = srbytes(&r, &len);
        if (!sp || len != sizeof(s)) { r.bad = true; break; }
        memcpy(&s, sp, sizeof(s));
        gh_current = h;
        h->inited = false;
        if (!gles_platform_context_create()) {
            h->failed = true;
            fprintf(stderr, "[gles] snapshot: cannot recreate context %08x\n", handle);
            continue;
        }
        h->inited = true;
        h->readback = g_malloc0((size_t)GLES_FB_WIDTH * GLES_FB_HEIGHT * 4);
        if (!handle) qatomic_inc(&gles_live_contexts);
        if (objects) snap_get_objects(&r);
        snap_apply_gl(&s);
    }
    gh_current = &gh_legacy;
    CGLSetCurrentContext(NULL);
    if (r.bad) {
        fprintf(stderr, "[gles] snapshot: GL state truncated or corrupt\n");
        return false;
    }
    return true;
}

typedef struct {
    uint32_t len;
    uint8_t *data;
} GLESSnap;

static GLESSnap gles_snap;

static int gles_snap_pre_save(void *opaque)
{
    GByteArray *b = gles_snapshot_save();
    gles_snap.len = b->len;
    gles_snap.data = (uint8_t *)g_byte_array_free(b, false);
    return 0;
}

static int gles_snap_post_save(void *opaque)
{
    g_free(gles_snap.data);
    gles_snap.data = NULL;
    gles_snap.len = 0;
    return 0;
}

static int gles_snap_post_load(void *opaque, int version_id)
{
    bool ok = gles_snapshot_load(gles_snap.data, gles_snap.len);
    gles_snap_post_save(opaque);
    return ok ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_gles_host = {
    .name = "gles-host",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = gles_snap_pre_save,
    .post_save = gles_snap_post_save,
    .post_load = gles_snap_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(len, GLESSnap),
        VMSTATE_VBUFFER_ALLOC_UINT32(data, GLESSnap, 0, NULL, len),
        VMSTATE_END_OF_LIST()
    }
};

static void gles_snapshot_register(void)
{
    static bool done;
    if (!done) {
        done = true;
        vmstate_register(NULL, 0, &vmstate_gles_host, &gles_snap);
    }
}

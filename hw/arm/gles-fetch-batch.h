/* Conservative framebuffer-fetch scheduling. Capture the vertex shader's
 * actual clip positions, then preserve input order for every pair whose
 * expanded screen-space bounds intersect. Only independent primitives may
 * share a native draw/barrier. No guest shader assumptions or assets. */
#define GLES_FETCH_GRID 32
static uint64_t gles_fetch_batch_triangles, gles_fetch_batch_draws;

static bool gles_fetch_capture_supported(void)
{
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    return extensions && strstr(extensions, "GL_EXT_transform_feedback ");
}

/* Called with a complete host copy of the guest arrays and indices. The
 * capture is before clipping/culling and writes no framebuffer samples. */
static bool gles_fetch_batch_draw(uint32_t mode, uint32_t count,
                                  const uint8_t *indices, unsigned index_bytes)
{
    if (mode != GL_TRIANGLES || count < 72 || count > 262144 ||
        !gles_fetch_capture_supported()) {
        return false;
    }
    GLint program = 0, varyings = 0, viewport[4] = {0};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetProgramiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS_EXT, &varyings);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (varyings != 1 || viewport[2] <= 0 || viewport[3] <= 0 ||
        glIsEnabled(GL_RASTERIZER_DISCARD_EXT)) {
        return false;
    }
    unsigned triangles = count / 3;
    count = triangles * 3;
    size_t bytes = (size_t)count * 4 * sizeof(GLfloat);
    g_autofree GLfloat *positions = g_try_malloc(bytes);
    g_autofree GLuint *ordered = g_try_malloc((size_t)count * sizeof(GLuint));
    g_autofree unsigned *levels =
        g_try_malloc((size_t)triangles * sizeof(unsigned));
    g_autofree unsigned *offsets =
        g_try_malloc0(((size_t)triangles + 1) * sizeof(unsigned));
    if (!positions || !ordered || !levels || !offsets) {
        return false;
    }
    GLint general = 0, indexed = 0, start = 0, length = 0;
    glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING_EXT, &general);
    glGetIntegerIndexedvEXT(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING_EXT, 0,
                            &indexed);
    if (indexed) {
        glGetIntegerIndexedvEXT(GL_TRANSFORM_FEEDBACK_BUFFER_START_EXT, 0,
                                &start);
        glGetIntegerIndexedvEXT(GL_TRANSFORM_FEEDBACK_BUFFER_SIZE_EXT, 0,
                                &length);
    }
    if (bytes > gh.fetch_position_bytes) {
        if (!gles_budget_ok(gh.fetch_position_bytes, bytes)) {
            return false;
        }
        uint64_t previous;
        if (!gles_charge(gles_charge_table(true),
                         gles_charge_key(GLES_CHARGE_CAPTURE, 0, 0, 0), bytes,
                         &previous)) {
            return false;
        }
        if (!gh.fetch_position_buffer) {
            gh.fetch_position_buffer = gles_private_name(glIsBuffer);
        }
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT,
                     gh.fetch_position_buffer);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, bytes, NULL,
                     GL_STREAM_READ);
        if (glGetError() != GL_NO_ERROR) {
            gles_charge(gles_charge_table(true),
                        gles_charge_key(GLES_CHARGE_CAPTURE, 0, 0, 0), previous,
                        NULL);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, general);
            return false;
        }
        gh.fetch_position_bytes = bytes;
    }
    glBindBufferBaseEXT(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0,
                        gh.fetch_position_buffer);
    bool unlocked = gles_native_begin();
    glEnable(GL_RASTERIZER_DISCARD_EXT);
    glBeginTransformFeedbackEXT(GL_TRIANGLES);
    if (indices) {
        glDrawElements(GL_TRIANGLES, count,
                       index_bytes == 1 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT,
                       indices);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, count);
    }
    glEndTransformFeedbackEXT();
    glDisable(GL_RASTERIZER_DISCARD_EXT);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0, bytes, positions);
    GLenum error = glGetError();
    if (indexed && length > 0) {
        glBindBufferRangeEXT(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0, indexed,
                             start, length);
    } else {
        glBindBufferBaseEXT(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0, indexed);
    }
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, general);
    if (error) {
        gles_native_end(unlocked);
        return false;
    }

    unsigned grid[GLES_FETCH_GRID][GLES_FETCH_GRID] = {{0}};
    unsigned batches = 0;
    for (unsigned t = 0; t < triangles; t++) {
        double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
        bool finite = true;
        for (unsigned vertex = 0; vertex < 3; vertex++) {
            const GLfloat *p = positions + (t * 3 + vertex) * 4;
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) ||
                !isfinite(p[3]) || p[3] <= 0) {
                finite = false;
                break;
            }
            for (unsigned axis = 0; axis < 2; axis++) {
                double value = (double)p[axis] / p[3];
                lo[axis] = MIN(lo[axis], value);
                hi[axis] = MAX(hi[axis], value);
            }
        }
        unsigned low[2] = {0, 0},
                 high[2] = {GLES_FETCH_GRID - 1, GLES_FETCH_GRID - 1};
        if (finite) {
            for (unsigned axis = 0; axis < 2; axis++) {
                /* Expand by two pixels before quantizing: shared edges and
                 * rasterizer rounding must never become false independence. */
                double margin = 2.0 * GLES_FETCH_GRID / viewport[axis + 2];
                double a = (lo[axis] + 1) * (GLES_FETCH_GRID / 2.0) - margin;
                double b = (hi[axis] + 1) * (GLES_FETCH_GRID / 2.0) + margin;
                low[axis] =
                    (unsigned)MAX(0, MIN(GLES_FETCH_GRID - 1, floor(a)));
                high[axis] =
                    (unsigned)MAX(0, MIN(GLES_FETCH_GRID - 1, floor(b)));
            }
        }
        unsigned level = 0;
        for (unsigned y = low[1]; y <= high[1]; y++) {
            for (unsigned x = low[0]; x <= high[0]; x++) {
                level = MAX(level, grid[y][x]);
            }
        }
        for (unsigned y = low[1]; y <= high[1]; y++) {
            for (unsigned x = low[0]; x <= high[0]; x++) {
                grid[y][x] = level + 1;
            }
        }
        levels[t] = level;
        offsets[level + 1] += 3;
        batches = MAX(batches, level + 1);
    }
    for (unsigned b = 1; b <= batches; b++) {
        offsets[b] += offsets[b - 1];
    }
    /* Stable grouping: overlapping bounds strictly increase the level; any
     * reorder within a level is between primitives with disjoint pixels. */
    for (unsigned t = 0; t < triangles; t++) {
        unsigned out = offsets[levels[t]];
        offsets[levels[t]] += 3;
        for (unsigned v = 0; v < 3; v++) {
            unsigned i = t * 3 + v;
            ordered[out + v] = !indices           ? i
                               : index_bytes == 1 ? indices[i]
                                                  : lduw_le_p(indices + i * 2);
        }
    }
    unsigned first = 0;
    for (unsigned b = 0; b < batches; b++) {
        glTextureBarrierNV();
        glDrawElements(GL_TRIANGLES, offsets[b] - first, GL_UNSIGNED_INT,
                       ordered + first);
        first = offsets[b];
    }
    gles_native_end(unlocked);
    gles_fetch_batch_triangles += triangles;
    gles_fetch_batch_draws += batches;
    return true;
}

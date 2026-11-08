/* Context-local ES occlusion queries backed by desktop GPU sample queries. */
static bool gles_query_boolean(GLenum target)
{
    return target == 0x8c2f || target == 0x8d6a;
}
static bool gles_query_target(GLenum target)
{
    return gles_query_boolean(target) || target == GL_SAMPLES_PASSED;
}
static GLuint gles_query_combine(GLESQuery *query, GLuint value)
{
    return gles_query_boolean(query->target)
               ? !!(value || query->partial)
               : (GLuint)MIN((uint64_t)value + query->partial,
                             (uint64_t)UINT32_MAX);
}
static GLESQuery *gles_query_active(void)
{
    return gh.deleted_query ? gh.deleted_query
           : gh.queries ? g_hash_table_lookup(gh.queries,
                                              GUINT_TO_POINTER(gh.active_query))
                        : NULL;
}
/* GL 2.1 may forbid name allocation/deletion while a query is active. Split
 * its GPU work around those operations, preserving the accumulated result. */
static void gles_query_pause(void)
{
    GLESQuery *q = gles_query_active();
    if (q) {
        GLuint value = 0;
        glEndQuery(GL_SAMPLES_PASSED);
        glGetQueryObjectuiv(q->host, GL_QUERY_RESULT, &value);
        q->partial = gles_query_combine(q, value);
    }
}
static void gles_query_resume(void)
{
    GLESQuery *q = gles_query_active();
    if (q)
        glBeginQuery(GL_SAMPLES_PASSED, q->host);
}
static int64_t gles_query_call(CPUState *cpu, uint32_t slot, const uint32_t *a)
{
    if (!gh.queries)
        gh.queries =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    GLESQuery *query = g_hash_table_lookup(gh.queries, GUINT_TO_POINTER(a[0]));
    if (slot == 634 || slot == 635) {
        int32_t n = a[0];
        if (n < 0 || n > 4096)
            return gles_reject(GL_INVALID_VALUE);
        if (!n)
            return 0;
        if (slot == 635) {
            g_autofree GLuint *names = gles_es2_fetch(cpu, a[1], n);
            if (!names)
                return -1;
            for (int i = 0; i < n; i++) {
                query =
                    g_hash_table_lookup(gh.queries, GUINT_TO_POINTER(names[i]));
                if (!query)
                    continue;
                if (gh.active_query == names[i] && !gh.deleted_query) {
                    gh.deleted_query = query;
                    g_hash_table_steal(gh.queries, GUINT_TO_POINTER(names[i]));
                } else {
                    gles_query_pause();
                    glDeleteQueries(1, &query->host);
                    gles_query_resume();
                    g_hash_table_remove(gh.queries, GUINT_TO_POINTER(names[i]));
                }
            }
            return 0;
        }
        if (g_hash_table_size(gh.queries) + n > 16384)
            return gles_reject(GL_OUT_OF_MEMORY);
        g_autofree GLuint *ids = g_new0(GLuint, n), *hosts = g_new0(GLuint, n);
        gles_query_pause();
        glGenQueries(n, hosts);
        GLenum error = glGetError();
        gles_query_resume();
        if (error)
            return gles_reject(error);
        for (int i = 0; i < n; i++) {
            do {
                ids[i] = ++gh.next_query;
            } while (
                !ids[i] || ids[i] == gh.active_query ||
                g_hash_table_contains(gh.queries, GUINT_TO_POINTER(ids[i])));
        }
        if (!gles_es2_write(cpu, a[1], ids, n * 4)) {
            gles_query_pause();
            glDeleteQueries(n, hosts);
            gles_query_resume();
            return -1;
        }
        for (int i = 0; i < n; i++) {
            query = g_new0(GLESQuery, 1);
            query->host = hosts[i];
            g_hash_table_insert(gh.queries, GUINT_TO_POINTER(ids[i]), query);
        }
        return 0;
    }
    if (slot == 636)
        return query && query->begun;
    if (slot == 637) {
        query = g_hash_table_lookup(gh.queries, GUINT_TO_POINTER(a[1]));
        if (!gles_query_target(a[0]))
            return gles_reject(GL_INVALID_ENUM);
        if (!query || gh.active_query ||
            (query->target && query->target != a[0]))
            return gles_reject(GL_INVALID_OPERATION);
        glBeginQuery(GL_SAMPLES_PASSED, query->host);
        GLenum error = glGetError();
        if (error)
            return gles_reject(error);
        query->target = a[0];
        query->begun = true;
        query->ended = false;
        query->restored = false;
        query->partial = 0;
        gh.active_query = a[1];
        return 0;
    }
    if (slot == 638) {
        if (!gles_query_target(a[0]))
            return gles_reject(GL_INVALID_ENUM);
        query = gles_query_active();
        if (!query || query->target != a[0])
            return gles_reject(GL_INVALID_OPERATION);
        glEndQuery(GL_SAMPLES_PASSED);
        query->ended = true;
        gh.active_query = 0;
        if (gh.deleted_query) {
            glDeleteQueries(1, &query->host);
            g_free(query);
            gh.deleted_query = NULL;
        }
        return 0;
    }
    if (slot == 639) {
        if (!gles_query_target(a[0]))
            return gles_reject(GL_INVALID_ENUM);
        GLuint value = 0;
        if (a[1] == GL_CURRENT_QUERY) {
            query = gles_query_active();
            if (query && query->target == a[0])
                value = gh.active_query;
        } else if (a[1] == GL_QUERY_COUNTER_BITS) {
            GLint bits = 0;
            glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS, &bits);
            value = gles_query_boolean(a[0]) ? !!bits : bits;
        } else
            return gles_reject(GL_INVALID_ENUM);
        return gles_es2_write(cpu, a[2], &value, 4) ? 0 : -1;
    }
    if (slot == 640 || slot == 641) {
        if (a[1] != GL_QUERY_RESULT && a[1] != GL_QUERY_RESULT_AVAILABLE)
            return gles_reject(GL_INVALID_ENUM);
        if (!query || !query->ended || gh.active_query == a[0])
            return gles_reject(GL_INVALID_OPERATION);
        GLuint value = 0;
        if (query->restored)
            value = a[1] == GL_QUERY_RESULT_AVAILABLE ? 1 : query->saved;
        else {
            glGetQueryObjectuiv(query->host, a[1], &value);
            if (a[1] == GL_QUERY_RESULT)
                value = gles_query_combine(query, value);
        }
        if (slot == 640 && !gles_query_boolean(query->target))
            value = MIN(value, (GLuint)INT_MAX);
        return gles_es2_write(cpu, a[2], &value, 4) ? 0 : -1;
    }
    return -1;
}
static void gles_queries_free(GLESHost *host)
{
    if (!host->queries)
        return;
    if (host->active_query)
        glEndQuery(GL_SAMPLES_PASSED);
    if (host->deleted_query) {
        glDeleteQueries(1, &host->deleted_query->host);
        g_free(host->deleted_query);
        host->deleted_query = NULL;
    }
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, host->queries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        GLESQuery *q = value;
        glDeleteQueries(1, &q->host);
    }
    g_hash_table_destroy(host->queries);
    host->queries = NULL;
}

/* APPLE_clip_distance on the single-sample legacy desktop backend.
 * Keep the shader's distances, interpolate them on the GPU, and reject samples
 * before the guest fragment main. Only programs with real clip outputs acquire
 * the two packed varyings and private enable uniforms. */
static char *gles_clip_insert(const char *source, const char *prefix,
                              const char *suffix)
{
    size_t offset = gles_extension_prefix_end(source);
    if (g_str_has_prefix(source, "#version")) {
        const char *end = strchr(source, '\n');
        if (end)
            offset = MAX(offset, (size_t)(end + 1 - source));
    }
    GString *s = g_string_new_len(source, offset);
    g_string_append(s, prefix);
    g_string_append(s, source + offset);
    g_string_append(s, suffix);
    return g_string_free(s, false);
}

static char *gles_clip_vertex_glsl(const char *source)
{
    if (!strstr(source, "gl_ClipDistance"))
        return g_strdup(source);
    GRegex *decl = g_regex_new(
        "(?m)^[ \\t]*(?:varying[ \\t]+)?(?:(?:lowp|mediump|highp)[ \\t]+)?float[ "
        "\\t]+gl_ClipDistance[ \\t]*\\[[^\\]]*\\][ \\t]*;[ \\t]*$",
        0, 0, NULL);
    g_autofree char *clean =
        g_regex_replace_literal(decl, source, -1, 0, "", 0, NULL);
    g_regex_unref(decl);
    GRegex *ext = g_regex_new(
        "(?m)^[ \\t]*#extension[ \\t]+GL_APPLE_clip_distance[^\\n]*", 0, 0,
        NULL);
    g_autofree char *body =
        g_regex_replace_literal(ext, clean, -1, 0, "", 0, NULL);
    g_regex_unref(ext);
    GRegex *word = g_regex_new("\\bgl_ClipDistance\\b", 0, 0, NULL);
    g_autofree char *named = g_regex_replace_literal(
        word, body, -1, 0, "qemu_clip_distance", 0, NULL);
    g_regex_unref(word);
    GRegex *capability =
        g_regex_new("\\bGL_APPLE_clip_distance\\b", 0, 0, NULL);
    g_autofree char *capable = g_regex_replace_literal(
        capability, named, -1, 0, "QEMU_APPLE_clip_distance", 0, NULL);
    g_regex_unref(capability);
    GRegex *maximum = g_regex_new("\\bgl_MaxClipDistances\\b", 0, 0, NULL);
    g_autofree char *bounded =
        g_regex_replace_literal(maximum, capable, -1, 0, "8", 0, NULL);
    g_regex_unref(maximum);
    return gles_clip_insert(
        bounded,
        "#define main qemu_clip_vertex_main\n"
        "#define QEMU_APPLE_clip_distance 1\n"
        "float qemu_clip_distance[8];\n"
        "varying vec4 qemu_clip_varying0;\nvarying vec4 qemu_clip_varying1;\n",
        "\n#undef main\nvoid main(){\n"
        "for(int i=0;i<8;i++)qemu_clip_distance[i]=1.0;\n"
        "qemu_clip_vertex_main();\n"
        "qemu_clip_varying0=vec4(qemu_clip_distance[0],qemu_clip_distance[1],"
        "qemu_clip_distance[2],qemu_clip_distance[3]);\n"
        "qemu_clip_varying1=vec4(qemu_clip_distance[4],qemu_clip_distance[5],"
        "qemu_clip_distance[6],qemu_clip_distance[7]);\n}\n");
}

static char *gles_clip_fragment_glsl(const char *source)
{
    return gles_clip_insert(
        source,
        "#define main qemu_clip_fragment_main\n"
        "varying vec4 qemu_clip_varying0;\nvarying vec4 qemu_clip_varying1;\n"
        "uniform vec4 qemu_clip_enabled0;\nuniform vec4 qemu_clip_enabled1;\n",
        "\n#undef main\nvoid main(){\n"
        "if(any(lessThan(mix(vec4(1.0),qemu_clip_varying0,qemu_clip_enabled0),vec4(0.0)))||"
        "any(lessThan(mix(vec4(1.0),qemu_clip_varying1,qemu_clip_enabled1),vec4(0.0))))discard;\n"
        "qemu_clip_fragment_main();}\n");
}

static char *gles_clip_source(GLuint shader)
{
    GLint length = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    if (length <= 0 || length > GLES_MAX_STRING * 2)
        return NULL;
    char *source = g_malloc0(length + 1);
    glGetShaderSource(shader, length, NULL, source);
    return source;
}

static void gles_clip_link(GLuint program)
{
    GLuint shaders[16], fragment = 0, adapted = 0;
    GLsizei count = 0;
    bool clips = false;
    glGetAttachedShaders(program, ARRAY_SIZE(shaders), &count, shaders);
    for (unsigned i = 0; i < count; i++) {
        GLint type;
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        if (type == GL_FRAGMENT_SHADER)
            fragment = shaders[i];
        if (type == GL_VERTEX_SHADER) {
            g_autofree char *source = gles_clip_source(shaders[i]);
            clips |= source && strstr(source, "qemu_clip_vertex_main");
        }
    }
    if (clips && fragment) {
        g_autofree char *source = gles_clip_source(fragment);
        if (source) {
            g_autofree char *text = gles_clip_fragment_glsl(source);
            const char *p = text;
            adapted = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(adapted, 1, &p, NULL);
            glCompileShader(adapted);
            glDetachShader(program, fragment);
            glAttachShader(program, adapted);
        }
    }
    glLinkProgram(program);
    if (adapted) {
        glDetachShader(program, adapted);
        glAttachShader(program, fragment);
        glDeleteShader(adapted);
    }
}

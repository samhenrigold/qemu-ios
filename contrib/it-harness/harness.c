/* Reuse the working ARMv6 runtime dispatch and GLES fixture, without changing
 * GLTest's independent regression app. Frameworks cannot be linked by modern ld
 * against this SDK; see ../it-gles/glapp.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>
#define main gltest_reference_main
#include "../it-gles/glapp.c"
#undef main

extern int dlclose(void *handle);

static id_ owner, menu, output, field, stage, badge, audio, movie, connection;
static id_ ticker;
static void *gl;
static char documents[1024], transcript[24000];
static unsigned ticks, frames, received, sensor_samples;
static long http_status;
static double started;
static int mode, suspended;
static const char *audio_name;

#define CALL(ret, args) ((ret (*) args)p_objc_msgSend)
static const char *utf8(id_ value) { return (const char *)m0(value, S("UTF8String")); }
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
static Rect_ rect(float x, float y, float width, float height) { Rect_ r = {{x,y},{width,height}}; return r; }

static void report(const char *format, ...)
{
    char line[1600], path[1200];
    va_list ap;
    va_start(ap, format); vsnprintf(line, sizeof(line), format, ap); va_end(ap);
    fprintf(stderr, "[Harness] %s\n", line);
    if (strlen(transcript) + strlen(line) + 2 >= sizeof(transcript)) transcript[0] = 0;
    strcat(transcript, line); strcat(transcript, "\n");
    if (output) m1(output, S("setText:"), nsstr(transcript));
    snprintf(path, sizeof(path), "%s/results.log", documents);
    FILE *f = fopen(path, "a");
    if (f) {
        int failed = fprintf(f, "%.3f %s\n", now(), line) < 0;
        if (fclose(f) != 0) failed = 1;
        if (failed && output) m1(output, S("setText:"), nsstr("FAIL: saving results.log; storage error"));
    } else if (output) m1(output, S("setText:"), nsstr("FAIL: opening results.log; storage error"));
}

static id_ view(const char *cls, id_ parent, Rect_ frame)
{
    id_ v = mrect(m0(C(cls), S("alloc")), S("initWithFrame:"), frame);
    m1(parent, S("addSubview:"), v); m0(v, S("release")); return v;
}

static void button(id_ parent, const char *title, int tag, Rect_ frame)
{
    id_ b = m1u(C("UIButton"), S("buttonWithType:"), 1);
    mrect(b, S("setFrame:"), frame);
    CALL(void, (id_, SEL_, id_, unsigned))(b, S("setTitle:forState:"), nsstr(title), 0);
    m1u(b, S("setTag:"), tag);
    CALL(void, (id_, SEL_, id_, SEL_, unsigned))(b, S("addTarget:action:forControlEvents:"), owner, S("select:"), 64);
    m1(parent, S("addSubview:"), b);
}

static void storage(int create)
{
    char path[1200], temporary[1200];
    unsigned char block[4096], readback[4096];
    snprintf(path, sizeof(path), "%s/persistence.bin", documents);
    snprintf(temporary, sizeof(temporary), "%s/persistence.tmp", documents);
    for (unsigned i = 0; i < sizeof(block); ++i) block[i] = (i * 73 + i / 251) & 255;
    double begin = now();
    int ok = 1;
    FILE *f;
    if (create) {
        f = fopen(temporary, "wb");
        if (!f) { report("FAIL storage create: %s", strerror(errno)); return; }
        for (int i = 0; i < 256 && ok; ++i) ok = fwrite(block, 1, sizeof(block), f) == sizeof(block);
        if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok && rename(temporary, path) != 0) ok = 0;
        if (!ok) { report("FAIL storage write/fsync/rename: %s", strerror(errno)); return; }
    }
    f = fopen(path, "rb");
    if (!f) { report("%s persistence marker: %s", create ? "FAIL" : "NOT RUN (write marker first)", strerror(errno)); return; }
    for (int i = 0; i < 256 && ok; ++i)
        ok = fread(readback, 1, sizeof(readback), f) == sizeof(readback) && !memcmp(block, readback, sizeof(block));
    if (fgetc(f) != EOF || ferror(f)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    report("%s %s: 1 MiB byte comparison, %.2fs", ok ? "PASS" : "FAIL", create ? "write/fsync/rename/read" : "saved marker", now() - begin);
    if (create) report("MANUAL: quit, cleanly reboot, then Verify saved marker.");
}

static void memory_test(void)
{
    const unsigned count = 1024 * 1024;
    unsigned *p = malloc(count * sizeof(*p));
    if (!p) { report("FAIL allocating 4 MiB"); return; }
    double begin = now();
    unsigned bad = 0;
    for (unsigned i = 0; i < count; ++i) p[i] = i ^ 0xa5c39e71u;
    for (unsigned i = 0; i < count; ++i) if (p[i] != (i ^ 0xa5c39e71u)) ++bad;
    free(p);
    volatile double sum = 0;
    for (unsigned i = 1; i <= 10000; ++i) sum += 1.0 / i;
    report("%s CPU/memory: 4 MiB, FP %.9f, %.2fs", !bad && fabs(sum - 9.787606036) < 1e-8 ? "PASS" : "FAIL", sum, now()-begin);
}

static void preferences(void)
{
    id_ defaults = m0(C("NSUserDefaults"), S("standardUserDefaults"));
    id_ key = nsstr("HarnessLaunchCount");
    unsigned previous = CALL(unsigned, (id_, SEL_, id_))(defaults, S("integerForKey:"), key);
    CALL(void, (id_, SEL_, unsigned, id_))(defaults, S("setInteger:forKey:"), previous + 1, key);
    int ok = CALL(int, (id_, SEL_))(defaults, S("synchronize"));
    report("%s preferences sync; launch count %u (check after relaunch)", ok ? "PASS" : "FAIL", previous + 1);
}

static void stop_tests(void)
{
    if (mode == 1) report("INFO GLES stopped after %u frames", frames);
    mode = 0;
    m0(stage, S("removeFromSuperview")); stage = 0; badge = 0;
    m0(audio, S("stop")); m0(audio, S("release")); audio = 0;
    m0(movie, S("stop")); m0(movie, S("release")); movie = 0;
    if (connection) { m0(connection, S("cancel")); m0(connection, S("release")); connection = 0; report("CANCELLED network"); }
    m1(m0(C("UIAccelerometer"), S("sharedAccelerometer")), S("setDelegate:"), 0);
    m1u(menu, S("setHidden:"), 0); m1u(output, S("setHidden:"), 0);
    m0(field, S("resignFirstResponder"));
}

static void visual_stage(void)
{
    m1u(menu, S("setHidden:"), 1); m1u(output, S("setHidden:"), 1);
    stage = view("UIView", g_window, rect(0, 60, 320, 420));
    m1(stage, S("setBackgroundColor:"), m0(C("UIColor"), S("blackColor")));
}

static int init_gl(void)
{
    if (g_gl_ready) return 1;
    if (g_ctx) { report("FAIL prior GL setup failed; relaunch to retry"); return 0; }
    gl = resolve_gl();
    if (!gl) return 0;
    g_view = mrect(m0(C("GLTestView"), S("alloc")), S("initWithFrame:"), rect(40, 0, VIEW_W, VIEW_H));
    /* Retain this view between runs so its drawable/context can be reused. */
    m1(stage, S("addSubview:"), g_view);
    g_ctx = m1u(m0(C("EAGLContext"), S("alloc")), S("initWithAPI:"), 1);
    if (!g_ctx || !m1(C("EAGLContext"), S("setCurrentContext:"), g_ctx)) return 0;
    p_glGenRenderbuffersOES(1, &g_rb);
    p_glBindRenderbufferOES(GL_RENDERBUFFER_OES, g_rb);
    if (!m2ru(g_ctx, S("renderbufferStorage:fromDrawable:"), GL_RENDERBUFFER_OES, m0(g_view, S("layer")))) return 0;
    p_glGenFramebuffersOES(1, &g_fb);
    p_glBindFramebufferOES(GL_FRAMEBUFFER_OES, g_fb);
    p_glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, g_rb);
    g_gl_ready = p_glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES;
    return g_gl_ready;
}

static void gl_frame(void)
{
    if (!m1(C("EAGLContext"), S("setCurrentContext:"), g_ctx)) {
        report("FAIL GLES: cannot make context current"); stop_tests(); return;
    }
    /* ponytail: resident stack vertices until the bridge can fault file-backed
     * guest pages in; the CPU must touch client data before submission. */
    float quad[] = {0,0, 120,0, 0,360, 120,360};
    p_glBindFramebufferOES(GL_FRAMEBUFFER_OES,g_fb);
    p_glViewport(0,0,VIEW_W,VIEW_H);
    p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity();
    p_glOrthof(0,VIEW_W,0,VIEW_H,-1,1);
    p_glMatrixMode(GL_MODELVIEW); p_glLoadIdentity();
    p_glClearColor(1,0,1,1); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glColor4f(0,1,1,1); p_glEnableClientState(GL_VERTEX_ARRAY);
    p_glVertexPointer(2,GL_FLOAT,0,quad); p_glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    void (*rotate)(float,float,float,float) = dlsym(gl, "glRotatef");
    void (*translate)(float,float,float) = dlsym(gl, "glTranslatef");
    float triangle[] = {-65,-60, 65,-60, 0,75};
    translate(120,180,0); rotate((float)(now()-started)*60,0,0,1);
    p_glColor4f(1,1,1,1); p_glVertexPointer(2,GL_FLOAT,0,triangle); p_glDrawArrays(4,0,3);
    if (!frames) {
        unsigned char left[4] = {0}, right[4] = {0};
        void (*read_pixels)(int,int,int,int,unsigned,unsigned,void *) = dlsym(gl,"glReadPixels");
        read_pixels(10,10,1,1,0x1908,0x1401,left);
        read_pixels(230,10,1,1,0x1908,0x1401,right);
        report("%s GLES pixel readback: L=%u,%u,%u R=%u,%u,%u",
            left[0]<32 && left[1]>223 && left[2]>223 && right[0]>223 && right[1]<32 && right[2]>223 ? "PASS" : "FAIL",
            left[0],left[1],left[2],right[0],right[1],right[2]);
    }
    unsigned error = p_glGetError();
    p_glBindRenderbufferOES(GL_RENDERBUFFER_OES, g_rb);
    int presented = m1uI(g_ctx, S("presentRenderbuffer:"), GL_RENDERBUFFER_OES);
    if (error || !presented) { report("FAIL GLES: error 0x%x, present %d", error, presented); stop_tests(); return; }
    if (++frames == 1) report("PASS GLES framebuffer/draw/present API; MANUAL: white rotating triangle on cyan/magenta, black border.");
    if (frames % 60 == 0) {
        char text[100]; snprintf(text,sizeof(text),"%u frames | %.1f fps",frames,frames/(now()-started));
        m1(badge,S("setText:"),nsstr(text));
    }
}

static id_ resource(const char *name)
{
    id_ base = m0(m0(C("NSBundle"), S("mainBundle")), S("bundlePath"));
    return m1(C("NSURL"), S("fileURLWithPath:"), m1(base, S("stringByAppendingPathComponent:"), nsstr(name)));
}

static void play_audio(const char *name)
{
    id_ error = 0;
    audio_name = name;
    audio = CALL(id_, (id_, SEL_, id_, id_ *))(m0(C("AVAudioPlayer"), S("alloc")), S("initWithContentsOfURL:error:"), resource(name), &error);
    m1(audio, S("setDelegate:"), owner);
    CALL(void, (id_, SEL_, float))(audio, S("setVolume:"), 0.5f);
    int ok = CALL(int, (id_, SEL_))(audio, S("play"));
    report("%s audio %s%s%s", ok ? "RUNNING" : "FAIL", name, error ? ": " : "", error ? utf8(m0(error,S("localizedDescription"))) : "");
    if (ok) report("MANUAL: 6 seconds; left 440 Hz, right 880 Hz. Pause/resume and volume below.");
    /* AudioServices.h (3.1.3): 'rout' is the current CFString audio route. */
    void *toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    int (*get_property)(unsigned, unsigned *, void *) = toolbox ? dlsym(toolbox, "AudioSessionGetProperty") : 0;
    if (get_property) {
        id_ route = 0;
        unsigned length = sizeof(route);
        int status = get_property('rout', &length, &route);
        report("INFO audio route: status=%d %s", status, route ? utf8(route) : "unknown");
    }
    if (toolbox) dlclose(toolbox);
}

static void audio_finished(id_ self, SEL_ cmd, id_ player, signed char ok)
{ report("%s audio completion: %s (audibility requires listening)", ok ? "PASS" : "FAIL", audio_name); }
static void audio_error(id_ self, SEL_ cmd, id_ player, id_ error)
{ report("FAIL audio: %s", utf8(m0(error,S("localizedDescription")))); }
static void movie_event(id_ self, SEL_ cmd, id_ note)
{
    if (m0(note,S("object")) != movie) return;
    id_ error = m1(m0(note,S("userInfo")),S("objectForKey:"),nsstr("error"));
    report("%s movie %s%s%s", error ? "FAIL" : "INFO", utf8(m0(note,S("name"))), error ? ": " : "", error ? utf8(m0(error,S("localizedDescription"))) : " (verify picture/sound manually)");
}

static void network_done(void)
{ m0(connection,S("release")); connection = 0; }
static void net_response(id_ self, SEL_ cmd, id_ conn, id_ response)
{ http_status = CALL(long,(id_,SEL_))(response,S("statusCode")); received = 0; }
static void net_data(id_ self, SEL_ cmd, id_ conn, id_ data)
{
    unsigned n = CALL(unsigned,(id_,SEL_))(data,S("length"));
    if (n > 1024*1024 - received) { m0(connection,S("cancel")); report("FAIL network: response exceeds 1 MiB limit"); network_done(); return; }
    received += n;
}
static void net_finish(id_ self, SEL_ cmd, id_ conn)
{ report("%s HTTP %ld, %u bytes, %.2fs", http_status >= 200 && http_status < 300 && received ? "PASS" : "FAIL", http_status, received, now()-started); network_done(); }
static void net_error(id_ self, SEL_ cmd, id_ conn, id_ error)
{ report("FAIL network: %s",utf8(m0(error,S("localizedDescription")))); network_done(); }
static void networking(void)
{
    id_ url = m1(C("NSURL"),S("URLWithString:"),m0(field,S("text")));
    const char *scheme = utf8(m0(url,S("scheme")));
    if (!url || !scheme || (strcmp(scheme,"http") && strcmp(scheme,"https")) || !m0(url,S("host"))) { report("FAIL: enter an http:// or https:// URL"); return; }
    id_ request = CALL(id_,(id_,SEL_,id_,unsigned,double))(C("NSURLRequest"),S("requestWithURL:cachePolicy:timeoutInterval:"),url,1,15.0);
    started = now(); received = 0; http_status = 0;
    connection = CALL(id_,(id_,SEL_,id_,id_))(m0(C("NSURLConnection"),S("alloc")),S("initWithRequest:delegate:"),request,owner);
    report("%s HTTP GET: %s",connection ? "RUNNING" : "FAIL",utf8(m0(field,S("text"))));
}

static void acceleration(id_ self, SEL_ cmd, id_ accel, id_ value)
{
    ++sensor_samples;
    char text[200];
    snprintf(text,sizeof(text),"Tilt #%u  x %.2f  y %.2f  z %.2f",sensor_samples,
        CALL(double,(id_,SEL_))(value,S("x")),CALL(double,(id_,SEL_))(value,S("y")),CALL(double,(id_,SEL_))(value,S("z")));
    m1(badge,S("setText:"),nsstr(text));
    if (sensor_samples == 1) report("PASS accelerometer callback; MANUAL: change host tilt and check values.");
}

static void tick(id_ self, SEL_ cmd, id_ timer)
{
    if (suspended) return;
    ++ticks;
    if (mode == 1) gl_frame();
    if (mode == 3 && !sensor_samples && now()-started > 5) { report("NOT OBSERVED: no accelerometer samples after 5s"); mode = 0; }
    if (connection && now()-started > 20) { m0(connection,S("cancel")); report("FAIL network: 20s overall deadline"); network_done(); }
}

static void select_test(id_ self, SEL_ cmd, id_ sender)
{
    int tag = CALL(int,(id_,SEL_))(sender,S("tag"));
    if (tag == 19) { report("MANUAL PASS: %s",utf8(m0(field,S("text")))); return; }
    if (tag == 20) { report("MANUAL FAIL: %s",utf8(m0(field,S("text")))); return; }
    if (tag == 15) { if (audio) { if (CALL(int,(id_,SEL_))(audio,S("isPlaying"))) m0(audio,S("pause")); else m0(audio,S("play")); report("INFO audio pause/resume"); } return; }
    if (tag == 16 || tag == 17) { CALL(void,(id_,SEL_,float))(audio,S("setVolume:"),tag == 16 ? 0.1f : 0.8f); report("MANUAL: audio volume %s",tag == 16 ? "10%" : "80%"); return; }
    stop_tests();
    switch (tag) {
    case 0: report("INFO stopped; menu restored"); break;
    case 1:
        visual_stage();
        if (!init_gl()) { report("FAIL GLES setup"); stop_tests(); break; }
        m1(stage,S("addSubview:"),g_view);
        badge = view("UILabel",stage,rect(20,365,280,35));
        mode = 1; frames = 0; started = now(); break;
    case 2:
        visual_stage();
        badge = view("UIView",stage,rect(15,100,70,70));
        m1(badge,S("setBackgroundColor:"),m0(C("UIColor"),S("orangeColor")));
        CALL(void,(id_,SEL_,id_,void *))(C("UIView"),S("beginAnimations:context:"),nsstr("HarnessMotion"),0);
        CALL(void,(id_,SEL_,double))(C("UIView"),S("setAnimationDuration:"),1.5);
        CALL(void,(id_,SEL_,float))(C("UIView"),S("setAnimationRepeatCount:"),1000.0f);
        m1u(C("UIView"),S("setAnimationRepeatAutoreverses:"),1);
        mrect(badge,S("setFrame:"),rect(235,270,70,70));
        CALL(void,(id_,SEL_,float))(badge,S("setAlpha:"),0.25f);
        m0(C("UIView"),S("commitAnimations"));
        report("MANUAL animation: orange square moves diagonally and fades every 1.5s."); break;
    case 3: storage(1); break;
    case 4: storage(0); break;
    case 5: memory_test(); break;
    case 6: networking(); break;
    case 7: play_audio("stereo.wav"); break;
    case 8: play_audio("aac.m4a"); break;
    case 9: play_audio("tone.mp3"); break;
    case 10: play_audio("lossless.m4a"); break;
    case 11: case 12:
        movie = m1(m0(C("MPMoviePlayerController"),S("alloc")),S("initWithContentURL:"),resource(tag == 11 ? "h264.mp4" : "mpeg4.mp4"));
        if (!movie) { report("FAIL movie initialization"); break; }
        report("MANUAL %s: moving test pattern, stereo sound, seek/pause/Done controls.",tag == 11 ? "H.264" : "MPEG-4");
        m0(movie,S("play")); break;
    case 13:
        visual_stage(); badge = view("UILabel",stage,rect(5,100,310,60));
        CALL(void,(id_,SEL_,id_))(badge,S("setFont:"),CALL(id_,(id_,SEL_,float))(C("UIFont"),S("systemFontOfSize:"),12.0f));
        m1(badge,S("setText:"),nsstr("Waiting for accelerometer..."));
        sensor_samples = 0; mode = 3; started = now();
        CALL(void,(id_,SEL_,double))(m0(C("UIAccelerometer"),S("sharedAccelerometer")),S("setUpdateInterval:"),0.1);
        m1(m0(C("UIAccelerometer"),S("sharedAccelerometer")),S("setDelegate:"),owner); break;
    case 14:
        m1u(field,S("setKeyboardType:"),0); m0(field,S("becomeFirstResponder"));
        report("MANUAL input: edit top field, tap/scroll menu, use keyboard. Test labels are VoiceOver labels."); break;
    case 21: {
        id_ query = m0(C("MPMediaQuery"), S("songsQuery"));
        id_ items = m0(query, S("items"));
        if (!query || !items) {
            report("MEDIA unavailable: music library service did not return items");
            break;
        }
        unsigned count = CALL(unsigned,(id_,SEL_))(items,S("count"));
        id_ first = count ? m1u(items,S("objectAtIndex:"),0) : 0;
        const char *title = utf8(m1(first,S("valueForProperty:"),nsstr("title")));
        report("MEDIA songs=%u first=%s",count,title ? title : "");
        break;
    }
    case 18:
        storage(1); memory_test();
        { id_ pb = m0(C("UIPasteboard"),S("generalPasteboard"));
          id_ saved = m0(m0(pb,S("items")),S("copy"));
          m1(pb,S("setString:"),nsstr("Harness clipboard 123"));
          report("%s clipboard string round trip",!strcmp(utf8(m0(pb,S("string"))) ?: "","Harness clipboard 123") ? "PASS" : "FAIL");
          m1(pb,S("setItems:"),saved ? saved : m0(C("NSArray"),S("array"))); m0(saved,S("release")); }
        report("INFO automatic checks complete; GL/media/input require individual tests."); break;
    }
}

static void inactive(id_ self, SEL_ cmd, id_ app)
{ suspended = 1; report("INFO lifecycle: resign active"); }
static void active(id_ self, SEL_ cmd, id_ app)
{ suspended = 0; report("INFO lifecycle: become active"); }
static void terminating(id_ self, SEL_ cmd, id_ app)
{ report("INFO lifecycle: terminate"); stop_tests(); }
static signed char field_return(id_ self, SEL_ cmd, id_ textfield)
{ m0(textfield,S("resignFirstResponder")); return 1; }

static void launch(id_ self, SEL_ cmd, id_ app)
{
    owner = self;
    const char *home = getenv("HOME");
    if (!home || snprintf(documents,sizeof(documents),"%s/Documents",home) >= (int)sizeof(documents)) _exit(2);
    if (mkdir(documents,0700) && errno != EEXIST) _exit(2);
    g_window = mrect(m0(C("UIWindow"),S("alloc")),S("initWithFrame:"),rect(0,0,320,480));
    m1(g_window,S("setBackgroundColor:"),m0(C("UIColor"),S("whiteColor")));
    button(g_window,"Stop / Back",0,rect(5,23,100,32));
    field = view("UITextField",g_window,rect(110,25,205,30));
    m1(field,S("setText:"),nsstr("http://10.0.2.2:8000/"));
    m1u(field,S("setBorderStyle:"),3); m1u(field,S("setAutocorrectionType:"),1);
    m1u(field,S("setAutocapitalizationType:"),0); m1u(field,S("setReturnKeyType:"),9);
    m1(field,S("setDelegate:"),self);
    menu = view("UIScrollView",g_window,rect(0,60,320,255));
    const char *titles[] = {"GL: rotating triangle", "Core Animation", "Write + verify 1 MiB", "Verify saved marker", "CPU + memory", "HTTP GET (top URL)", "Audio: stereo PCM", "Audio: AAC", "Audio: MP3", "Audio: ALAC", "Video: H.264", "Video: MPEG-4", "Tilt / accelerometer", "Touch + keyboard", "Audio pause/resume", "Volume 10%", "Volume 80%", "Run automatic checks", "Record manual PASS", "Record manual FAIL", "Media library: count songs"};
    for (unsigned i=0;i<sizeof(titles)/sizeof(*titles);++i) button(menu,titles[i],i+1,rect(10, i*42,300,38));
    CALL(void,(id_,SEL_,Size_))(menu,S("setContentSize:"),(Size_){320,42 * sizeof(titles) / sizeof(*titles)});
    output = view("UITextView",g_window,rect(5,320,310,155));
    m1u(output,S("setEditable:"),0);
    m1(output,S("setFont:"),CALL(id_,(id_,SEL_,float))(C("UIFont"),S("systemFontOfSize:"),12.0f));
    m0(g_window,S("makeKeyAndVisible"));
    report("Harness 1.0 | iOS %s | %s",utf8(m0(m0(C("UIDevice"),S("currentDevice")),S("systemVersion"))),documents);
    report("Results append to Documents/results.log. Top field is URL or manual-test note.");
    preferences(); storage(0);
    id_ center = m0(C("NSNotificationCenter"),S("defaultCenter"));
    const char *notifications[] = {"MPMoviePlayerContentPreloadDidFinishNotification","MPMoviePlayerPlaybackDidFinishNotification"};
    for (int i=0;i<2;++i) CALL(void,(id_,SEL_,id_,SEL_,id_,id_))(center,S("addObserver:selector:name:object:"),self,S("movieEvent:"),nsstr(notifications[i]),0);
    ticker = mtimer(C("NSTimer"),S("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),1.0/30,self,S("tick:"),0,1);
}

int main(void)
{
    if (!resolve_objc()) _exit(1);
    const char *frameworks[] = {"Foundation","UIKit","QuartzCore","AVFoundation","MediaPlayer"};
    void *uikit = 0;
    for (unsigned i=0;i<5;++i) {
        char path[256]; snprintf(path,sizeof(path),"/System/Library/Frameworks/%s.framework/%s",frameworks[i],frameworks[i]);
        void *h = dlopen(path,RTLD_NOW);
        if (!h) { fprintf(stderr,"Harness: %s: %s\n",path,dlerror()); _exit(1); }
        if (i == 1) uikit = h;
    }
    id_ pool = m0(m0(C("NSAutoreleasePool"),S("alloc")),S("init"));
    Class_ cls = p_objc_allocateClassPair(C("NSObject"),"HarnessDelegate",0);
#define METHOD(name, fn, types) p_class_addMethod(cls,S(name),(void *)fn,types)
    METHOD("applicationDidFinishLaunching:",launch,"v@:@");
    METHOD("applicationWillResignActive:",inactive,"v@:@");
    METHOD("applicationDidBecomeActive:",active,"v@:@");
    METHOD("applicationWillTerminate:",terminating,"v@:@");
    METHOD("select:",select_test,"v@:@"); METHOD("tick:",tick,"v@:@");
    METHOD("textFieldShouldReturn:",field_return,"c@:@");
    METHOD("audioPlayerDidFinishPlaying:successfully:",audio_finished,"v@:@c");
    METHOD("audioPlayerDecodeErrorDidOccur:error:",audio_error,"v@:@@");
    METHOD("movieEvent:",movie_event,"v@:@");
    METHOD("connection:didReceiveResponse:",net_response,"v@:@@");
    METHOD("connection:didReceiveData:",net_data,"v@:@@");
    METHOD("connectionDidFinishLoading:",net_finish,"v@:@");
    METHOD("connection:didFailWithError:",net_error,"v@:@@");
    METHOD("accelerometer:didAccelerate:",acceleration,"v@:@@");
    p_objc_registerClassPair(cls);
    cls = p_objc_allocateClassPair(C("UIView"),"GLTestView",0);
    p_class_addMethod(p_object_getClass(cls),S("layerClass"),(void *)view_layer_class,"#@:");
    p_objc_registerClassPair(cls);
    int (*run)(int,char **,id_,id_) = dlsym(uikit,"UIApplicationMain");
    if (!run) _exit(1);
    char *argv[] = {"Harness",0}; run(1,argv,0,nsstr("HarnessDelegate"));
    m0(pool,S("drain")); _exit(0);
}

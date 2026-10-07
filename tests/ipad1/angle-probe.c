#include <assert.h>
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>
static void pixel(unsigned r,unsigned g) { unsigned char p[4];glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p); assert(glGetError()==GL_NO_ERROR);printf("pixel %u %u %u %u\n",p[0],p[1],p[2],p[3]);assert(p[0]==r&&p[1]==g&&p[2]==0&&p[3]==255); }
int main(void) {
 PFNEGLGETPLATFORMDISPLAYEXTPROC get=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");assert(get);
 EGLint attrs[]={EGL_PLATFORM_ANGLE_TYPE_ANGLE,EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,EGL_NONE};
 EGLDisplay d=get(EGL_PLATFORM_ANGLE_ANGLE,(void *)0,attrs);assert(d!=EGL_NO_DISPLAY);assert(eglInitialize(d,0,0));
 assert(eglBindAPI(EGL_OPENGL_ES_API));
 EGLint cfgattrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES_BIT|EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 EGLConfig cfg;EGLint n;assert(eglChooseConfig(d,cfgattrs,&cfg,1,&n)&&n);
 EGLint surfaceattrs[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};EGLSurface s=eglCreatePbufferSurface(d,cfg,surfaceattrs);assert(s!=EGL_NO_SURFACE);
 for(int version=1;version<=2;version++) {
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,version,EGL_NONE};EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);assert(c!=EGL_NO_CONTEXT);assert(eglMakeCurrent(d,s,s,c));
  printf("ES%d: %s; %s\n",version,glGetString(GL_VERSION),glGetString(GL_RENDERER));glViewport(0,0,64,64);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  GLfloat vertices[]={-1,-1,1,-1,0,1};
  if(version==1) {glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glColor4f(1,0,0,1);glEnableClientState(GL_VERTEX_ARRAY);glVertexPointer(2,GL_FLOAT,0,vertices);glDrawArrays(GL_TRIANGLES,0,3);pixel(255,0);}
  else {
   const char *vs="attribute vec2 a; void main(){gl_Position=vec4(a,0.0,1.0);}";
   const char *fs="precision mediump float;void main(){gl_FragColor=vec4(0.0,1.0,0.0,1.0);}";
   GLuint shaders[]={glCreateShader(GL_VERTEX_SHADER),glCreateShader(GL_FRAGMENT_SHADER)};const char *src[]={vs,fs};GLint ok;
   for(int i=0;i<2;i++){glShaderSource(shaders[i],1,&src[i],0);glCompileShader(shaders[i]);glGetShaderiv(shaders[i],GL_COMPILE_STATUS,&ok);assert(ok);}
   GLuint p=glCreateProgram();glAttachShader(p,shaders[0]);glAttachShader(p,shaders[1]);glBindAttribLocation(p,0,"a");glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(p);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices);glEnableVertexAttribArray(0);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,255);
   glDeleteProgram(p);for(int i=0;i<2;i++)glDeleteShader(shaders[i]);
  }
  assert(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));assert(eglDestroyContext(d,c));
 }
 assert(eglDestroySurface(d,s));assert(eglTerminate(d));puts("PASS ANGLE Metal ES1 and unmodified ES2 shader draw/readback");
}

// Multi-context / multi-thread probe for nvc0 deferred submission.
// Context A draws without flushing, then context B is created on another
// thread; both keep rendering and verify their own results.
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static EGLDisplay dpy;
static int failures;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static EGLContext make_ctx(void)
{
   eglBindAPI(EGL_OPENGL_API);
   EGLint attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
   EGLContext c = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attr);
   if (!c || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      fprintf(stderr, "ctx failed 0x%x\n", eglGetError());
      _exit(2);
   }
   return c;
}

static GLuint fbo(void)
{
   GLuint rb, fb;
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   glViewport(0, 0, 32, 32);
   return fb;
}

static GLuint prog(void)
{
   const char *vs = "#version 330\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}";
   const char *fs = "#version 330\nuniform vec4 c;out vec4 o;void main(){o=c;}";
   GLuint p = glCreateProgram(), s;
   s = glCreateShader(GL_VERTEX_SHADER); glShaderSource(s, 1, &vs, NULL); glCompileShader(s); glAttachShader(p, s);
   s = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(s, 1, &fs, NULL); glCompileShader(s); glAttachShader(p, s);
   glLinkProgram(p);
   GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
   glUseProgram(p);
   return p;
}

static void render_check(GLuint p, int id, int iter)
{
   uint8_t want[4] = { (uint8_t)(id * 100 + 20), (uint8_t)(iter & 0xff), (uint8_t)(255 - id * 80), 255 };
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glUniform4f(glGetUniformLocation(p, "c"), want[0] / 255.f, want[1] / 255.f, want[2] / 255.f, 1);
   for (int i = 0; i < 20; i++)
      glDrawArrays(GL_TRIANGLES, 0, 3);
   uint8_t got[32 * 32 * 4];
   glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, got);
   for (int i = 0; i < 32 * 32; i++)
      for (int c = 0; c < 4; c++)
         if (abs(got[i * 4 + c] - want[c]) > 1) {
            pthread_mutex_lock(&mtx);
            if (failures++ < 5)
               printf("ctx %d iter %d px %d: got %d,%d,%d want %d,%d,%d\n", id, iter, i,
                      got[i*4], got[i*4+1], got[i*4+2], want[0], want[1], want[2]);
            pthread_mutex_unlock(&mtx);
            return;
         }
}

static void *thread_b(void *arg)
{
   make_ctx();
   GLuint p = prog();
   fbo();
   for (int i = 0; i < 400; i++)
      render_check(p, 1, i);
   return NULL;
}

int main(void)
{
   dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   eglInitialize(dpy, NULL, NULL);
   eglBindAPI(EGL_OPENGL_API);
   make_ctx();
   printf("renderer: %s\n", glGetString(GL_RENDERER));
   GLuint p = prog();
   fbo();
   /* leave deferred work pending in context A */
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   pthread_t t;
   pthread_create(&t, NULL, thread_b, NULL);
   for (int i = 0; i < 400; i++)
      render_check(p, 0, i);
   pthread_join(t, NULL);
   printf("failures=%d\n", failures);
   return failures ? 1 : 0;
}

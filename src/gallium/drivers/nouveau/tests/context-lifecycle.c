// Stress: thread A uploads + draws + verifies continuously while thread B
// keeps creating, using and destroying contexts on the same screen.
// Reproduces cross-thread pushbuf races in nvc0 submission handling.
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static EGLDisplay dpy;
static atomic_int stop, failures, iters_a, iters_b;

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

static GLuint setup(int sz, GLuint *tex)
{
   const char *vs = "#version 330\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}";
   const char *fs = "#version 330\nuniform sampler2D s;out vec4 o;void main(){o=texelFetch(s,ivec2(gl_FragCoord.xy),0);}";
   GLuint p = glCreateProgram(), s;
   s = glCreateShader(GL_VERTEX_SHADER); glShaderSource(s, 1, &vs, NULL); glCompileShader(s); glAttachShader(p, s);
   s = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(s, 1, &fs, NULL); glCompileShader(s); glAttachShader(p, s);
   glLinkProgram(p);
   GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
   glUseProgram(p);
   GLuint rb, fb;
   glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, sz, sz);
   glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   glViewport(0, 0, sz, sz);
   glGenTextures(1, tex); glBindTexture(GL_TEXTURE_2D, *tex);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, sz, sz);
   return p;
}

static void *thread_a(void *arg)
{
   const int sz = 64;
   make_ctx();
   GLuint tex;
   setup(sz, &tex);
   uint32_t *in = malloc(sz * sz * 4), *out = malloc(sz * sz * 4);
   for (int it = 0; !atomic_load(&stop); it++) {
      for (int i = 0; i < sz * sz; i++)
         in[i] = (uint32_t)(i * 2654435761u + it * 40503u) | 0xff000000u;
      /* several small uploads (inline pushbuf data) and draws per check */
      for (int k = 0; k < 8; k++) {
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, k * 8, sz, 8, GL_RGBA, GL_UNSIGNED_BYTE, in + k * 8 * sz);
         glDrawArrays(GL_TRIANGLES, 0, 3);
      }
      glReadPixels(0, 0, sz, sz, GL_RGBA, GL_UNSIGNED_BYTE, out);
      for (int i = 0; i < sz * sz; i++)
         if (in[i] != out[i]) {
            if (atomic_fetch_add(&failures, 1) < 5)
               printf("A iter %d px %d: got %08x want %08x\n", it, i, out[i], in[i]);
            break;
         }
      atomic_fetch_add(&iters_a, 1);
   }
   return NULL;
}

static void *thread_b(void *arg)
{
   for (int n = 0; n < 150 && !atomic_load(&stop); n++) {
      EGLContext c = make_ctx();
      GLuint tex;
      setup(16, &tex);
      uint32_t px[16 * 16];
      for (int i = 0; i < 256; i++) px[i] = 0xff00ff00u | i;
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(dpy, c);
      atomic_fetch_add(&iters_b, 1);
      usleep(2000);
   }
   return NULL;
}

int main(void)
{
   dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   eglInitialize(dpy, NULL, NULL);
   pthread_t a, b;
   pthread_create(&a, NULL, thread_a, NULL);
   usleep(200000);
   pthread_create(&b, NULL, thread_b, NULL);
   pthread_join(b, NULL);
   atomic_store(&stop, 1);
   pthread_join(a, NULL);
   printf("iters A=%d B=%d failures=%d\n", iters_a, iters_b, failures);
   return failures ? 1 : 0;
}

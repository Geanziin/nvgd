/* SPDX-License-Identifier: MIT
 * Regression for clear callbacks receiving plain pipe_surface templates.
 * cc -Wall -Wextra -Werror surface-clear.c -lEGL -lGL -o surface-clear
 * Run offscreen with the candidate libgallium on LD_LIBRARY_PATH.
 */
#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { W = 96, H = 96, LAYERS = 3 };

static void check_gl(void)
{
   GLenum error = glGetError();
   if (error != GL_NO_ERROR) {
      fprintf(stderr, "GL error: 0x%x\n", error);
      exit(2);
   }
}

int main(void)
{
   EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                               EGL_DEFAULT_DISPLAY, NULL);
   if (!eglInitialize(display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
      return 2;
   const EGLint attributes[] = {
      EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
      EGL_NONE
   };
   EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR,
                                         EGL_NO_CONTEXT, attributes);
   if (context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
      return 2;
   printf("renderer: %s\n", glGetString(GL_RENDERER));

   unsigned bad = 0;
   GLuint texture;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
   glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, W, H, LAYERS);
   const uint8_t initial[] = {51, 102, 153, 255};
   const uint8_t partial[] = {255, 0, 0, 255};
   glClearTexImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial);
   glClearTexSubImage(texture, 0, 50, 50, 1, 20, 30, 1,
                      GL_RGBA, GL_UNSIGNED_BYTE, partial);
   uint8_t rgba[LAYERS][H][W][4];
   glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
   check_gl();
   for (unsigned layer = 0; layer < LAYERS; layer++)
      for (unsigned y = 0; y < H; y++)
         for (unsigned x = 0; x < W; x++) {
            const uint8_t *expected = layer == 1 && x >= 50 && x < 70 &&
                                       y >= 50 && y < 80 ? partial : initial;
            for (unsigned c = 0; c < 4; c++)
               bad += abs((int)rgba[layer][y][x][c] - expected[c]) > 1;
         }
   glDeleteTextures(1, &texture);

   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
   glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH24_STENCIL8, W, H, LAYERS);
   const uint32_t ds_initial = 0x40000003, ds_partial = 0x80000007;
   glClearTexImage(texture, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8,
                   &ds_initial);
   glClearTexSubImage(texture, 0, 10, 20, 1, 33, 17, 1,
                      GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, &ds_partial);
   uint32_t depth_stencil[LAYERS][H][W];
   glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, depth_stencil);
   check_gl();
   for (unsigned layer = 0; layer < LAYERS; layer++)
      for (unsigned y = 0; y < H; y++)
         for (unsigned x = 0; x < W; x++) {
            uint32_t expected = layer == 1 && x >= 10 && x < 43 &&
                                 y >= 20 && y < 37 ? ds_partial : ds_initial;
            uint32_t actual = depth_stencil[layer][y][x];
            bad += (actual & 255) != (expected & 255);
            bad += abs((int)(actual >> 8) - (int)(expected >> 8)) > 1;
         }
   glDeleteTextures(1, &texture);
   glFinish();
   check_gl();
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglTerminate(display);
   printf("color/depth/stencil partial clears, 3 layers: bad_components=%u\n", bad);
   return bad != 0;
}

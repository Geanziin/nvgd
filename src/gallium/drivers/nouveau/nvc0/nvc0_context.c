/*
 * Copyright 2010 Christoph Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "pipe/p_defines.h"
#include "util/u_draw.h"
#include "util/u_framebuffer.h"
#include "util/u_upload_mgr.h"
#include "util/u_threaded_context.h"

#include "nvc0/nvc0_context.h"
#include "nvc0/nvc0_screen.h"
#include "nvc0/nvc0_resource.h"


#include "xf86drm.h"
#include "drm-uapi/nouveau_drm.h"


static void
nvc0_svm_migrate(struct pipe_context *pipe, unsigned num_ptrs,
                 const void* const* ptrs, const size_t *sizes,
                 bool to_device, bool mem_undefined)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nouveau_screen *screen = &nvc0->screen->base;
   int fd = screen->drm->fd;
   unsigned i;

   for (i = 0; i < num_ptrs; i++) {
      struct drm_nouveau_svm_bind args;
      uint64_t cmd, prio, target;

      args.va_start = (uint64_t)(uintptr_t)ptrs[i];
      if (sizes && sizes[i]) {
         args.va_end = (uint64_t)(uintptr_t)ptrs[i] + sizes[i];
         args.npages = DIV_ROUND_UP(args.va_end - args.va_start, 0x1000);
      } else {
         args.va_end = 0;
         args.npages = 0;
      }
      args.stride = 0;

      args.reserved0 = 0;
      args.reserved1 = 0;

      prio = 0;
      cmd = NOUVEAU_SVM_BIND_COMMAND__MIGRATE;
      target = to_device ? NOUVEAU_SVM_BIND_TARGET__GPU_VRAM : 0;

      args.header = cmd << NOUVEAU_SVM_BIND_COMMAND_SHIFT;
      args.header |= prio << NOUVEAU_SVM_BIND_PRIORITY_SHIFT;
      args.header |= target << NOUVEAU_SVM_BIND_TARGET_SHIFT;

      /* This is best effort, so no garanty whatsoever */
      drmCommandWrite(fd, DRM_NOUVEAU_SVM_BIND,
                      &args, sizeof(args));
   }
}


/* Submit work another context left deferred in its pushbuf.  It must reach
 * the GPU before anything recorded under a different context, otherwise it
 * would execute on top of that context's 3D state.  Deferred work always ends
 * at a state_lock release, so it is a complete command sequence. */
static void
nvc0_kick_deferred_locked(struct nvc0_screen *screen)
{
   struct nvc0_context *prev = screen->deferred_ctx;

   simple_mtx_assert_locked(&screen->state_lock);
   screen->deferred_ctx = NULL;
   screen->deferred_ops = 0;
   if (prev)
      PUSH_KICK(prev->base.pushbuf);
}

/* Take the screen state lock for a 3D/compute operation. */
void
nvc0_state_lock(struct nvc0_context *nvc0)
{
   struct nvc0_screen *screen = nvc0->screen;

   simple_mtx_lock(&screen->state_lock);
   if (unlikely(screen->deferred_ctx && screen->deferred_ctx != nvc0))
      nvc0_kick_deferred_locked(screen);
}

/* Long frames would otherwise leave the GPU idle until flush time while the
 * CPU records hundreds of draws.  Once a minimum batch is recorded, submit it
 * as soon as the GPU has drained everything it was given (the fence page
 * shows the last emitted sequence), so recording and execution overlap
 * without paying a submission per draw.  defer_batch caps the batch size. */
static inline bool
nvc0_should_kick_deferred(struct nvc0_screen *screen)
{
   const unsigned ops = ++screen->deferred_ops;

   if (screen->defer_batch && ops >= screen->defer_batch)
      return true;
   if (ops < screen->defer_idle_min || !screen->defer_idle_min)
      return false;
   return screen->base.fence.update(&screen->base.base) ==
          screen->base.fence.sequence;
}

void
nvc0_state_unlock_kick(struct nvc0_context *nvc0)
{
   struct nvc0_screen *screen = nvc0->screen;

   simple_mtx_assert_locked(&screen->state_lock);

   /* On desktop, buffers shared through dma-buf are synchronised by the kernel
    * only once the commands touching them are submitted (Chromium/ANGLE/Discord).
    * On Android, the back buffer is always an imported dma-buf from Gralloc,
    * which is presented explicitly at eglSwapBuffers() - holding work back
    * across draws is safe and essential to prevent ioctl flooding.
    * Multi-context deferral is safe because nvc0_state_lock() flushes any
    * pending work from another context before that context records commands. */
   bool can_defer = screen->defer_kicks && !nvc0_should_kick_deferred(screen);
   if (can_defer && !screen->defer_imported &&
       nouveau_pushbuf_refs_imported(nvc0->base.pushbuf))
      can_defer = false;

   if (can_defer) {
      screen->deferred_ctx = nvc0;
   } else {
      PUSH_KICK(nvc0->base.pushbuf);
      screen->deferred_ops = 0;
      if (screen->deferred_ctx == nvc0)
         screen->deferred_ctx = NULL;
   }
   simple_mtx_unlock(&screen->state_lock);
}

static void
nvc0_flush(struct pipe_context *pipe,
           struct pipe_fence_handle **fence,
           unsigned flags)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nvc0_screen *screen = nvc0->screen;

   if (fence)
      nouveau_fence_ref(nvc0->base.fence, (struct nouveau_fence **)fence,
                        nvc0->base.screen);

   simple_mtx_lock(&screen->state_lock);
   PUSH_KICK(nvc0->base.pushbuf); /* fencing handled in kick_notify */
   screen->deferred_ops = 0;
   if (screen->deferred_ctx == nvc0)
      screen->deferred_ctx = NULL;
   simple_mtx_unlock(&screen->state_lock);

   nouveau_context_update_frame_stats(&nvc0->base);
}

static void
nvc0_texture_barrier(struct pipe_context *pipe, unsigned flags)
{
   struct nouveau_pushbuf *push = nvc0_context(pipe)->base.pushbuf;

   IMMED_NVC0(push, NVC0_3D(SERIALIZE), 0);
   IMMED_NVC0(push, NVC0_3D(TEX_CACHE_CTL), 0);
}

static void
nvc0_memory_barrier(struct pipe_context *pipe, unsigned flags)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   int i, s;

   if (!(flags & ~PIPE_BARRIER_UPDATE))
      return;

   if (flags & PIPE_BARRIER_MAPPED_BUFFER) {
      for (i = 0; i < nvc0->num_vtxbufs; ++i) {
         if (!nvc0->vtxbuf[i].buffer.resource && !nvc0->vtxbuf[i].is_user_buffer)
            continue;
         if (nvc0->vtxbuf[i].buffer.resource->flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT)
            nvc0->base.vbo_dirty = true;
      }

      for (s = 0; s < 5 && !nvc0->cb_dirty; ++s) {
         uint32_t valid = nvc0->constbuf_valid[s];

         while (valid && !nvc0->cb_dirty) {
            const unsigned i = ffs(valid) - 1;
            struct pipe_resource *res;

            valid &= ~(1 << i);
            if (nvc0->constbuf[s][i].user)
               continue;

            res = nvc0->constbuf[s][i].u.buf;
            if (!res)
               continue;

            if (res->flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT)
               nvc0->cb_dirty = true;
         }
      }
   } else {
      /* Pretty much any writing by shaders needs a serialize after
       * it. Especially when moving between 3d and compute pipelines, but even
       * without that.
       */
      IMMED_NVC0(push, NVC0_3D(SERIALIZE), 0);
   }

   /* If we're going to texture from a buffer/image written by a shader, we
    * must flush the texture cache.
    */
   if (flags & PIPE_BARRIER_TEXTURE)
      IMMED_NVC0(push, NVC0_3D(TEX_CACHE_CTL), 0);

   if (flags & PIPE_BARRIER_CONSTANT_BUFFER)
      nvc0->cb_dirty = true;
   if (flags & (PIPE_BARRIER_VERTEX_BUFFER | PIPE_BARRIER_INDEX_BUFFER))
      nvc0->base.vbo_dirty = true;
}

static void
nvc0_emit_string_marker(struct pipe_context *pipe, const char *str, int len)
{
   struct nouveau_pushbuf *push = nvc0_context(pipe)->base.pushbuf;
   int string_words = len / 4;
   int data_words;

   if (len <= 0)
      return;
   string_words = MIN2(string_words, NV04_PFIFO_MAX_PACKET_LEN);
   if (string_words == NV04_PFIFO_MAX_PACKET_LEN)
      data_words = string_words;
   else
      data_words = string_words + !!(len & 3);
   BEGIN_NIC0(push, SUBC_3D(NV04_GRAPH_NOP), data_words);
   if (string_words)
      PUSH_DATAp(push, str, string_words);
   if (string_words != data_words) {
      int data = 0;
      memcpy(&data, &str[string_words * 4], len & 3);
      PUSH_DATA (push, data);
   }
}

static enum pipe_reset_status
nvc0_get_device_reset_status(struct pipe_context *pipe)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);

   /* The kernel killed our channel (GSP-RM robust-channel recovery or
    * pushbuf timeout): every submission silently fails, so report the
    * loss.  The state tracker turns this into a GL_KHR_robustness
    * reset, Chromium/EGL clients get GL_GUILTY_CONTEXT_RESET and can
    * recreate their context on a fresh channel. */
   if (nvc0->base.context_lost)
      return PIPE_GUILTY_CONTEXT_RESET;

   return PIPE_NO_RESET;
}

static void
nvc0_context_unreference_resources(struct nvc0_context *nvc0)
{
   unsigned s, i;

   nouveau_bufctx_del(&nvc0->bufctx_3d);
   nouveau_bufctx_del(&nvc0->bufctx);
   nouveau_bufctx_del(&nvc0->bufctx_cp);

   nvc0_framebuffer_init(&nvc0->base.pipe, NULL, nvc0->fb_cbufs, &nvc0->fb_zsbuf);
   util_unreference_framebuffer_state(&nvc0->framebuffer);

   for (i = 0; i < nvc0->num_vtxbufs; ++i)
      pipe_vertex_buffer_unreference(&nvc0->vtxbuf[i]);

   for (s = 0; s < 6; ++s) {
      for (i = 0; i < nvc0->num_textures[s]; ++i)
         pipe_sampler_view_reference(&nvc0->textures[s][i], NULL);

      for (i = 0; i < NVC0_MAX_PIPE_CONSTBUFS; ++i)
         if (!nvc0->constbuf[s][i].user)
            pipe_resource_reference(&nvc0->constbuf[s][i].u.buf, NULL);

      for (i = 0; i < NVC0_MAX_BUFFERS; ++i)
         pipe_resource_reference(&nvc0->buffers[s][i].buffer, NULL);

      for (i = 0; i < NVC0_MAX_IMAGES; ++i) {
         pipe_resource_reference(&nvc0->images[s][i].resource, NULL);
         if (nvc0->screen->base.class_3d >= GM107_3D_CLASS)
            pipe_sampler_view_reference(&nvc0->images_tic[s][i], NULL);
      }
   }

   for (i = 0; i < nvc0->num_tfbbufs; ++i)
      pipe_so_target_reference(&nvc0->tfbbuf[i], NULL);

   for (i = 0; i < nvc0->global_residents.size / sizeof(struct pipe_resource *);
        ++i) {
      struct pipe_resource **res = util_dynarray_element(
         &nvc0->global_residents, struct pipe_resource *, i);
      pipe_resource_reference(res, NULL);
   }
   util_dynarray_fini(&nvc0->global_residents);

   if (nvc0->tcp_empty)
      nvc0->base.pipe.delete_tcs_state(&nvc0->base.pipe, nvc0->tcp_empty);
}

static void
nvc0_destroy(struct pipe_context *pipe)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);

   simple_mtx_lock(&nvc0->screen->state_lock);
   if (nvc0->screen->cur_ctx == nvc0) {
      nvc0->screen->cur_ctx = NULL;
      nvc0->screen->save_state = nvc0->state;
      nvc0->screen->save_state.tfb = NULL;
   }
   /* our pending work is kicked right below */
   if (nvc0->screen->deferred_ctx == nvc0)
      nvc0->screen->deferred_ctx = NULL;
   nvc0->screen->num_contexts--;
   simple_mtx_unlock(&nvc0->screen->state_lock);

   if (nvc0->base.pipe.stream_uploader)
      u_upload_destroy(nvc0->base.pipe.stream_uploader);

   /* Unset bufctx, we don't want to revalidate any resources after the flush.
    * Other contexts will always set their bufctx again on action calls.
    */
   nouveau_pushbuf_bufctx(nvc0->base.pushbuf, NULL);
   PUSH_KICK(nvc0->base.pushbuf);

   nvc0_context_unreference_resources(nvc0);
   nvc0_blitctx_destroy(nvc0);

   list_for_each_entry_safe(struct nvc0_resident, pos, &nvc0->tex_head, list) {
      list_del(&pos->list);
      free(pos);
   }

   list_for_each_entry_safe(struct nvc0_resident, pos, &nvc0->img_head, list) {
      list_del(&pos->list);
      free(pos);
   }

   nouveau_fence_cleanup(&nvc0->base);
   nouveau_context_destroy(&nvc0->base);
}

bool
nvc0_default_kick_notify(struct nouveau_context *context)
{
   struct nvc0_context *nvc0 = nvc0_context(&context->pipe);

   if (!_nouveau_fence_next(context)) {
      /* The fence can only fail when the channel was killed (RC
       * recovery / pushbuf timeout).  Everything after this point
       * would silently go nowhere; flag the loss so
       * nvc0_get_device_reset_status() reports it to the state
       * tracker and the client can recover. */
      context->context_lost = true;
      return false;
   }
   _nouveau_fence_update(context->screen, true);

   nvc0->state.flushed = true;
   return true;
}

static int
nvc0_invalidate_resource_storage(struct nouveau_context *ctx,
                                 struct pipe_resource *res,
                                 int ref)
{
   struct nvc0_context *nvc0 = nvc0_context(&ctx->pipe);
   unsigned s, i;

   if (res->bind & PIPE_BIND_RENDER_TARGET) {
      for (i = 0; i < nvc0->framebuffer.nr_cbufs; ++i) {
         if (nvc0->framebuffer.cbufs[i].texture == res) {
            nvc0->dirty_3d |= NVC0_NEW_3D_FRAMEBUFFER;
            nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_FB);
            if (!--ref)
               return ref;
         }
      }
   }
   if (res->bind & PIPE_BIND_DEPTH_STENCIL) {
      if (nvc0->framebuffer.zsbuf.texture == res) {
         nvc0->dirty_3d |= NVC0_NEW_3D_FRAMEBUFFER;
         nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_FB);
         if (!--ref)
            return ref;
      }
   }

   if (res->target == PIPE_BUFFER) {
      for (i = 0; i < nvc0->num_vtxbufs; ++i) {
         if (nvc0->vtxbuf[i].buffer.resource == res) {
            nvc0->dirty_3d |= NVC0_NEW_3D_ARRAYS;
            nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_VTX);
            if (!--ref)
               return ref;
         }
      }

      for (s = 0; s < 6; ++s) {
         for (i = 0; i < nvc0->num_textures[s]; ++i) {
            if (nvc0->textures[s][i] &&
                nvc0->textures[s][i]->texture == res) {
               nvc0->textures_dirty[s] |= 1 << i;
               if (unlikely(s == 5)) {
                  nvc0->dirty_cp |= NVC0_NEW_CP_TEXTURES;
                  nouveau_bufctx_reset(nvc0->bufctx_cp, NVC0_BIND_CP_TEX(i));
               } else {
                  nvc0->dirty_3d |= NVC0_NEW_3D_TEXTURES;
                  nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_TEX(s, i));
               }
               if (!--ref)
                  return ref;
            }
         }
      }

      for (s = 0; s < 6; ++s) {
         for (i = 0; i < NVC0_MAX_PIPE_CONSTBUFS; ++i) {
            if (!(nvc0->constbuf_valid[s] & (1 << i)))
               continue;
            if (!nvc0->constbuf[s][i].user &&
                nvc0->constbuf[s][i].u.buf == res) {
               nvc0->constbuf_dirty[s] |= 1 << i;
               if (unlikely(s == 5)) {
                  nvc0->dirty_cp |= NVC0_NEW_CP_CONSTBUF;
                  nouveau_bufctx_reset(nvc0->bufctx_cp, NVC0_BIND_CP_CB(i));
               } else {
                  nvc0->dirty_3d |= NVC0_NEW_3D_CONSTBUF;
                  nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_CB(s, i));
               }
               if (!--ref)
                  return ref;
            }
         }
      }

      for (s = 0; s < 6; ++s) {
         for (i = 0; i < NVC0_MAX_BUFFERS; ++i) {
            if (nvc0->buffers[s][i].buffer == res) {
               nvc0->buffers_dirty[s] |= 1 << i;
               if (unlikely(s == 5)) {
                  nvc0->dirty_cp |= NVC0_NEW_CP_BUFFERS;
                  nouveau_bufctx_reset(nvc0->bufctx_cp, NVC0_BIND_CP_BUF);
               } else {
                  nvc0->dirty_3d |= NVC0_NEW_3D_BUFFERS;
                  nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_BUF);
               }
               if (!--ref)
                  return ref;
            }
         }
      }

      for (s = 0; s < 6; ++s) {
         for (i = 0; i < NVC0_MAX_IMAGES; ++i) {
            if (nvc0->images[s][i].resource == res) {
               nvc0->images_dirty[s] |= 1 << i;
               if (unlikely(s == 5)) {
                  nvc0->dirty_cp |= NVC0_NEW_CP_SURFACES;
                  nouveau_bufctx_reset(nvc0->bufctx_cp, NVC0_BIND_CP_SUF);
               } else {
                  nvc0->dirty_3d |= NVC0_NEW_3D_SURFACES;
                  nouveau_bufctx_reset(nvc0->bufctx_3d, NVC0_BIND_3D_SUF);
               }
            }
            if (!--ref)
               return ref;
         }
      }
   }

   return ref;
}

static void
nvc0_context_get_sample_position(struct pipe_context *, unsigned, unsigned,
                                 float *);

static void
nvc0_replace_buffer_storage(struct pipe_context *ctx, struct pipe_resource *dst,
                            struct pipe_resource *src, unsigned num_rebinds,
                            uint64_t rebind_mask, uint32_t delete_buffer_id)
{
   struct nvc0_context *nvc0 = nvc0_context(ctx);
   struct nv04_resource *sdst = nv04_resource(dst);
   struct nv04_resource *ssrc = nv04_resource(src);

   /* Release dst's old CPU data if allocated, then take ownership of src's data */
   if (sdst->data && !(sdst->status & NOUVEAU_BUFFER_STATUS_USER_MEMORY))
      align_free(sdst->data);
   sdst->data = ssrc->data;
   ssrc->data = NULL;

   nouveau_bo_ref(ssrc->bo, &sdst->bo);
   sdst->address = ssrc->address;
   sdst->offset = ssrc->offset;
   sdst->domain = ssrc->domain;
   sdst->status = ssrc->status;
   nouveau_fence_ref(ssrc->fence, &sdst->fence, &nvc0->screen->base);
   nouveau_fence_ref(ssrc->fence_wr, &sdst->fence_wr, &nvc0->screen->base);

   nvc0_invalidate_resource_storage(&nvc0->base, dst, 1);
}

struct pipe_context *
nvc0_create(struct pipe_screen *pscreen, void *priv, unsigned ctxflags)
{
   struct nvc0_screen *screen = nvc0_screen(pscreen);
   struct nvc0_context *nvc0;
   struct pipe_context *pipe;
   int ret;
   uint32_t flags;

   nvc0 = CALLOC_STRUCT(nvc0_context);
   if (!nvc0)
      return NULL;
   pipe = &nvc0->base.pipe;

   if (!nvc0_blitctx_create(nvc0))
      goto out_err;

   if (nouveau_context_init(&nvc0->base, &screen->base))
      goto out_err;
   nvc0->base.kick_notify = nvc0_default_kick_notify;
   nvc0->base.pushbuf->rsvd_kick = 5;

   ret = nouveau_bufctx_new(nvc0->base.client, 2, &nvc0->bufctx);
   if (!ret)
      ret = nouveau_bufctx_new(nvc0->base.client, NVC0_BIND_3D_COUNT,
                               &nvc0->bufctx_3d);
   if (!ret)
      ret = nouveau_bufctx_new(nvc0->base.client, NVC0_BIND_CP_COUNT,
                               &nvc0->bufctx_cp);
   if (ret)
      goto out_err;

   nvc0->screen = screen;
   pipe->screen = pscreen;
   pipe->priv = priv;
   pipe->stream_uploader = u_upload_create_default(pipe);
   if (!pipe->stream_uploader)
      goto out_err;
   pipe->const_uploader = pipe->stream_uploader;

   pipe->destroy = nvc0_destroy;

   pipe->draw_vbo = nvc0_draw_vbo;
   pipe->draw_vbo_buffers = util_draw_vbo_buffers;
   pipe->clear = nvc0_clear;
   pipe->launch_grid = (nvc0->screen->base.class_3d >= NVE4_3D_CLASS) ?
      nve4_launch_grid : nvc0_launch_grid;

   pipe->svm_migrate = nvc0_svm_migrate;

   pipe->flush = nvc0_flush;
   pipe->texture_barrier = nvc0_texture_barrier;
   pipe->memory_barrier = nvc0_memory_barrier;
   pipe->get_sample_position = nvc0_context_get_sample_position;
   pipe->emit_string_marker = nvc0_emit_string_marker;
   pipe->get_device_reset_status = nvc0_get_device_reset_status;

   nvc0_init_query_functions(nvc0);
   nvc0_init_surface_functions(nvc0);
   nvc0_init_state_functions(nvc0);
   nvc0_init_transfer_functions(nvc0);
   nvc0_init_resource_functions(pipe);
   if (nvc0->screen->base.class_3d >= NVE4_3D_CLASS)
      nvc0_init_bindless_functions(pipe);

   list_inithead(&nvc0->tex_head);
   list_inithead(&nvc0->img_head);

   nvc0->base.invalidate_resource_storage = nvc0_invalidate_resource_storage;

   pipe->create_video_codec = nvc0_create_decoder;
   pipe->create_video_buffer = nvc0_video_buffer_create;

   /* shader builtin library is per-screen, but we need a context for m2mf */
   nvc0_program_library_upload(nvc0);
   nvc0_program_init_tcp_empty(nvc0);
   if (!nvc0->tcp_empty)
      goto out_err;

   if (!nouveau_fence_new(&nvc0->base, &nvc0->base.fence))
      goto out_err;

   /* set the empty tctl prog on next draw in case one is never set */
   nvc0->dirty_3d |= NVC0_NEW_3D_TCTLPROG;

   /* Do not bind the COMPUTE driver constbuf at screen initialization because
    * CBs are aliased between 3D and COMPUTE, but make sure it will be bound if
    * a grid is launched later. */
   nvc0->dirty_cp |= NVC0_NEW_CP_DRIVERCONST;

   /* now that there are no more opportunities for errors, set the current
    * context if there isn't already one.
    */
   simple_mtx_lock(&screen->state_lock);
   if (!screen->cur_ctx) {
      nvc0->state = screen->save_state;
      screen->cur_ctx = nvc0;
   }
   /* Flush any pending work from the previous context when a new context joins. */
   if (++screen->num_contexts > 1)
      nvc0_kick_deferred_locked(screen);
   simple_mtx_unlock(&screen->state_lock);

   nouveau_pushbuf_bufctx(nvc0->base.pushbuf, nvc0->bufctx);
   PUSH_SPACE(nvc0->base.pushbuf, 8);

   /* add permanently resident buffers to bufctxts */

   flags = NV_VRAM_DOMAIN(&screen->base) | NOUVEAU_BO_RD;

   BCTX_REFN_bo(nvc0->bufctx_3d, 3D_SCREEN, flags, screen->uniform_bo);
   BCTX_REFN_bo(nvc0->bufctx_3d, 3D_SCREEN, flags, screen->txc);
   if (screen->compute) {
      BCTX_REFN_bo(nvc0->bufctx_cp, CP_SCREEN, flags, screen->uniform_bo);
      BCTX_REFN_bo(nvc0->bufctx_cp, CP_SCREEN, flags, screen->txc);
   }

   flags = NV_VRAM_DOMAIN(&screen->base) | NOUVEAU_BO_RDWR;

   if (screen->poly_cache)
      BCTX_REFN_bo(nvc0->bufctx_3d, 3D_SCREEN, flags, screen->poly_cache);
   if (screen->compute)
      BCTX_REFN_bo(nvc0->bufctx_cp, CP_SCREEN, flags, screen->tls);

   flags = NOUVEAU_BO_GART | NOUVEAU_BO_WR;

   BCTX_REFN_bo(nvc0->bufctx_3d, 3D_SCREEN, flags, screen->fence.bo);
   BCTX_REFN_bo(nvc0->bufctx, FENCE, flags, screen->fence.bo);
   if (screen->compute)
      BCTX_REFN_bo(nvc0->bufctx_cp, CP_SCREEN, flags, screen->fence.bo);

   nvc0->base.scratch.bo_size = 2 << 20;

   memset(nvc0->tex_handles, ~0, sizeof(nvc0->tex_handles));

   nvc0->global_residents = UTIL_DYNARRAY_INIT;

   // Make sure that the first TSC entry has SRGB conversion bit set, since we
   // use it as a fallback on Fermi for TXF, and on Kepler+ generations for
   // FBFETCH handling (which also uses TXF).
   //
   // NOTE: Preliminary testing suggests that this isn't necessary at all at
   // least on GM20x (untested on Kepler). However this is ~free, so no reason
   // not to do it.
   if (!screen->tsc.entries[0])
      nvc0_upload_tsc0(nvc0);

   // On Fermi, mark samplers dirty so that the proper binding can happen
   if (screen->base.class_3d < NVE4_3D_CLASS) {
      for (int s = 0; s < 6; s++)
         nvc0->samplers_dirty[s] = 1;
      nvc0->dirty_3d |= NVC0_NEW_3D_SAMPLERS;
      nvc0->dirty_cp |= NVC0_NEW_CP_SAMPLERS;
   }

   const bool use_tc = debug_get_bool_option("NVGD_THREADED_CONTEXT", true);
   if (!use_tc)
      return pipe;

   if (!(ctxflags & PIPE_CONTEXT_PREFER_THREADED))
      return pipe;

   if (ctxflags & PIPE_CONTEXT_COMPUTE_ONLY)
      return pipe;

   struct pipe_context *tc =
      threaded_context_create(pipe,
                              &screen->transfer_pool,
                              nvc0_replace_buffer_storage,
                              &(struct threaded_context_options){
                                 .unsynchronized_get_device_reset_status = true,
                              },
                              &nvc0->tc);

   if (tc && tc != pipe)
      threaded_context_init_bytes_mapped_limit((struct threaded_context *)tc, 4);

   return tc;

out_err:
   if (nvc0) {
      if (pipe->stream_uploader)
         u_upload_destroy(pipe->stream_uploader);
      if (nvc0->bufctx_3d)
         nouveau_bufctx_del(&nvc0->bufctx_3d);
      if (nvc0->bufctx_cp)
         nouveau_bufctx_del(&nvc0->bufctx_cp);
      if (nvc0->bufctx)
         nouveau_bufctx_del(&nvc0->bufctx);
      FREE(nvc0->blit);
      FREE(nvc0);
   }
   return NULL;
}

void
nvc0_bufctx_fence(struct nvc0_context *nvc0, struct nouveau_bufctx *bufctx,
                  bool on_flush)
{
   struct list_head *list = on_flush ? &bufctx->current : &bufctx->pending;
   struct list_head *it;
   NOUVEAU_DRV_STAT_IFD(unsigned count = 0);

   for (it = list->next; it != list; it = it->next) {
      struct nouveau_bufref *ref = (struct nouveau_bufref *)it;
      struct nv04_resource *res = ref->priv;
      if (res)
         nvc0_resource_validate(nvc0, res, (unsigned)ref->priv_data);
      NOUVEAU_DRV_STAT_IFD(count++);
   }
   NOUVEAU_DRV_STAT(&nvc0->screen->base, resource_validate_count, count);
}

const void *
nvc0_get_sample_locations(unsigned sample_count)
{
   static const uint8_t ms1[1][2] = { { 0x8, 0x8 } };
   static const uint8_t ms2[2][2] = {
      { 0x4, 0x4 }, { 0xc, 0xc } }; /* surface coords (0,0), (1,0) */
   static const uint8_t ms4[4][2] = {
      { 0x6, 0x2 }, { 0xe, 0x6 },   /* (0,0), (1,0) */
      { 0x2, 0xa }, { 0xa, 0xe } }; /* (0,1), (1,1) */
   static const uint8_t ms8[8][2] = {
      { 0x1, 0x7 }, { 0x5, 0x3 },   /* (0,0), (1,0) */
      { 0x3, 0xd }, { 0x7, 0xb },   /* (0,1), (1,1) */
      { 0x9, 0x5 }, { 0xf, 0x1 },   /* (2,0), (3,0) */
      { 0xb, 0xf }, { 0xd, 0x9 } }; /* (2,1), (3,1) */

   const uint8_t (*ptr)[2];

   switch (sample_count) {
   case 0:
   case 1: ptr = ms1; break;
   case 2: ptr = ms2; break;
   case 4: ptr = ms4; break;
   case 8: ptr = ms8; break;
   default:
      assert(0);
      return NULL; /* bad sample count -> undefined locations */
   }
   return ptr;
}

static void
nvc0_context_get_sample_position(struct pipe_context *pipe,
                                 unsigned sample_count, unsigned sample_index,
                                 float *xy)
{
   const uint8_t (*ptr)[2];

   ptr = nvc0_get_sample_locations(sample_count);
   if (!ptr)
      return;

   xy[0] = ptr[sample_index][0] * 0.0625f;
   xy[1] = ptr[sample_index][1] * 0.0625f;
}

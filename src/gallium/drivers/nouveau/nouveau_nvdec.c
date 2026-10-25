/* SPDX-License-Identifier: MIT
 * Native Gallium video submission. Like radeonsi's multimedia backend, this
 * owns a video channel and submits firmware commands through the DRM winsys;
 * VA-API does not create a Vulkan instance/device or call another graphics API.
 */
#define MESA_LOG_TAG "nouveau-nvdec"

#include "nouveau_nvdec.h"
#include "nouveau_nvdec_h264.h"
#include "nouveau_nvdec_av1.h"
#include "nouveau_nvdec_hevc.h"
#include "nouveau_nvdec_vp9.h"
#include "nouveau_screen.h"
#include "nvc0/nvc0_resource.h"
#include "nvdec_drv.h"
#include "nvif/class.h"
#include "clc5b0.h"
#include "clc7b0.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "vl/vl_video_buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

#define NVDEC_PICTURES 17
#define NVDEC_SUBC 2
#define NVDEC_TIMEOUT_NS 5000000000ll
#define NVDEC_ALIGN256_LOCAL(x) align64(x, 256)

enum nvdec_buffer {
   NVDEC_PICTURE,
   NVDEC_OFFSETS,
   NVDEC_STATUS,
   NVDEC_FENCE,
   NVDEC_COLOC,
   NVDEC_MBHIST,
   NVDEC_HISTORY,
   NVDEC_BITSTREAM,
   NVDEC_AV1_CDF_DEFAULT,
   NVDEC_AV1_CDF_OUT,
   NVDEC_AV1_TILES,
   NVDEC_AV1_FILTER,
   NVDEC_AV1_INTRA,
   NVDEC_AV1_GM,
   NVDEC_AV1_GRAIN,
   NVDEC_AV1_TILE_STREAM,
   NVDEC_AV1_ZERO_SEGMENT,
   NVDEC_HEVC_SCALING,
   NVDEC_HEVC_TILES,
   NVDEC_HEVC_FILTER,
   NVDEC_HEVC_INTRA,
   NVDEC_HEVC_SLICE_INFO,
   NVDEC_HEVC_COLOC,
   NVDEC_VP9_PROBS,
   NVDEC_VP9_COUNTS,
   NVDEC_VP9_SEG0,
   NVDEC_VP9_SEG1,
   NVDEC_VP9_TILES,
   NVDEC_VP9_COLMV0,
   NVDEC_VP9_COLMV1,
   NVDEC_VP9_FILTER,
   NVDEC_VP9_INTRA,
   NVDEC_BUFFERS,
};

struct nvdec_surface {
   struct pipe_resource *planes[2];
   struct nouveau_bo *cdf, *segment, *tmv;
   struct nouveau_nvdec_av1_ref av1;
   unsigned vp9_width, vp9_height;
};

struct nouveau_nvdec {
   struct pipe_video_codec base;
   struct nouveau_screen *screen;
   struct nouveau_client *client;
   struct nouveau_object *channel, *engine;
   struct nouveau_pushbuf *push;
   struct nouveau_bo *buffers[NVDEC_BUFFERS];
   struct nvdec_surface surfaces[NVDEC_PICTURES];
   struct nouveau_nvdec_bitstream stream;
   struct nouveau_vp9_state vp9;
   unsigned vp9_width, vp9_height;  /* the previous decoded frame (7.2.6) */
   bool vp9_have_last, vp9_last_show, vp9_last_intra;
   unsigned vp9_seg_prev, vp9_mv_prev;
   uint32_t sequence;
   unsigned width, height;
   enum pipe_format format;
   int error;
};

static int
open_channel(struct nouveau_screen *screen, struct nouveau_object **channel,
             struct nouveau_object **engine)
{
   struct nve0_fifo args = {.engine = NOUVEAU_FIFO_ENGINE_NVDEC};
   int ret = nouveau_object_new(&screen->device->object, 0,
                                NOUVEAU_FIFO_CHANNEL_CLASS, &args, sizeof(args), channel);
   if (ret)
      return ret;
   const struct nouveau_mclass classes[] = {
      {NVC7B0_VIDEO_DECODER, -1},
      {0, 0},
   };
   ret = nouveau_object_mclass(*channel, classes);
   if (ret >= 0)
      ret = nouveau_object_new(*channel, 0xbeefb0, classes[ret].oclass, NULL, 0, engine);
   if (ret)
      nouveau_object_del(channel);
   return ret;
}

bool
nouveau_nvdec_enabled(struct nouveau_screen *screen)
{
   /* Only the GA107 firmware/layout has been validated so far. This is not a
    * Vulkan capability check: the native backend probes the DRM video channel.
    */
   if (screen->device->chipset != 0x177 ||
       !debug_get_bool_option("NOUVEAU_NVDEC", true))
      return false;

   simple_mtx_lock(&screen->fence.lock);
   if (!screen->nvdec.probed) {
      struct nouveau_object *channel = NULL, *engine = NULL;
      screen->nvdec.supported = open_channel(screen, &channel, &engine) == 0;
      nouveau_object_del(&engine);
      nouveau_object_del(&channel);
      screen->nvdec.probed = true;
   }
   bool supported = screen->nvdec.supported;
   simple_mtx_unlock(&screen->fence.lock);
   return supported;
}

int
nouveau_nvdec_get_param(struct nouveau_screen *screen,
                        enum pipe_video_profile profile,
                        enum pipe_video_entrypoint entrypoint,
                        enum pipe_video_cap param)
{
   bool av1 = profile == PIPE_VIDEO_PROFILE_AV1_MAIN &&
               debug_get_bool_option("NOUVEAU_NVDEC_AV1", true);
   bool hevc = (profile == PIPE_VIDEO_PROFILE_HEVC_MAIN ||
                profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10) &&
               debug_get_bool_option("NOUVEAU_NVDEC_HEVC", true);
   bool vp9 = (profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
               profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2) &&
              debug_get_bool_option("NOUVEAU_NVDEC_VP9", true);
   if (entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM ||
       (profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE &&
        profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE &&
        profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN &&
        profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH && !av1 && !hevc && !vp9) ||
       !nouveau_nvdec_enabled(screen))
      return 0;
   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTED:
   case PIPE_VIDEO_CAP_SUPPORTS_PROGRESSIVE:
      return 1;
   case PIPE_VIDEO_CAP_MIN_WIDTH:
      return av1 ? 128 : 48;
   case PIPE_VIDEO_CAP_MIN_HEIGHT:
      return av1 ? 128 : 16;
   case PIPE_VIDEO_CAP_MAX_WIDTH:
   case PIPE_VIDEO_CAP_MAX_HEIGHT:
      return 4096;
   case PIPE_VIDEO_CAP_MAX_MACROBLOCKS:
      return 65536;
   default:
      return 0;
   }
}

static void
release_surface(struct nvdec_surface *surface)
{
   for (unsigned i = 0; i < 2; i++)
      pipe_resource_reference(&surface->planes[i], NULL);
}

static void
destroy_decoder(struct pipe_video_codec *codec)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   /* end_frame is synchronous, including its failure path. Destroy the channel
    * before dropping storage when a watchdog timeout poisoned the decoder.
    */
   nouveau_pushbuf_del(&dec->push);
   nouveau_object_del(&dec->engine);
   nouveau_object_del(&dec->channel);
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      release_surface(&dec->surfaces[i]);
      nouveau_bo_ref(NULL, &dec->surfaces[i].cdf);
      nouveau_bo_ref(NULL, &dec->surfaces[i].segment);
      nouveau_bo_ref(NULL, &dec->surfaces[i].tmv);
   }
   for (unsigned i = 0; i < NVDEC_BUFFERS; i++)
      nouveau_bo_ref(NULL, &dec->buffers[i]);
   nouveau_client_del(&dec->client);
   free(dec->stream.data);
   free(dec);
}

static int
allocate_bo(struct nouveau_nvdec *dec, struct nouveau_bo **bo, size_t size)
{
   int ret = nouveau_bo_new(dec->screen->device,
                            NOUVEAU_BO_GART | NOUVEAU_BO_COHERENT | NOUVEAU_BO_MAP,
                            4096, align64(size, 4096), NULL, bo);
   if (ret)
      return ret;
   ret = nouveau_bo_map(*bo, 0, dec->client);
   if (!ret)
      memset((*bo)->map, 0, (*bo)->size);
   return ret;
}

static int
allocate_buffer(struct nouveau_nvdec *dec, unsigned index, size_t size)
{
   return allocate_bo(dec, &dec->buffers[index], size);
}

static bool
get_surface(struct nouveau_nvdec *dec, struct pipe_video_buffer *buffer,
            struct pipe_resource *planes[3])
{
   if (!buffer || buffer->buffer_format != dec->format || buffer->interlaced ||
       buffer->width < dec->width || buffer->height < dec->height ||
       buffer->context->screen != dec->base.context->screen)
      return false;
   buffer->get_resources(buffer, planes);
   if (!planes[0] || !planes[1])
      return false;
   for (unsigned i = 0; i < 2; i++) {
      unsigned bpp = dec->format == PIPE_FORMAT_P010 ? 2 : 1;
      struct nv50_miptree *mt = nv50_miptree(planes[i]);
      if (!mt->base.bo || mt->base.base.last_level || mt->base.base.nr_samples > 1 ||
          mt->base.base.width0 < (dec->width >> i) ||
          mt->base.base.height0 < (dec->height >> i) ||
          mt->base.base.format != (bpp == 1 ? (i ? PIPE_FORMAT_R8G8_UNORM : PIPE_FORMAT_R8_UNORM) :
                                              (i ? PIPE_FORMAT_R16G16_UNORM : PIPE_FORMAT_R16_UNORM)) ||
          mt->base.base.array_size != 1 || mt->level[0].tile_mode != 0x10 ||
          (mt->base.address + mt->level[0].offset) & 255 ||
          mt->level[0].pitch < dec->width * bpp ||
          mt->base.bo->config.nvc0.memtype != 0x06)
         return false;
   }
   return true;
}

static bool
same_surface(const struct nvdec_surface *surface, struct pipe_resource *const planes[3])
{
   return surface->planes[0] == planes[0] && surface->planes[1] == planes[1];
}

static bool
prepare_surfaces(struct nouveau_nvdec *dec, struct pipe_video_buffer *target,
                 struct pipe_video_buffer *const *pictures, unsigned picture_count,
                 unsigned slot_limit, unsigned *current, uint8_t references[16])
{
   bool keep[NVDEC_PICTURES] = {false};
   struct pipe_resource *planes[3] = {NULL};
   memset(references, 0xff, 16);
   for (unsigned i = 0; i < picture_count; i++) {
      if (!pictures[i])
         continue;
      if (!get_surface(dec, pictures[i], planes))
         return false;
      for (unsigned j = 0; j < NVDEC_PICTURES; j++) {
         if (!same_surface(&dec->surfaces[j], planes))
            continue;
         references[i] = j;
         keep[j] = true;
         break;
      }
      /* A reference must have been decoded in this session. In particular,
       * never guess its colocated-MV slot from the application's DPB order.
       */
      if (references[i] == 0xff)
         return false;
   }
   for (unsigned i = 0; i < NVDEC_PICTURES; i++)
      if (!keep[i])
         release_surface(&dec->surfaces[i]);
   if (!get_surface(dec, target, planes))
      return false;
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      if (keep[i] && same_surface(&dec->surfaces[i], planes))
         return false;
   }
   for (unsigned i = 0; i < slot_limit; i++) {
      if (keep[i])
         continue;
      for (unsigned p = 0; p < 2; p++)
         pipe_resource_reference(&dec->surfaces[i].planes[p], planes[p]);
      *current = i;
      return true;
   }
   return false;
}

static void
method(struct nouveau_pushbuf *push, unsigned address, uint32_t value)
{
   *push->cur++ = 0x20010000 | (NVDEC_SUBC << 13) | (address >> 2);
   *push->cur++ = value;
}

static uint32_t
buffer_address(struct nouveau_nvdec *dec, unsigned index)
{
   return dec->buffers[index]->offset >> 8;
}

enum nvdec_kind {
   NVDEC_KIND_H264,
   NVDEC_KIND_AV1,
   NVDEC_KIND_HEVC,
   NVDEC_KIND_VP9,
};

struct nvdec_vp9_submit {
   const struct nouveau_vp9_frame *frame;
   unsigned engine_refs[3];
};

static int
submit_frame(struct nouveau_nvdec *dec, unsigned current, enum nvdec_kind kind,
             const struct nouveau_nvdec_av1_frame *av1,
             const struct nouveau_nvdec_hevc_frame *hevc,
             const struct nvdec_vp9_submit *vp9,
             const uint8_t references[16])
{
   struct nouveau_pushbuf_refn refs[NVDEC_BUFFERS + NVDEC_PICTURES * 5 + 2];
   unsigned count = 0;
   for (unsigned i = 0; i < NVDEC_BUFFERS; i++) {
      if (dec->buffers[i])
         refs[count++] = (struct nouveau_pushbuf_refn) {
            dec->buffers[i], NOUVEAU_BO_GART | NOUVEAU_BO_RDWR,
         };
   }
   unsigned slots = kind == NVDEC_KIND_HEVC ? NVDEC_HEVC_SLOTS : NVDEC_PICTURES;
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      for (unsigned p = 0; p < 2; p++) {
         struct pipe_resource *plane = dec->surfaces[i].planes[p];
         if (plane)
            refs[count++] = (struct nouveau_pushbuf_refn) {
               nv04_resource(plane)->bo,
               nv04_resource(plane)->domain | (i == current ? NOUVEAU_BO_WR : NOUVEAU_BO_RD),
            };
      }
      if (kind == NVDEC_KIND_AV1 && dec->surfaces[i].planes[0]) {
         struct nouveau_bo *bos[] = {dec->surfaces[i].cdf, dec->surfaces[i].segment,
                                     dec->surfaces[i].tmv};
         for (unsigned b = 0; b < ARRAY_SIZE(bos); b++)
            refs[count++] = (struct nouveau_pushbuf_refn) {bos[b],
                                                           NOUVEAU_BO_GART | NOUVEAU_BO_RDWR};
      }
   }
   if (av1 && av1->pic.enable_fgs) {
      for (unsigned i = 0; i < 2; i++) {
         struct nv04_resource *plane = nv04_resource(av1->display[i]);
         refs[count++] = (struct nouveau_pushbuf_refn) {plane->bo, plane->domain | NOUVEAU_BO_WR};
      }
   }
   int ret = nouveau_pushbuf_space(dec->push, 256, 0, 0);
   if (!ret)
      ret = nouveau_pushbuf_refn(dec->push, refs, count);
   if (ret)
      return ret;

   struct nouveau_pushbuf *push = dec->push;
   const uint32_t seq = ++dec->sequence ? dec->sequence : ++dec->sequence;
   *(uint32_t *)dec->buffers[NVDEC_FENCE]->map = 0;
   method(push, 0, kind == NVDEC_KIND_H264 ? NVC5B0_VIDEO_DECODER
                                           : NVC7B0_VIDEO_DECODER);
   method(push, NVC5B0_SET_APPLICATION_ID,
          kind == NVDEC_KIND_AV1 ? NVC7B0_SET_APPLICATION_ID_ID_AVD :
          kind == NVDEC_KIND_HEVC ? NVC7B0_SET_APPLICATION_ID_ID_HEVC :
          kind == NVDEC_KIND_VP9 ? NVC7B0_SET_APPLICATION_ID_ID_VP9 :
                                    NVC5B0_SET_APPLICATION_ID_ID_H264);
   method(push, NVC5B0_SET_CONTROL_PARAMS,
          (kind == NVDEC_KIND_AV1 ? NVDEC_CODEC_AV1 :
           kind == NVDEC_KIND_HEVC ? NVDEC_CODEC_HEVC :
           kind == NVDEC_KIND_VP9 ? NVDEC_CODEC_VP9 : 3) |
          (kind == NVDEC_KIND_H264 ? (1 << 4) | (1 << 6) | (1 << 13) : (1 << 4)));
   method(push, NVC5B0_SET_DRV_PIC_SETUP_OFFSET, buffer_address(dec, NVDEC_PICTURE));
   method(push, NVC5B0_SET_IN_BUF_BASE_OFFSET, buffer_address(dec, NVDEC_BITSTREAM));
   method(push, NVC5B0_SET_PICTURE_INDEX,
          kind == NVDEC_KIND_HEVC ? current : kind == NVDEC_KIND_VP9 ? 3 : seq);
   if (kind == NVDEC_KIND_H264) {
      method(push, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, buffer_address(dec, NVDEC_OFFSETS));
      method(push, NVC5B0_SET_COLOC_DATA_OFFSET, buffer_address(dec, NVDEC_COLOC));
      method(push, NVC5B0_SET_HISTORY_OFFSET, buffer_address(dec, NVDEC_HISTORY));
   }
   if (kind == NVDEC_KIND_VP9) {
      method(push, NVC7B0_VP9_SET_PROB_TAB_BUF_OFFSET, buffer_address(dec, NVDEC_VP9_PROBS));
      method(push, NVC7B0_VP9_SET_CTX_COUNTER_BUF_OFFSET, buffer_address(dec, NVDEC_VP9_COUNTS));
      method(push, NVC7B0_VP9_SET_SEGMENT_READ_BUF_OFFSET,
             dec->buffers[NVDEC_VP9_SEG0 + dec->vp9_seg_prev]->offset >> 8);
      method(push, NVC7B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET,
             dec->buffers[NVDEC_VP9_SEG0 + (dec->vp9_seg_prev ^ 1)]->offset >> 8);
      method(push, NVC7B0_VP9_SET_TILE_SIZE_BUF_OFFSET, buffer_address(dec, NVDEC_VP9_TILES));
      method(push, NVC7B0_VP9_SET_COL_MVWRITE_BUF_OFFSET,
             dec->buffers[NVDEC_VP9_COLMV0 + (dec->vp9_mv_prev ^ 1)]->offset >> 8);
      method(push, NVC7B0_VP9_SET_COL_MVREAD_BUF_OFFSET,
             dec->buffers[NVDEC_VP9_COLMV0 + dec->vp9_mv_prev]->offset >> 8);
      method(push, NVC7B0_VP9_SET_FILTER_BUFFER_OFFSET, buffer_address(dec, NVDEC_VP9_FILTER));
      method(push, NVC7B0_SET_INTRA_TOP_BUF_OFFSET, buffer_address(dec, NVDEC_VP9_INTRA));
   }
   if (kind == NVDEC_KIND_HEVC) {
      method(push, NVC5B0_SET_COLOC_DATA_OFFSET, buffer_address(dec, NVDEC_HEVC_COLOC));
      method(push, NVC7B0_SET_INTRA_TOP_BUF_OFFSET, buffer_address(dec, NVDEC_HEVC_INTRA));
      method(push, NVC7B0_HEVC_SET_SCALING_LIST_OFFSET, buffer_address(dec, NVDEC_HEVC_SCALING));
      method(push, NVC7B0_HEVC_SET_TILE_SIZES_OFFSET, buffer_address(dec, NVDEC_HEVC_TILES));
      method(push, NVC7B0_HEVC_SET_FILTER_BUFFER_OFFSET, buffer_address(dec, NVDEC_HEVC_FILTER));
      method(push, NVC7B0_HEVC_SET_SAO_BUFFER_OFFSET,
             (dec->buffers[NVDEC_HEVC_FILTER]->offset +
              ((uint64_t)hevc->pic.HevcSaoBufferOffset << 8)) >> 8);
      method(push, NVC7B0_HEVC_SET_SLICE_INFO_BUFFER_OFFSET,
             buffer_address(dec, NVDEC_HEVC_SLICE_INFO));
   }
   method(push, NVC5B0_SET_NVDEC_STATUS_OFFSET, buffer_address(dec, NVDEC_STATUS));
   if (kind == NVDEC_KIND_VP9)
      slots = 4;
   for (unsigned i = 0; i < slots; i++) {
      unsigned source = i;
      if (kind == NVDEC_KIND_VP9)
         source = i == 3 ? current : vp9->engine_refs[i];
      if (kind == NVDEC_KIND_AV1) {
         if (i > 8 || i == 7)
            continue;
         bool intra = av1->pic.frame_type == 0 || av1->pic.frame_type == 2;
         source = i == 8 || intra ? current : references[av1->reference_map[i]];
      }
      for (unsigned p = 0; p < 2; p++) {
         struct pipe_resource *plane = dec->surfaces[source].planes[p];
         uint32_t address = plane ? (nv04_resource(plane)->address +
                                     nv50_miptree(plane)->level[0].offset) >> 8 : 0;
         method(push, (p ? NVC5B0_SET_PICTURE_CHROMA_OFFSET0 :
                           NVC5B0_SET_PICTURE_LUMA_OFFSET0) + i * 4, address);
      }
   }
   if (kind == NVDEC_KIND_AV1) {
      struct nvdec_surface *dst = &dec->surfaces[current];
      struct nouveau_bo *cdf = av1->primary < 0 ? dec->buffers[NVDEC_AV1_CDF_DEFAULT] :
                                                dec->surfaces[references[av1->primary]].cdf;
      struct nouveau_bo *segment = av1->primary >= 0 && av1->pic.prevsegid_flag &&
                                    dec->surfaces[references[av1->primary]].av1.segmented ?
                                    dec->surfaces[references[av1->primary]].segment :
                                    dec->buffers[NVDEC_AV1_ZERO_SEGMENT];
      method(push, NVC7B0_SET_TILE_SIZE_BUF_OFFSET, buffer_address(dec, NVDEC_AV1_TILES));
      method(push, NVC7B0_SET_FILTER_BUFFER_OFFSET, buffer_address(dec, NVDEC_AV1_FILTER));
      method(push, NVC7B0_SET_INTRA_TOP_BUF_OFFSET, buffer_address(dec, NVDEC_AV1_INTRA));
      method(push, NVC7B0_AV1_SET_PROB_TAB_READ_BUF_OFFSET, cdf->offset >> 8);
      method(push, NVC7B0_AV1_SET_PROB_TAB_WRITE_BUF_OFFSET, buffer_address(dec, NVDEC_AV1_CDF_OUT));
      method(push, NVC7B0_AV1_SET_SEGMENT_READ_BUF_OFFSET, segment->offset >> 8);
      method(push, NVC7B0_AV1_SET_SEGMENT_WRITE_BUF_OFFSET, dst->segment->offset >> 8);
      method(push, NVC7B0_AV1_SET_COL_MVWRITE_BUF_OFFSET, dst->tmv->offset >> 8);
      for (unsigned i = 0; i < 3; i++) {
         struct nvdec_surface *src = av1->projections[i] < 0 ? dst :
                                       &dec->surfaces[references[av1->projections[i]]];
         method(push, NVC7B0_AV1_SET_COL_MV0_READ_BUF_OFFSET + i * 4, src->tmv->offset >> 8);
      }
      method(push, NVC7B0_AV1_SET_GLOBAL_MODEL_BUF_OFFSET, buffer_address(dec, NVDEC_AV1_GM));
      method(push, NVC7B0_AV1_SET_FILM_GRAIN_BUF_OFFSET, buffer_address(dec, NVDEC_AV1_GRAIN));
      method(push, NVC7B0_AV1_SET_TILE_STREAM_INFO_BUF_OFFSET,
             buffer_address(dec, NVDEC_AV1_TILE_STREAM));
      if (av1->pic.enable_fgs) {
         struct nv50_miptree *disp_y = nv50_miptree(av1->display[0]);
         method(push, NVC7B0_SET_DISPLAY_BUF_SIZE, (uint32_t)disp_y->base.bo->size);
         for (unsigned i = 0; i < 2; i++) {
            struct nv50_miptree *mt = nv50_miptree(av1->display[i]);
            uint64_t address = mt->base.address + mt->level[0].offset;
            method(push, i ? NVC7B0_SET_DISPLAY_BUF_CHROMA_OFFSET :
                             NVC7B0_SET_DISPLAY_BUF_LUMA_OFFSET, address >> 8);
         }
      }
   } else if (kind == NVDEC_KIND_H264) {
      method(push, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, buffer_address(dec, NVDEC_MBHIST));
   }
   method(push, NVC5B0_EXECUTE, 0);
   method(push, NVC5B0_SEMAPHORE_A, dec->buffers[NVDEC_FENCE]->offset >> 32);
   method(push, NVC5B0_SEMAPHORE_B, dec->buffers[NVDEC_FENCE]->offset);
   method(push, NVC5B0_SEMAPHORE_C, seq);
   method(push, NVC5B0_SEMAPHORE_D, 0);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
   ret = nouveau_pushbuf_kick(push);
   if (ret)
      return ret;

   /* Bound the wait; a bad bitstream must not spin the desktop forever. The
    * firmware watchdog is enabled as well. No software decoding fallback.
    */
   int64_t deadline = os_time_get_nano() + NVDEC_TIMEOUT_NS;
   uint32_t *fence = dec->buffers[NVDEC_FENCE]->map;
   while (__atomic_load_n(fence, __ATOMIC_ACQUIRE) != seq) {
      if (os_time_get_nano() >= deadline)
         return -ETIMEDOUT;
      os_time_sleep(50);
   }
   const nvdec_status_s *status = dec->buffers[NVDEC_STATUS]->map;
   if (status->error_status ||
       (kind == NVDEC_KIND_H264 &&
        (status->mbs_in_error || status->slice_header_error_code))) {
      mesa_loge("decode failed: status=%#x, bad MBs=%u, slice=%#x",
                status->error_status, status->mbs_in_error, status->slice_header_error_code);
      return -EIO;
   }
   return 0;
}

static void
begin_frame(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
            struct pipe_picture_desc *picture)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   dec->stream.size = 0;
   dec->stream.slices = 0;
   if (!target || !picture || picture->protected_playback)
      dec->error = -EINVAL;
}

static void
decode_bitstream(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
                 struct pipe_picture_desc *picture, unsigned count,
                 const void *const *data, const unsigned *sizes)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   if (!dec->error && !nouveau_nvdec_bitstream_append(&dec->stream, count, data, sizes))
      dec->error = -EINVAL;
}

static int
finish_pipe(struct pipe_video_codec *codec)
{
   struct pipe_fence_handle *fence = NULL;
   codec->context->flush(codec->context, &fence, 0);
   bool ready = fence && codec->context->screen->fence_finish(codec->context->screen,
                                                            codec->context, fence, NVDEC_TIMEOUT_NS);
   codec->context->screen->fence_reference(codec->context->screen, &fence, NULL);
   return ready ? 0 : -ETIMEDOUT;
}

static int
end_frame_av1(struct nouveau_nvdec *dec, struct pipe_video_buffer *target,
              const struct pipe_av1_picture_desc *picture)
{
   if (!picture || !target ||
       (target->buffer_format != PIPE_FORMAT_NV12 && target->buffer_format != PIPE_FORMAT_P010) ||
       picture->picture_parameter.bit_depth_idx != (target->buffer_format == PIPE_FORMAT_P010))
      return dec->error = -EINVAL;
   dec->format = target->buffer_format;
   unsigned current;
   uint8_t references[16];
   if (!prepare_surfaces(dec, target, picture->ref, 8, NVDEC_PICTURES,
                         &current, references)) {
      return dec->error = -EINVAL;
   }
   struct nvdec_surface *surface = &dec->surfaces[current];
   struct nouveau_nvdec_av1_frame frame;
   const struct nouveau_nvdec_av1_ref *refmeta[8] = {NULL};
   for (unsigned i = 0; i < 8; i++)
      if (references[i] != 0xff)
         refmeta[i] = &dec->surfaces[references[i]].av1;
   struct nv50_miptree *y = nv50_miptree(surface->planes[0]);
   struct nv50_miptree *uv = nv50_miptree(surface->planes[1]);
   if (!nouveau_nvdec_av1_picture(picture, dec->stream.size,
                                  y->level[0].pitch, uv->level[0].pitch,
                                  dec->width, dec->height, refmeta, &frame))
      return dec->error = -EINVAL;
   if (frame.pic.enable_fgs) {
      struct pipe_resource *planes[3] = {NULL};
      if (!picture->film_grain_target || !get_surface(dec, picture->film_grain_target, planes)) {
         frame.pic.enable_fgs = 0;
      } else {
         bool conflict = false;
         for (unsigned i = 0; i < 2; i++) {
            struct nv50_miptree *mt = nv50_miptree(planes[i]);
            if (mt->level[0].pitch != (i ? uv : y)->level[0].pitch) {
               conflict = true;
               break;
            }
            /* Applying grain must never modify a live reference (including
             * the current reconstructed picture). Reject aliasing imports.
             */
            for (unsigned slot = 0; slot < NVDEC_PICTURES; slot++) {
               for (unsigned p = 0; p < 2; p++) {
                  struct pipe_resource *ref = dec->surfaces[slot].planes[p];
                  if (ref && nv04_resource(ref)->bo->handle == mt->base.bo->handle) {
                     conflict = true;
                     break;
                  }
               }
               if (conflict)
                  break;
            }
            if (conflict)
               break;
            frame.display[i] = planes[i];
         }
         if (conflict)
            frame.pic.enable_fgs = 0;
      }
   }
   int ret = finish_pipe(&dec->base);
   if (!ret && !surface->cdf)
      ret = allocate_bo(dec, &surface->cdf, NVDEC_AV1_CDF_SIZE);
   if (!ret && !surface->segment)
      ret = allocate_bo(dec, &surface->segment, AV1_SEGMENT_ID_BUF_SIZE(dec->width, dec->height));
   if (!ret && !surface->tmv)
      ret = allocate_bo(dec, &surface->tmv, AV1_TEMPORAL_MV_BUF_SIZE(dec->width, dec->height));
   if (ret)
      return dec->error = ret;
   if (!dec->buffers[NVDEC_BITSTREAM] || dec->buffers[NVDEC_BITSTREAM]->size < dec->stream.size + 64) {
      nouveau_bo_ref(NULL, &dec->buffers[NVDEC_BITSTREAM]);
      ret = allocate_buffer(dec, NVDEC_BITSTREAM, dec->stream.size + 64);
      if (ret)
         return dec->error = ret;
   }
   if (frame.primary < 0) {
      unsigned q = picture->picture_parameter.base_qindex;
      nouveau_nvdec_av1_default_cdf(q <= 20 ? 0 : q <= 60 ? 1 : q <= 120 ? 2 : 3,
                                     dec->buffers[NVDEC_AV1_CDF_DEFAULT]->map);
   }
   memcpy(dec->buffers[NVDEC_PICTURE]->map, &frame.pic, sizeof(frame.pic));
   memcpy(dec->buffers[NVDEC_AV1_TILES]->map, frame.tiles, sizeof(frame.tiles));
   memcpy(dec->buffers[NVDEC_AV1_GM]->map, frame.global_motion, sizeof(frame.global_motion));
   if (frame.pic.enable_fgs)
      memcpy(dec->buffers[NVDEC_AV1_GRAIN]->map, &frame.grain, sizeof(frame.grain));
   memcpy(dec->buffers[NVDEC_BITSTREAM]->map, dec->stream.data, dec->stream.size);
   memset((uint8_t *)dec->buffers[NVDEC_BITSTREAM]->map + dec->stream.size, 0, 64);
   memset(dec->buffers[NVDEC_STATUS]->map, 0, dec->buffers[NVDEC_STATUS]->size);
   memset(dec->buffers[NVDEC_AV1_CDF_OUT]->map, 0, NVDEC_AV1_CDF_SIZE);
   ret = submit_frame(dec, current, NVDEC_KIND_AV1, &frame, NULL, NULL, references);
   if (ret) {
      mesa_loge("native AV1 submission failed: %s", strerror(-ret));
      return dec->error = ret;
   }
   struct nouveau_bo *cdf_in = frame.primary < 0 ? dec->buffers[NVDEC_AV1_CDF_DEFAULT] :
                                                   dec->surfaces[references[frame.primary]].cdf;
   memcpy(surface->cdf->map, picture->picture_parameter.pic_info_fields.disable_frame_end_update_cdf ?
                                cdf_in->map : dec->buffers[NVDEC_AV1_CDF_OUT]->map, NVDEC_AV1_CDF_SIZE);
   surface->av1 = (struct nouveau_nvdec_av1_ref) {
      .width = picture->picture_parameter.frame_width,
      .height = picture->picture_parameter.frame_height,
      .coded_width = frame.pic.current_frame.width,
      .pitch = {y->level[0].pitch, uv->level[0].pitch},
      .order_hint = picture->picture_parameter.order_hint,
      .intra = frame.pic.frame_type == 0 || frame.pic.frame_type == 2,
      .segmented = frame.pic.segment_enabled,
   };
   for (unsigned i = 0; i < 7; i++) {
      const struct nouveau_nvdec_av1_ref *r = refmeta[frame.reference_map[i]];
      surface->av1.saved_order_hints[i] = r ? r->order_hint : 0;
   }
   return 0;
}

/* Annex B scan for the H.265 access unit: keep the base-layer VCL NALs
 * (slice segments), drop everything else, and rewrite each with a clean
 * start code. The engine parses NALs itself (sw_start_code_e).
 */
static bool
hevc_assemble(struct nouveau_nvdec *dec, size_t *au_size, const uint8_t **first_nal,
              size_t *first_size)
{
   const uint8_t *data = dec->stream.data;
   size_t size = dec->stream.size;
   size_t au = 0;
   *first_nal = NULL;
   *first_size = 0;
   for (size_t i = 0; i + 3 <= size;) {
      if (data[i] || data[i + 1] || data[i + 2] != 1) {
         i++;
         continue;
      }
      size_t payload = i + 3;
      size_t end = payload;
      while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 &&
                                  (data[end + 2] == 1 || (data[end + 2] == 0 &&
                                   end + 4 <= size && data[end + 3] == 1))))
         end++;
      if (end + 3 > size)
         end = size;
      while (end > payload && !data[end - 1])
         end--;
      i = end;
      if (end - payload < 2)
         continue;
      unsigned type = (data[payload] >> 1) & 63;
      unsigned layer = ((data[payload] & 1) << 5) | (data[payload + 1] >> 3);
      if (layer || (type > 9 && (type < 16 || type > 21)))
         continue;
      if (!*first_nal) {
         *first_nal = data + payload;
         *first_size = end - payload;
      }
      au += 3 + end - payload;
   }
   if (!*first_nal)
      return false;
   *au_size = au;
   return true;
}

static int
ensure_bitstream(struct nouveau_nvdec *dec, size_t needed)
{
   if (dec->buffers[NVDEC_BITSTREAM] && dec->buffers[NVDEC_BITSTREAM]->size >= needed)
      return 0;
   nouveau_bo_ref(NULL, &dec->buffers[NVDEC_BITSTREAM]);
   return allocate_buffer(dec, NVDEC_BITSTREAM, needed);
}

static int
end_frame_hevc(struct nouveau_nvdec *dec, struct pipe_video_buffer *target,
               const struct pipe_h265_picture_desc *picture)
{
   unsigned bpp = dec->base.profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10 ? 2 : 1;
   if (!picture || !target ||
       target->buffer_format != (bpp == 2 ? PIPE_FORMAT_P010 : PIPE_FORMAT_NV12))
      return dec->error = -EINVAL;
   dec->format = target->buffer_format;
   unsigned current;
   uint8_t references[16];
   if (!prepare_surfaces(dec, target, picture->ref, 16, NVDEC_HEVC_SLOTS,
                         &current, references))
      return dec->error = -EINVAL;
   size_t au_size, first_size;
   const uint8_t *first_nal;
   if (!hevc_assemble(dec, &au_size, &first_nal, &first_size))
      return dec->error = -EINVAL;
   struct nv50_miptree *y = nv50_miptree(dec->surfaces[current].planes[0]);
   struct nv50_miptree *uv = nv50_miptree(dec->surfaces[current].planes[1]);
   struct nouveau_nvdec_hevc_frame frame;
   if (!nouveau_nvdec_hevc_picture(picture, first_nal, first_size,
                                   y->level[0].pitch, uv->level[0].pitch, bpp,
                                   dec->width, dec->height, current, references,
                                   &frame))
      return dec->error = -EINVAL;
   int ret = finish_pipe(&dec->base);
   if (!ret)
      ret = ensure_bitstream(dec, au_size + 64);
   if (ret)
      return dec->error = ret;

   /* Rewrite the kept NALs with clean start codes. */
   uint8_t *au = dec->buffers[NVDEC_BITSTREAM]->map;
   size_t written = 0;
   for (size_t i = 0; i + 3 <= dec->stream.size && written < au_size;) {
      const uint8_t *data = dec->stream.data;
      if (data[i] || data[i + 1] || data[i + 2] != 1) {
         i++;
         continue;
      }
      size_t payload = i + 3;
      size_t end = payload;
      while (end + 3 <= dec->stream.size &&
             !(data[end] == 0 && data[end + 1] == 0 &&
               (data[end + 2] == 1 || (data[end + 2] == 0 &&
                end + 4 <= dec->stream.size && data[end + 3] == 1))))
         end++;
      if (end + 3 > dec->stream.size)
         end = dec->stream.size;
      while (end > payload && !data[end - 1])
         end--;
      i = end;
      if (end - payload < 2)
         continue;
      unsigned type = (data[payload] >> 1) & 63;
      unsigned layer = ((data[payload] & 1) << 5) | (data[payload + 1] >> 3);
      if (layer || (type > 9 && (type < 16 || type > 21)))
         continue;
      au[written++] = 0;
      au[written++] = 0;
      au[written++] = 1;
      memcpy(au + written, data + payload, end - payload);
      written += end - payload;
   }
   assert(written == au_size);
   memset(au + written, 0, 64);
   frame.pic.stream_len = au_size;

   memcpy(dec->buffers[NVDEC_PICTURE]->map, &frame.pic, sizeof(frame.pic));
   memcpy(dec->buffers[NVDEC_HEVC_TILES]->map, frame.tiles, sizeof(frame.tiles));
   memcpy(dec->buffers[NVDEC_HEVC_SCALING]->map, frame.scaling, sizeof(frame.scaling));
   memset(dec->buffers[NVDEC_STATUS]->map, 0, dec->buffers[NVDEC_STATUS]->size);
   ret = submit_frame(dec, current, NVDEC_KIND_HEVC, NULL, &frame, NULL, references);
   if (ret) {
      mesa_loge("native HEVC submission failed: %s", strerror(-ret));
      return dec->error = ret;
   }
   return 0;
}

/* The engine reads reference samples up to the superblock-aligned frame
 * size without clamping at the frame edge (which the spec does, 8.5.2.4), so
 * the right and bottom padding of the decoded surface must repeat the edge.
 * The Gallium video surfaces are 64-aligned (nouveau_nvdec_buffer_create),
 * so the bands stay inside the resource. The map is read/write so the parts
 * of the staging that are not extended keep their decoded content.
 */
static void
vp9_extend_plane(struct pipe_context *pipe, struct pipe_resource *res,
                 unsigned width, unsigned height,
                 unsigned full_width, unsigned full_height, unsigned bpp)
{
   if (width >= full_width && height >= full_height)
      return;
   struct pipe_transfer *transfer = NULL;
   /* Box units are pixels, not bytes. */
   uint8_t *map = pipe_texture_map(pipe, res, 0, 0, PIPE_MAP_READ | PIPE_MAP_WRITE,
                                   0, 0, full_width, full_height, &transfer);
   if (!map || !transfer)
      return;
   const unsigned stride = transfer->stride;
   if (width < full_width) {
      for (unsigned y = 0; y < MIN2(height, full_height); y++) {
         const uint8_t *edge = map + (size_t)y * stride + (width - 1) * bpp;
         for (unsigned x = width; x < full_width; x++)
            for (unsigned b = 0; b < bpp; b++)
               map[(size_t)y * stride + x * bpp + b] = edge[b];
      }
   }
   if (height < full_height) {
      const uint8_t *last = map + (size_t)(height - 1) * stride;
      for (unsigned y = height; y < full_height; y++)
         memcpy(map + (size_t)y * stride, last, (size_t)full_width * bpp);
   }
   pipe_texture_unmap(pipe, transfer);
}

static int
end_frame_vp9(struct nouveau_nvdec *dec, struct pipe_video_buffer *target,
              const struct pipe_vp9_picture_desc *picture)
{
   unsigned bpp = dec->base.profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2 ? 2 : 1;
   if (!picture || !target ||
       target->buffer_format != (bpp == 2 ? PIPE_FORMAT_P010 : PIPE_FORMAT_NV12)) {
      return dec->error = -EINVAL;
   }
   dec->format = target->buffer_format;

   struct nouveau_vp9_frame frame;
   if (!nouveau_vp9_parse(&dec->vp9, dec->stream.data, dec->stream.size,
                          picture->picture_parameter.frame_width,
                          picture->picture_parameter.frame_height, &frame)) {
      return dec->error = -EINVAL;
   }
   if (frame.show_existing_frame)
      return dec->error = -EINVAL; /* the frontend outputs those itself */
   if (frame.profile != picture->picture_parameter.profile ||
       frame.bit_depth != picture->picture_parameter.bit_depth ||
       frame.subsampling_x != picture->picture_parameter.pic_fields.subsampling_x ||
       frame.subsampling_y != picture->picture_parameter.pic_fields.subsampling_y ||
       frame.seg_enabled != picture->picture_parameter.pic_fields.segmentation_enabled)
      return dec->error = -EINVAL;

   unsigned current;
   uint8_t references[16];
   if (!prepare_surfaces(dec, target, picture->ref, 8, NVDEC_PICTURES,
                         &current, references)) {
      return dec->error = -EINVAL;
   }
   if (!frame.frame_is_intra) {
      for (unsigned i = 0; i < 3; i++)
         if (frame.ref_frame_idx[i] > 7 || references[frame.ref_frame_idx[i]] == 0xff) {
            return dec->error = -EINVAL;
         }
      if (frame.size_from_ref) {
         unsigned slot = references[frame.ref_frame_idx[frame.size_ref_idx]];
         frame.width = dec->surfaces[slot].vp9_width;
         frame.height = dec->surfaces[slot].vp9_height;
      }
   }
   if (!frame.width || !frame.height || frame.width > 8192 || frame.height > 8192 ||
       frame.width > dec->width || frame.height > dec->height ||
       frame.width != picture->picture_parameter.frame_width ||
       frame.height != picture->picture_parameter.frame_height ||
       frame.uncomp_bytes != picture->picture_parameter.frame_header_length_in_bytes ||
       frame.header_size_in_bytes != picture->picture_parameter.first_partition_size ||
       frame.tile_cols_log2 != picture->picture_parameter.log2_tile_columns ||
       frame.tile_rows_log2 != picture->picture_parameter.log2_tile_rows ||
       (frame.profile == 0 && frame.bit_depth != 8) ||
       (frame.profile == 2 && frame.bit_depth != 10))
      return dec->error = -EINVAL;

   bool size_changed = !dec->vp9_have_last || frame.width != dec->vp9_width ||
                       frame.height != dec->vp9_height;
   if (size_changed || frame.frame_is_intra || frame.error_resilient_mode)
      memset(dec->buffers[NVDEC_VP9_SEG0 + dec->vp9_seg_prev]->map, 0,
             dec->buffers[NVDEC_VP9_SEG0 + dec->vp9_seg_prev]->size);

   struct nv50_miptree *y = nv50_miptree(dec->surfaces[current].planes[0]);
   struct nv50_miptree *uv = nv50_miptree(dec->surfaces[current].planes[1]);
   nvdec_vp9_pic_s *pic = dec->buffers[NVDEC_PICTURE]->map;
   memset(pic, 0, sizeof(*pic));
   size_t headers = frame.uncomp_bytes + frame.header_size_in_bytes;
   size_t tiles = dec->stream.size - headers;
   pic->stream_len = tiles;
   pic->tileformat = 1;
   pic->gob_height = 0;
   {
      unsigned top = NVDEC_ALIGN256_LOCAL((align(dec->width, 64) / 64 + 2) * 2048);
      unsigned left = NVDEC_ALIGN256_LOCAL((align(dec->height, 64) / 64 + 2) * 1664);
      unsigned above = NVDEC_ALIGN256_LOCAL((uint64_t)top + left);
      pic->Vp9BsdCtrlOffset = (above + NVDEC_ALIGN256_LOCAL(
         (uint64_t)dec->height * GIP_ASIC_VERT_FILTER_RAM_SIZE * 64)) >> 8;
      pic->v1.Vp9FltAboveOffset = above >> 8;
   }
   pic->v1.bit_depth = frame.bit_depth;
   unsigned samples_y = y->level[0].pitch / bpp;
   unsigned samples_uv = uv->level[0].pitch / bpp;
   for (unsigned i = 0; i < 3; i++) {
      bool intra = frame.frame_is_intra;
      unsigned slot = intra ? current : references[frame.ref_frame_idx[i]];
      unsigned short *dims = &pic->ref0_width + i * 4;
      dims[0] = intra ? frame.width : dec->surfaces[slot].vp9_width;
      dims[1] = intra ? frame.height : dec->surfaces[slot].vp9_height;
      dims[2] = samples_y;
      dims[3] = samples_uv;
   }
   pic->width = frame.width;
   pic->height = frame.height;
   pic->framestride[0] = samples_y;
   pic->framestride[1] = samples_uv;
   pic->keyFrame = frame.frame_type == VP9_KEY_FRAME;
   pic->prevIsKeyFrame = dec->vp9_have_last && dec->vp9_last_intra;
   pic->resolutionChange = size_changed;
   pic->errorResilient = frame.error_resilient_mode;
   pic->prevShowFrame = dec->vp9_have_last && dec->vp9_last_show;
   pic->intraOnly = frame.intra_only;
   for (unsigned i = 0; i < 4; i++)
      pic->refFrameSignBias[i] = frame.ref_frame_sign_bias[i];
   pic->loopFilterLevel = frame.lf_level;
   pic->loopFilterSharpness = frame.lf_sharpness;
   pic->qpYAc = frame.base_q_idx;
   pic->qpYDc = frame.delta_q_y_dc;
   pic->qpChAc = frame.delta_q_uv_ac;
   pic->qpChDc = frame.delta_q_uv_dc;
   pic->lossless = frame.lossless;
   pic->transform_mode = frame.tx_mode;
   pic->allow_high_precision_mv = frame.allow_high_precision_mv;
   /* The engine numbers the interpolation filters in the bitstream's literal
    * order (smooth, regular, sharp, bilinear; 4 = switchable), not the spec's.
    */
   {
      static const char literal[5] = { 1, 0, 2, 3, 4 };
      pic->mcomp_filter_type = literal[frame.interp_filter];
   }
   pic->comp_pred_mode = frame.reference_mode;
   pic->comp_fixed_ref = frame.comp_fixed_ref;
   pic->comp_var_ref[0] = frame.comp_var_ref[0];
   pic->comp_var_ref[1] = frame.comp_var_ref[1];
   pic->log2_tile_columns = frame.tile_cols_log2;
   pic->log2_tile_rows = frame.tile_rows_log2;
   pic->segmentEnabled = frame.seg_enabled;
   pic->segmentMapUpdate = frame.seg_update_map;
   pic->segmentMapTemporalUpdate = frame.seg_temporal_update;
   pic->segmentFeatureMode = dec->vp9.seg_abs_or_delta_update;
   for (unsigned i = 0; i < 8; i++)
      for (unsigned j = 0; j < 4; j++) {
         pic->segmentFeatureEnable[i][j] = dec->vp9.seg_feature_enabled[i][j];
         pic->segmentFeatureData[i][j] = dec->vp9.seg_feature_data[i][j];
      }
   pic->modeRefLfEnabled = dec->vp9.lf_delta_enabled;
   for (unsigned i = 0; i < 4; i++)
      pic->mbRefLfDelta[i] = dec->vp9.lf_ref_deltas[i];
   for (unsigned i = 0; i < 2; i++)
      pic->mbModeLfDelta[i] = dec->vp9.lf_mode_deltas[i];

   /* Tile sizes in superblocks, raster order. */
   {
      const unsigned mi_cols = (frame.width + 7) >> 3;
      const unsigned mi_rows = (frame.height + 7) >> 3;
      unsigned n = 0;
      uint16_t *sizes = dec->buffers[NVDEC_VP9_TILES]->map;
      memset(sizes, 0, dec->buffers[NVDEC_VP9_TILES]->size);
      for (unsigned r = 0; r < 1u << frame.tile_rows_log2; r++)
         for (unsigned c = 0; c < 1u << frame.tile_cols_log2; c++) {
            const int sbs = (mi_cols + 7) >> 3;
            int c0 = ((c * sbs) >> frame.tile_cols_log2) << 3;
            int c1 = (((c + 1) * sbs) >> frame.tile_cols_log2) << 3;
            const int rows = (mi_rows + 7) >> 3;
            int r0 = ((r * rows) >> frame.tile_rows_log2) << 3;
            int r1 = (((r + 1) * rows) >> frame.tile_rows_log2) << 3;
            if (c0 > (int)mi_cols) c0 = mi_cols;
            if (c1 > (int)mi_cols) c1 = mi_cols;
            if (r0 > (int)mi_rows) r0 = mi_rows;
            if (r1 > (int)mi_rows) r1 = mi_rows;
            sizes[2 * n] = (uint16_t)((c1 - c0 + 7) >> 3);
            sizes[2 * n + 1] = (uint16_t)((r1 - r0 + 7) >> 3);
            n++;
         }
   }

   int ret = finish_pipe(&dec->base);
   if (!ret)
      ret = ensure_bitstream(dec, tiles + 64);
   if (ret) {
      return dec->error = ret;
   }
   memcpy(dec->buffers[NVDEC_BITSTREAM]->map, dec->stream.data + headers, tiles);
   memset((uint8_t *)dec->buffers[NVDEC_BITSTREAM]->map + tiles, 0, 64);
   nouveau_vp9_pack_probs(dec->buffers[NVDEC_VP9_PROBS]->map, &dec->vp9, &frame);
   memset(dec->buffers[NVDEC_VP9_COUNTS]->map, 0, dec->buffers[NVDEC_VP9_COUNTS]->size);
   memset(dec->buffers[NVDEC_STATUS]->map, 0, dec->buffers[NVDEC_STATUS]->size);

   struct nvdec_vp9_submit submit = { .frame = &frame };
   for (unsigned i = 0; i < 3; i++)
      submit.engine_refs[i] = frame.frame_is_intra ? current
                                                   : references[frame.ref_frame_idx[i]];
   ret = submit_frame(dec, current, NVDEC_KIND_VP9, NULL, NULL, &submit, references);
   if (ret) {
      mesa_loge("native VP9 submission failed: %s", strerror(-ret));
      return dec->error = ret;
   }

   /* The engine reads references up to the superblock-aligned size. */
   vp9_extend_plane(dec->base.context, dec->surfaces[current].planes[0],
                    frame.width, frame.height,
                    y->base.base.width0, align(frame.height, 64), bpp);
   vp9_extend_plane(dec->base.context, dec->surfaces[current].planes[1],
                    (frame.width + 1) / 2, (frame.height + 1) / 2,
                    uv->base.base.width0, align(frame.height, 64) / 2, 2 * bpp);

   nouveau_vp9_end_frame(&dec->vp9, &frame,
                         nouveau_vp9_adapts(&frame) ?
                         dec->buffers[NVDEC_VP9_COUNTS]->map : NULL);
   dec->surfaces[current].vp9_width = frame.width;
   dec->surfaces[current].vp9_height = frame.height;
   if (frame.seg_enabled && frame.seg_update_map)
      dec->vp9_seg_prev ^= 1;
   dec->vp9_mv_prev ^= 1;
   dec->vp9_have_last = true;
   dec->vp9_width = frame.width;
   dec->vp9_height = frame.height;
   dec->vp9_last_show = frame.show_frame;
   dec->vp9_last_intra = frame.frame_is_intra;
   return 0;
}

static int
end_frame(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
          struct pipe_picture_desc *picture)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   if (dec->error)
      return dec->error;
   if (codec->profile == PIPE_VIDEO_PROFILE_AV1_MAIN)
      return end_frame_av1(dec, target, (const struct pipe_av1_picture_desc *)picture);
   if (codec->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN ||
       codec->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10)
      return end_frame_hevc(dec, target, (const struct pipe_h265_picture_desc *)picture);
   if (codec->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
       codec->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2)
      return end_frame_vp9(dec, target, (const struct pipe_vp9_picture_desc *)picture);
   const struct pipe_h264_picture_desc *h264 = (const struct pipe_h264_picture_desc *)picture;
   unsigned current;
   uint8_t references[16];
   nvdec_h264_pic_s params;
   if (!h264 || !prepare_surfaces(dec, target, h264->ref, 16, NVDEC_PICTURES,
                              &current, references))
      return dec->error = -EINVAL;
   struct nv50_miptree *y = nv50_miptree(dec->surfaces[current].planes[0]);
   struct nv50_miptree *uv = nv50_miptree(dec->surfaces[current].planes[1]);
   if (!nouveau_nvdec_h264_picture(h264, &dec->stream, dec->width, dec->height,
                                    y->level[0].pitch, uv->level[0].pitch,
                                    current, references, &params))
      return dec->error = -EINVAL;

   /* Flush pending Gallium uses before submitting on the independent video
    * channel. Waiting here preserves ordering even for in-process resources
    * whose Gallium fence has not yet reached the kernel reservation object.
    */
   if (finish_pipe(codec))
      return dec->error = -ETIMEDOUT;
   if (!dec->buffers[NVDEC_BITSTREAM] ||
       dec->buffers[NVDEC_BITSTREAM]->size < dec->stream.size + 16) {
      nouveau_bo_ref(NULL, &dec->buffers[NVDEC_BITSTREAM]);
      int ret = allocate_buffer(dec, NVDEC_BITSTREAM, dec->stream.size + 16);
      if (ret)
         return dec->error = ret;
   }
   memcpy(dec->buffers[NVDEC_PICTURE]->map, &params, sizeof(params));
   memcpy(dec->buffers[NVDEC_OFFSETS]->map, dec->stream.offsets,
          (dec->stream.slices + 1) * sizeof(uint32_t));
   memcpy(dec->buffers[NVDEC_BITSTREAM]->map, dec->stream.data, dec->stream.size);
   memcpy((uint8_t *)dec->buffers[NVDEC_BITSTREAM]->map + dec->stream.size, params.eos, 16);
   memset(dec->buffers[NVDEC_STATUS]->map, 0, dec->buffers[NVDEC_STATUS]->size);
   int ret = submit_frame(dec, current, NVDEC_KIND_H264, NULL, NULL, NULL, NULL);
   if (ret) {
      mesa_loge("native H.264 submission failed: %s", strerror(-ret));
      dec->error = ret;
   }
   return ret;
}

static void
flush_decoder(struct pipe_video_codec *codec)
{
   /* end_frame has already completed the video submission. */
}

struct pipe_video_codec *
nouveau_nvdec_create(struct pipe_context *context, const struct pipe_video_codec *templ)
{
   struct nouveau_screen *screen = nouveau_screen(context->screen);
   if (!nouveau_nvdec_get_param(screen, templ->profile, templ->entrypoint, PIPE_VIDEO_CAP_SUPPORTED) ||
       templ->max_references > 16 || templ->width < 48 || templ->width > 4096 ||
       templ->height < 16 || templ->height > 4096)
      return NULL;
   struct nouveau_nvdec *dec = calloc(1, sizeof(*dec));
   if (!dec)
      return NULL;
   dec->base = *templ;
   pipe_reference_init(&dec->base.reference, 1);
   dec->base.context = context;
   dec->base.destroy = destroy_decoder;
   dec->base.begin_frame = begin_frame;
   dec->base.decode_bitstream = decode_bitstream;
   dec->base.end_frame = end_frame;
   dec->base.flush = flush_decoder;
   dec->screen = screen;
   dec->width = align(templ->width, 16);
   dec->height = align(templ->height, 16);
   dec->format = PIPE_FORMAT_NV12;
   if (templ->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2)
      dec->format = PIPE_FORMAT_P010;
   if (templ->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
       templ->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2)
      nouveau_vp9_init(&dec->vp9);
   if (templ->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10)
      dec->format = PIPE_FORMAT_P010;
   int ret = nouveau_client_new(screen->device, &dec->client);
   if (!ret)
      ret = open_channel(screen, &dec->channel, &dec->engine);
   if (!ret)
      ret = nouveau_pushbuf_new(dec->client, dec->channel, 2, 16384, &dec->push);
   size_t sizes[NVDEC_BUFFERS] = {
      [NVDEC_PICTURE] = sizeof(nvdec_h264_pic_s),
      [NVDEC_OFFSETS] = (NOUVEAU_NVDEC_MAX_SLICES + 1) * sizeof(uint32_t),
      [NVDEC_STATUS] = 4096,
      [NVDEC_FENCE] = 4096,
      [NVDEC_COLOC] = align(align(dec->height / 16, 2) * (dec->width / 16) * 64 - 63, 256) * NVDEC_PICTURES,
      [NVDEC_MBHIST] = align((dec->width / 16) * 104, 256),
      [NVDEC_HISTORY] = align((dec->width / 16) * 0x300, 512),
   };
   if (templ->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN ||
       templ->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10) {
      sizes[NVDEC_PICTURE] = sizeof(nvdec_hevc_pic_s);
      sizes[NVDEC_OFFSETS] = sizes[NVDEC_COLOC] = 0;
      sizes[NVDEC_MBHIST] = sizes[NVDEC_HISTORY] = 0;
      sizes[NVDEC_HEVC_SCALING] = NVDEC_HEVC_SCALING_SIZE;
      sizes[NVDEC_HEVC_TILES] = sizeof(((struct nouveau_nvdec_hevc_frame *)0)->tiles);
      sizes[NVDEC_HEVC_FILTER] = nouveau_nvdec_hevc_filter_size(dec->width, dec->height);
      sizes[NVDEC_HEVC_INTRA] = (uint64_t)dec->width * 8 *
                                (templ->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10 ? 2 : 1);
      sizes[NVDEC_HEVC_SLICE_INFO] = 0x10000;
      sizes[NVDEC_HEVC_COLOC] = (uint64_t)NVDEC_HEVC_SLOTS *
                                nouveau_nvdec_hevc_coloc_size(dec->width, dec->height);
   }
   if (templ->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
       templ->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2) {
      sizes[NVDEC_PICTURE] = sizeof(nvdec_vp9_pic_s);
      sizes[NVDEC_OFFSETS] = sizes[NVDEC_COLOC] = 0;
      sizes[NVDEC_MBHIST] = sizes[NVDEC_HISTORY] = 0;
      sizes[NVDEC_VP9_PROBS] = sizeof(nvdec_vp9EntropyProbs_t);
      sizes[NVDEC_VP9_COUNTS] = sizeof(nvdec_vp9EntropyCounts_t);
      sizes[NVDEC_VP9_SEG0] = sizes[NVDEC_VP9_SEG1] =
         nouveau_vp9_seg_size(align(dec->width, 64), align(dec->height, 64));
      sizes[NVDEC_VP9_TILES] = 0x1000;
      sizes[NVDEC_VP9_COLMV0] = sizes[NVDEC_VP9_COLMV1] =
         nouveau_vp9_colmv_size(align(dec->width, 64), align(dec->height, 64));
      sizes[NVDEC_VP9_FILTER] = nouveau_vp9_filter_size(dec->width, dec->height);
      sizes[NVDEC_VP9_INTRA] = (uint64_t)dec->width * 8 *
                               (templ->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2 ? 2 : 1);
   }
   if (templ->profile == PIPE_VIDEO_PROFILE_AV1_MAIN) {
      sizes[NVDEC_PICTURE] = sizeof(nvdec_av1_pic_s);
      sizes[NVDEC_OFFSETS] = sizes[NVDEC_COLOC] = sizes[NVDEC_MBHIST] = sizes[NVDEC_HISTORY] = 0;
      sizes[NVDEC_AV1_CDF_DEFAULT] = sizes[NVDEC_AV1_CDF_OUT] = NVDEC_AV1_CDF_SIZE;
      sizes[NVDEC_AV1_TILES] = AV1_TILEINFO_BUF_SIZE_OLD;
      sizes[NVDEC_AV1_FILTER] = nouveau_nvdec_av1_filter_size(dec->width, dec->height);
      sizes[NVDEC_AV1_INTRA] = AV1_INTRA_TOP_BUF_SIZE * 2;
      sizes[NVDEC_AV1_GM] = AV1_GLOBAL_MOTION_BUF_SIZE;
      sizes[NVDEC_AV1_GRAIN] = AV1_FGS_BUF_SIZE;
      sizes[NVDEC_AV1_TILE_STREAM] = AV1_TILESTREAMINFO_BUF_SIZE;
      sizes[NVDEC_AV1_ZERO_SEGMENT] = AV1_SEGMENT_ID_BUF_SIZE(dec->width, dec->height);
   }
   for (unsigned i = 0; !ret && i < ARRAY_SIZE(sizes); i++)
      if (sizes[i])
         ret = allocate_buffer(dec, i, sizes[i]);
   if (ret) {
      mesa_loge("native video initialization failed: %s", strerror(-ret));
      destroy_decoder(&dec->base);
      return NULL;
   }
   mesa_logd("H.264: native NVDEC, %ux%u, direct Gallium surfaces", dec->width, dec->height);
   return &dec->base;
}

struct pipe_video_buffer *
nouveau_nvdec_buffer_create(struct pipe_context *context, const struct pipe_video_buffer *templ)
{
   struct pipe_video_buffer native = *templ;
   native.flags |= NVC0_RESOURCE_FLAG_VIDEO;
   /* Video surfaces must use the uncompressed generic kind on every engine,
    * and VP9 references are read up to the superblock-aligned frame size, so
    * the storage is 64-aligned in both dimensions. */
   native.bind |= PIPE_BIND_SHARED;
   native.width = align(native.width, 64);
   native.height = align(native.height, 64);
   return vl_video_buffer_create(context, &native);
}

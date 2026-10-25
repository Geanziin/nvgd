/*
 * SPDX-License-Identifier: MIT
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 */
#define MESA_LOG_TAG "nouveau-nvenc"

#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nouveau.h"
#include "nouveau_nvenc.h"
#include "nouveau_nvenc_rc.h"
#include "nv50/nv50_resource.h"
#include "nouveau_screen.h"
#include "pipe/p_context.h"
#include "util/list.h"
#include "util/log.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_video.h"
#include "vl/vl_video_buffer.h"

#ifdef HAVE_NOUVEAU_NVENC

#include "nouveau_nvenc_backend.h"

struct nvenc_codec {
   struct pipe_video_codec base;
   struct nvenc_backend *backend;
   unsigned backend_width, backend_height;
   unsigned backend_rate_num, backend_rate_den;
   enum nvenc_profile backend_profile;
   unsigned qp;
   struct nvenc_rc rc;
   enum pipe_h2645_enc_rate_control_method rc_method;
   unsigned rc_target, rc_peak, rc_min_qp, rc_max_qp;
   int error;
   simple_mtx_t lock;
   bool is_p_frame;
   struct list_head pending;
};

struct nvenc_feedback {
   struct list_head head;
   struct nvenc_ticket ticket;
   struct pipe_resource *destination;
   unsigned qp;
   int error;
};

static bool
profile_supported(enum pipe_video_profile profile)
{
   if ((profile == PIPE_VIDEO_PROFILE_HEVC_MAIN || profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10) &&
       debug_get_bool_option("NOUVEAU_NVENC_HEVC", true))
      return true;
   return profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE ||
          profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE ||
          profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN ||
          profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
}

static enum nvenc_profile
backend_profile(enum pipe_video_profile profile)
{
   switch (profile) {
   case PIPE_VIDEO_PROFILE_HEVC_MAIN:
      return NVENC_PROFILE_HEVC_MAIN;
   case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
      return NVENC_PROFILE_HEVC_MAIN10;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
      return NVENC_PROFILE_HIGH;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
      return NVENC_PROFILE_MAIN;
   default:
      return NVENC_PROFILE_BASELINE;
   }
}

static bool
kernel_supported(struct nouveau_screen *screen)
{
   struct stat st;
   char path[128];

   if (fstat(screen->drm->fd, &st))
      return false;

   snprintf(path, sizeof(path),
            "/sys/dev/char/%u:%u/device/encoder_busy_percent",
            major(st.st_rdev), minor(st.st_rdev));
   return access(path, F_OK) == 0;
}

int
nouveau_nvenc_param(struct nouveau_screen *screen,
                    enum pipe_video_profile profile,
                    enum pipe_video_cap param)
{
   if (screen->device->chipset != 0x177 ||
       !debug_get_bool_option("NOUVEAU_NVENC", true) ||
       !profile_supported(profile) || !kernel_supported(screen))
      return 0;

   bool hevc = nvenc_profile_is_hevc(backend_profile(profile));

   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTED:
   case PIPE_VIDEO_CAP_SUPPORTS_PROGRESSIVE:
   case PIPE_VIDEO_CAP_ENC_MAX_SLICES_PER_FRAME:
   case PIPE_VIDEO_CAP_MAX_TEMPORAL_LAYERS:
   case PIPE_VIDEO_CAP_ENC_QUALITY_LEVEL:
      return 1;
   case PIPE_VIDEO_CAP_ENC_DRIVER_GENERATED_HEADERS:
      return hevc ? 0 : 1;
   case PIPE_VIDEO_CAP_ENC_RATE_CONTROL_MODES:
      return (1 << PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE) |
             (1 << PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT) |
              (1 << PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE);
   case PIPE_VIDEO_CAP_ENC_MAX_REFERENCES_PER_FRAME:
      return hevc ? 1 : 0; /* one L0 reference, no L1/B frames */
   case PIPE_VIDEO_CAP_ENC_HEVC_PREDICTION_DIRECTION:
      return hevc ? PIPE_H265_PRED_DIRECTION_PREVIOUS : 0;
   case PIPE_VIDEO_CAP_ENC_HEVC_FEATURE_FLAGS: {
      if (!hevc)
         return 0;
      union pipe_h265_enc_cap_features f = { .value = 0 };
      f.bits.config_supported = 1;
      f.bits.amp = f.bits.sao = f.bits.cu_qp_delta = 2; /* required by the emitted SPS/PPS */
      f.bits.transform_skip = profile == PIPE_VIDEO_PROFILE_HEVC_MAIN ? 2 : 0;
      return f.value;
   }
   case PIPE_VIDEO_CAP_ENC_HEVC_BLOCK_SIZES: {
      if (!hevc)
         return 0;
      union pipe_h265_enc_cap_block_sizes b = { .value = 0 };
      b.bits.config_supported = 1;
      b.bits.log2_max_coding_tree_block_size_minus3 = 2;
      b.bits.log2_min_coding_tree_block_size_minus3 = 2;
      b.bits.log2_min_luma_coding_block_size_minus3 = 1;
      b.bits.log2_max_luma_transform_block_size_minus2 = 3;
      b.bits.max_max_transform_hierarchy_depth_inter = b.bits.min_max_transform_hierarchy_depth_inter = 3;
      b.bits.max_max_transform_hierarchy_depth_intra = b.bits.min_max_transform_hierarchy_depth_intra = 3;
      return b.value;
   }
   case PIPE_VIDEO_CAP_MAX_WIDTH:
      return hevc ? 3840 : 1920;
   case PIPE_VIDEO_CAP_MAX_HEIGHT:
      return hevc ? 2160 : 1088;
   case PIPE_VIDEO_CAP_MIN_WIDTH:
      return 160;
   case PIPE_VIDEO_CAP_MIN_HEIGHT:
      return 96;
   case PIPE_VIDEO_CAP_MAX_MACROBLOCKS:
      return hevc ? (3840 / 16) * (2160 / 16) : 8160;
   case PIPE_VIDEO_CAP_ENC_SUPPORTS_FEEDBACK_METADATA:
      return PIPE_VIDEO_FEEDBACK_METADATA_TYPE_ENCODE_RESULT |
             PIPE_VIDEO_FEEDBACK_METADATA_TYPE_AVERAGE_FRAME_QP;
   default:
      return 0;
   }
}

static void
feedback_free(struct nvenc_feedback *feedback)
{
   pipe_resource_reference(&feedback->destination, NULL);
   free(feedback);
}

static void
destroy_codec(struct pipe_video_codec *base)
{
   struct nvenc_codec *codec = (struct nvenc_codec *)base;

   list_for_each_entry_safe(struct nvenc_feedback, feedback, &codec->pending,
                            head) feedback_free(feedback);
   nvenc_backend_destroy(codec->backend);
   simple_mtx_destroy(&codec->lock);
   free(codec);
}

static void
begin_hevc_frame(struct pipe_video_codec *base, struct pipe_video_buffer *target,
                  struct pipe_picture_desc *picture)
{
   struct nvenc_codec *codec = (struct nvenc_codec *)base;
   struct pipe_h265_enc_picture_desc *desc = (struct pipe_h265_enc_picture_desc *)picture;
   const struct pipe_h265_enc_rate_control *rate = &desc->rc[0];
   bool inter = desc->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_P;
   bool intra = desc->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_I ||
                 desc->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR;
   bool ten = base->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10;
   codec->error = 0;
   codec->is_p_frame = inter;
   codec->qp = inter ? rate->quant_p_frames : rate->quant_i_frames;
   if (!target || target->buffer_format != (ten ? PIPE_FORMAT_P010 : PIPE_FORMAT_NV12) ||
       target->interlaced || picture->protected_playback || (!inter && !intra) ||
       codec->qp > 51 || desc->num_ref_idx_l0_active_minus1 || desc->num_ref_idx_l1_active_minus1 ||
       desc->seq.chroma_format_idc != 1 || desc->seq.bit_depth_luma_minus8 != (ten ? 2 : 0) ||
       desc->seq.bit_depth_chroma_minus8 != (ten ? 2 : 0))
      codec->error = -ENOTSUP;
   if (rate->rate_ctrl_method == PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT ||
       rate->rate_ctrl_method == PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE) {
      unsigned min_qp = rate->app_requested_qp_range ? rate->min_qp : 0;
      unsigned max_qp = rate->app_requested_qp_range ? MIN2(rate->max_qp, 51) : 51;
      if (!rate->target_bitrate || min_qp > max_qp) {
         codec->error = -EINVAL;
      } else if (rate->rate_ctrl_method != codec->rc_method ||
                 rate->target_bitrate != codec->rc_target || rate->peak_bitrate != codec->rc_peak ||
                 min_qp != codec->rc_min_qp || max_qp != codec->rc_max_qp) {
         nvenc_rc_init(&codec->rc,
                        rate->rate_ctrl_method == PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE ?
                           NVENC_RC_VBR : NVENC_RC_CBR,
                        rate->target_bitrate, rate->peak_bitrate,
                        rate->frame_rate_num, rate->frame_rate_den, rate->vbv_buffer_size,
                        min_qp, max_qp, codec->qp);
         codec->rc_method = rate->rate_ctrl_method;
         codec->rc_target = rate->target_bitrate;
         codec->rc_peak = rate->peak_bitrate;
         codec->rc_min_qp = min_qp;
         codec->rc_max_qp = max_qp;
      }
      codec->qp = nvenc_rc_qp(&codec->rc);
   } else if (rate->rate_ctrl_method == PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE) {
      codec->rc_method = rate->rate_ctrl_method;
   } else {
      codec->error = -ENOTSUP;
   }
   unsigned width = base->width;
   unsigned height = base->height;
   if (desc->seq.conformance_window_flag) {
      unsigned right = 2u * desc->seq.conf_win_right_offset;
      unsigned bottom = 2u * desc->seq.conf_win_bottom_offset;
      if (desc->seq.conf_win_left_offset || desc->seq.conf_win_top_offset ||
          right >= desc->seq.pic_width_in_luma_samples ||
          bottom >= desc->seq.pic_height_in_luma_samples)
         codec->error = -ENOTSUP;
      else {
         width = desc->seq.pic_width_in_luma_samples - right;
         height = desc->seq.pic_height_in_luma_samples - bottom;
      }
   }
   unsigned rate_num = rate->frame_rate_num, rate_den = rate->frame_rate_den;
   if (!codec->error && (!codec->backend || width != codec->backend_width ||
                        height != codec->backend_height || rate_num != codec->backend_rate_num ||
                        rate_den != codec->backend_rate_den)) {
      nvenc_backend_destroy(codec->backend);
      codec->backend = NULL;
      codec->error = nvenc_backend_create(nouveau_screen(base->context->screen)->drm->fd,
                                           width, height, codec->qp, rate_num, rate_den,
                                           backend_profile(base->profile), &codec->backend);
      if (!codec->error) {
         codec->backend_width = width;
         codec->backend_height = height;
         codec->backend_rate_num = rate_num;
         codec->backend_rate_den = rate_den;
      }
   }
   if (picture->out_fence)
      *picture->out_fence = NULL;
}

static void
begin_frame(struct pipe_video_codec *base, struct pipe_video_buffer *target,
            struct pipe_picture_desc *picture)
{
   if (nvenc_profile_is_hevc(backend_profile(base->profile))) {
      begin_hevc_frame(base, target, picture);
      return;
   }
   struct nvenc_codec *codec = (struct nvenc_codec *)base;
   struct pipe_h264_enc_picture_desc *desc =
      (struct pipe_h264_enc_picture_desc *)picture;
   const bool inter = desc->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_P;
   const bool intra = desc->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_I ||
                      desc->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR;

   const struct pipe_h264_enc_rate_control *rate = &desc->rate_ctrl[0];

   codec->error = 0;
   codec->is_p_frame = inter;
   codec->qp = inter ? desc->quant_p_frames : desc->quant_i_frames;
   if (target->buffer_format != PIPE_FORMAT_NV12 || target->interlaced ||
       (!inter && !intra) || codec->qp > 51 || picture->protected_playback)
      codec->error = -ENOTSUP;

   if (rate->rate_ctrl_method ==
          PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT ||
       rate->rate_ctrl_method ==
          PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE) {
      const unsigned min_qp =
         rate->app_requested_qp_range ? rate->min_qp : 10;
      const unsigned max_qp =
         rate->app_requested_qp_range && rate->max_qp < 35 ? rate->max_qp : 35;

      if (!rate->target_bitrate) {
         codec->error = -EINVAL;
      } else if (rate->rate_ctrl_method != codec->rc_method ||
                 rate->target_bitrate != codec->rc_target ||
                 rate->peak_bitrate != codec->rc_peak ||
                 min_qp != codec->rc_min_qp || max_qp != codec->rc_max_qp) {
         nvenc_rc_init(&codec->rc,
                       rate->rate_ctrl_method ==
                             PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE
                          ? NVENC_RC_VBR
                          : NVENC_RC_CBR,
                       rate->target_bitrate, rate->peak_bitrate,
                       rate->frame_rate_num, rate->frame_rate_den,
                       rate->vbv_buffer_size, min_qp, max_qp,
                       codec->qp ? codec->qp : 22);
         codec->rc_method = rate->rate_ctrl_method;
         codec->rc_target = rate->target_bitrate;
         codec->rc_peak = rate->peak_bitrate;
         codec->rc_min_qp = min_qp;
         codec->rc_max_qp = max_qp;
      }
      codec->qp = nvenc_rc_qp(&codec->rc);
   } else {
      codec->rc_method = PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE;
      if (codec->qp == 0)
         codec->qp = 22;
      else if (codec->qp > 35)
         codec->qp = 35;
   }

   unsigned width = base->width, height = base->height;
   if (desc->seq.enc_frame_cropping_flag) {
      width -= 2 * (desc->seq.enc_frame_crop_left_offset +
                    desc->seq.enc_frame_crop_right_offset);
      height -= 2 * (desc->seq.enc_frame_crop_top_offset +
                     desc->seq.enc_frame_crop_bottom_offset);
   }

   const unsigned rate_num = desc->rate_ctrl[0].frame_rate_num;
   const unsigned rate_den = desc->rate_ctrl[0].frame_rate_den;
   if (!codec->error &&
       (!codec->backend || width != codec->backend_width ||
        height != codec->backend_height ||
        rate_num != codec->backend_rate_num ||
        rate_den != codec->backend_rate_den ||
        backend_profile(base->profile) != codec->backend_profile)) {
      nvenc_backend_destroy(codec->backend);
      codec->backend = NULL;
      if (nvenc_backend_create(nouveau_screen(base->context->screen)->drm->fd,
                               width, height, codec->qp, rate_num, rate_den,
                               backend_profile(base->profile),
                               &codec->backend)) {
         codec->error = -EINVAL;
      } else {
         codec->backend_width = width;
         codec->backend_height = height;
         codec->backend_rate_num = rate_num;
         codec->backend_rate_den = rate_den;
         codec->backend_profile = backend_profile(base->profile);
      }
   }

   if (picture->out_fence)
      *picture->out_fence = NULL;
}

static bool
surface_usable(const struct nvenc_codec *codec,
               struct pipe_resource *const *resources,
               struct nvenc_surface *surface)
{
   if (!debug_get_bool_option("NOUVEAU_NVENC_ZERO_COPY", true))
      return false;

   const struct nv50_miptree *luma = nv50_miptree(resources[0]);
   const struct nv50_miptree *chroma = nv50_miptree(resources[1]);
   const unsigned coded_height = align(codec->backend_height,
      nvenc_profile_is_hevc(backend_profile(codec->base.profile)) ? 32 : 16);
   const unsigned pitch = nvenc_backend_pitch(codec->backend);

   if (!luma->base.bo || !chroma->base.bo ||
       luma->base.domain != NOUVEAU_BO_VRAM || chroma->base.domain != NOUVEAU_BO_VRAM ||
       luma->level[0].tile_mode != 0x10 ||
       chroma->level[0].tile_mode != 0x10 || luma->level[0].pitch != pitch ||
       chroma->level[0].pitch != pitch)
      return false;

   const uint64_t luma_offset = luma->base.offset + luma->level[0].offset;
   const uint64_t chroma_offset =
      chroma->base.offset + chroma->level[0].offset;
   const uint64_t chroma_bytes =
      (uint64_t)pitch * align(coded_height / 2, 16);
   if ((luma->base.bo->offset | chroma->base.bo->offset | luma_offset | chroma_offset) & 0xff ||
       luma_offset + (uint64_t)pitch * coded_height > luma->base.bo->size ||
       chroma_offset + chroma_bytes > chroma->base.bo->size ||
       (luma->base.bo == chroma->base.bo &&
        chroma_offset < luma_offset + (uint64_t)pitch * coded_height))
      return false;

   surface->handle = luma->base.bo->handle;
   surface->address = luma->base.bo->offset;
   surface->luma_offset = luma_offset;
   surface->chroma_offset = chroma_offset;
   surface->chroma_handle = chroma->base.bo->handle;
   surface->chroma_address = chroma->base.bo->offset;
   surface->pitch = pitch;
   return true;
}

static bool
pad_surface(struct pipe_context *pipe, struct pipe_resource *const *resources,
            unsigned width, unsigned height, unsigned sample_bytes)
{
   for (unsigned plane = 0; plane < 2; plane++) {
      struct pipe_resource *res = resources[plane];
      const unsigned bpp = (plane ? 2 : 1) * sample_bytes;
      const unsigned visible_w = width >> plane, visible_h = height >> plane;
      const unsigned pad_w = res->width0 - visible_w,
                     pad_h = res->height0 - visible_h;
      struct pipe_transfer *transfer;
      uint8_t *map;

      if (pad_w) {
         map = pipe_texture_map(pipe, res, 0, 0, PIPE_MAP_READ_WRITE,
                                visible_w - 1, 0, pad_w + 1, visible_h,
                                &transfer);
         if (!map)
            return false;
         for (unsigned y = 0; y < visible_h; y++) {
            uint8_t *row = map + (size_t)y * transfer->stride;
            for (unsigned x = 1; x <= pad_w; x++)
               memcpy(row + x * bpp, row, bpp);
         }
         pipe_texture_unmap(pipe, transfer);
      }

      if (pad_h) {
         map = pipe_texture_map(pipe, res, 0, 0, PIPE_MAP_READ_WRITE, 0,
                                visible_h - 1, res->width0, pad_h + 1,
                                &transfer);
         if (!map)
            return false;
         for (unsigned y = 1; y <= pad_h; y++)
            memcpy(map + (size_t)y * transfer->stride, map,
                   (size_t)res->width0 * bpp);
         pipe_texture_unmap(pipe, transfer);
      }
   }
   return true;
}

static void
encode_bitstream(struct pipe_video_codec *base,
                 struct pipe_video_buffer *source,
                 struct pipe_resource *destination, void **feedback_out)
{
   struct nvenc_codec *codec = (struct nvenc_codec *)base;
   struct pipe_context *pipe = base->context;
   struct nvenc_feedback *feedback = CALLOC_STRUCT(nvenc_feedback);
   struct pipe_resource *resources[VL_NUM_COMPONENTS] = {0};
   struct pipe_transfer *transfers[2] = {0};
   const uint8_t *maps[2] = {0};

   *feedback_out = feedback;
   if (!feedback) {
      codec->error = -ENOMEM;
      return;
   }

   feedback->qp = codec->qp;
   pipe_resource_reference(&feedback->destination, destination);
   simple_mtx_lock(&codec->lock);
   list_addtail(&feedback->head, &codec->pending);
   simple_mtx_unlock(&codec->lock);

   if (codec->error || !codec->backend) {
      feedback->error = codec->error ? codec->error : -EINVAL;
      return;
   }

   source->get_resources(source, resources);
   if (!resources[0] || !resources[1]) {
      feedback->error = -EINVAL;
      return;
   }

   struct nvenc_surface surface;
   if (surface_usable(codec, resources, &surface)) {
       if (!pad_surface(pipe, resources, codec->backend_width,
                        codec->backend_height,
                        base->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10 ? 2 : 1)) {
         feedback->error = -EIO;
         return;
      }
      struct pipe_fence_handle *fence = NULL;
      pipe->flush(pipe, &fence, 0);
      if (fence) {
         bool ready = pipe->screen->fence_finish(pipe->screen, pipe, fence, 5000000000ull);
         pipe->screen->fence_reference(pipe->screen, &fence, NULL);
         if (!ready) {
            feedback->error = -ETIMEDOUT;
            return;
         }
      }
      feedback->error = nvenc_backend_submit_surface(
         codec->backend, &surface, codec->qp, codec->is_p_frame, &feedback->ticket);
      return;
   }

   for (unsigned i = 0; i < 2; i++) {
      maps[i] = pipe_texture_map(pipe, resources[i], 0, 0, PIPE_MAP_READ, 0,
                                 0, codec->backend_width >> i,
                                 codec->backend_height >> i, &transfers[i]);
      if (!maps[i]) {
         feedback->error = -EIO;
         goto unmap;
      }
   }

   feedback->error = nvenc_backend_submit(
      codec->backend, maps[0], transfers[0]->stride, maps[1],
      transfers[1]->stride, codec->qp, codec->is_p_frame, &feedback->ticket);

unmap:
   for (unsigned i = 0; i < 2; i++) {
      if (transfers[i])
         pipe_texture_unmap(pipe, transfers[i]);
   }
   if (feedback->error)
      mesa_loge("frame submit failed: %d", feedback->error);
}

static int
end_frame(struct pipe_video_codec *base, struct pipe_video_buffer *target,
          struct pipe_picture_desc *picture)
{
   struct nvenc_codec *codec = (struct nvenc_codec *)base;

   if (codec->error)
      return codec->error;
   base->context->flush(base->context, NULL, 0);
   return 0;
}

static void
flush(struct pipe_video_codec *base)
{
   base->context->flush(base->context, NULL, 0);
}

static void
get_feedback(struct pipe_video_codec *base, void *data, unsigned *size,
             struct pipe_enc_feedback_metadata *metadata)
{
   struct nvenc_codec *codec = (struct nvenc_codec *)base;
   struct nvenc_feedback *feedback = data;
   struct pipe_transfer *transfer = NULL;
   size_t bytes = 0;

   *size = 0;
   simple_mtx_lock(&codec->lock);
   list_del(&feedback->head);
   simple_mtx_unlock(&codec->lock);

   if (!feedback->error) {
      void *map = pipe_buffer_map(base->context, feedback->destination,
                                  PIPE_MAP_WRITE, &transfer);
      if (!map) {
         feedback->error = -EIO;
      } else {
         feedback->error =
            nvenc_backend_collect(codec->backend, &feedback->ticket, map,
                                  feedback->destination->width0, &bytes);
         pipe_buffer_unmap(base->context, transfer);
      }
      if (feedback->error)
         mesa_loge("frame failed: %d", feedback->error);
   }

   if (!feedback->error) {
      *size = bytes;
      if (codec->rc_method == PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT ||
          codec->rc_method == PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE)
         nvenc_rc_update(&codec->rc, bytes);
   }

   if (metadata) {
      memset(metadata, 0, sizeof(*metadata));
      metadata->present_metadata =
         PIPE_VIDEO_FEEDBACK_METADATA_TYPE_ENCODE_RESULT |
         PIPE_VIDEO_FEEDBACK_METADATA_TYPE_AVERAGE_FRAME_QP;
      metadata->encode_result =
         feedback->error ? PIPE_VIDEO_FEEDBACK_METADATA_ENCODE_FLAG_FAILED
                         : PIPE_VIDEO_FEEDBACK_METADATA_ENCODE_FLAG_OK;
      metadata->average_frame_qp = feedback->qp;
   }
   feedback_free(feedback);
}

struct pipe_video_codec *
nouveau_nvenc_create(struct pipe_context *pipe,
                     const struct pipe_video_codec *templ)
{
   if (!nouveau_nvenc_param(nouveau_screen(pipe->screen), templ->profile,
                            PIPE_VIDEO_CAP_SUPPORTED))
      return NULL;

   struct nvenc_codec *codec = CALLOC_STRUCT(nvenc_codec);
   if (!codec)
      return NULL;

   codec->base = *templ;
   pipe_reference_init(&codec->base.reference, 1);
   codec->base.context = pipe;
   codec->base.destroy = destroy_codec;
   codec->base.begin_frame = begin_frame;
   codec->base.encode_bitstream = encode_bitstream;
   codec->base.end_frame = end_frame;
   codec->base.flush = flush;
   codec->base.get_feedback = get_feedback;
   simple_mtx_init(&codec->lock, mtx_plain);
   list_inithead(&codec->pending);
   return &codec->base;
}

#else

int
nouveau_nvenc_param(struct nouveau_screen *screen,
                    enum pipe_video_profile profile,
                    enum pipe_video_cap param)
{
   return 0;
}

struct pipe_video_codec *
nouveau_nvenc_create(struct pipe_context *pipe,
                     const struct pipe_video_codec *templ)
{
   return NULL;
}

#endif

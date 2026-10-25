/*
 * SPDX-License-Identifier: MIT
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 */
#define MESA_LOG_TAG "nouveau-nvenc"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include "drm-uapi/nouveau_drm.h"
#include "nvif/ioctl.h"
#include "clc7b7.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/u_debug.h"

#include "nouveau_nvenc_backend.h"
#include "nouveau_nvenc_h264.h"
#include "nouveau_nvenc_hevc.h"

#define NVENC_SLOTS 4
#define NVENC_TIMEOUT_NS 1000000000ull

enum slot_buffer {
   SLOT_PUSH,
   SLOT_FENCE,
   SLOT_CONFIG,
   SLOT_INPUT_Y,
   SLOT_INPUT_UV,
   SLOT_STATUS,
   SLOT_BITSTREAM,
   SLOT_BUFFERS
};

enum shared_buffer {
   SHARED_RECON_Y_0,
   SHARED_RECON_UV_0,
   SHARED_RECON_Y_1,
   SHARED_RECON_UV_1,
   SHARED_HISTORY,
   SHARED_COLOC,
   SHARED_MEPRED_0,
   SHARED_MEPRED_1,
   SHARED_BUFFERS
};

struct buffer {
   struct drm_nouveau_gem_info info;
   void *map;
};

struct slot {
   struct buffer buffers[SLOT_BUFFERS];
   struct nouveau_nvenc_h264_config config;
   unsigned config_qp;
   unsigned qp;
   unsigned generation;
   uint32_t seq;
   bool is_p_frame;
   bool busy;
   bool stale;
};

struct nvenc_backend {
   int fd;
   struct drm_nouveau_channel_alloc channel;
   struct buffer shared[SHARED_BUFFERS];
   struct slot slots[NVENC_SLOTS];
   unsigned width, height, coded_width, coded_height, pitch, level;
   enum nvenc_profile profile;
   unsigned sequence, generation, frames;
   unsigned recon_idx;
   unsigned poc, rate_num, rate_den, sample_bytes;
   bool has_keyframe;
   bool allocated, poisoned;
   pthread_mutex_t mutex;
};

DEBUG_GET_ONCE_NUM_OPTION(nvenc_fault_frame, "NOUVEAU_NVENC_FAULT_FRAME", 0)

static uint32_t
slot_domain(unsigned index)
{
   return index == SLOT_INPUT_Y || index == SLOT_INPUT_UV
             ? NOUVEAU_GEM_DOMAIN_VRAM
             : NOUVEAU_GEM_DOMAIN_GART;
}

static uint32_t
shared_domain(unsigned index)
{
   return (index == SHARED_RECON_Y_0 || index == SHARED_RECON_UV_0 ||
           index == SHARED_RECON_Y_1 || index == SHARED_RECON_UV_1)
             ? NOUVEAU_GEM_DOMAIN_VRAM
             : NOUVEAU_GEM_DOMAIN_GART;
}

static uint64_t
now_ns(void)
{
   struct timespec time;
   clock_gettime(CLOCK_MONOTONIC, &time);
   return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int
pci_id(const char *path)
{
   FILE *file = fopen(path, "r");
   unsigned value;
   if (!file)
      return -1;
   int ret = fscanf(file, "%x", &value);
   fclose(file);
   return ret == 1 ? (int)value : -1;
}

static int
allocate(int fd, struct buffer *buffer, size_t bytes, uint32_t domain)
{
   struct drm_nouveau_gem_new request = {
      .info.size = bytes,
      .info.domain =
         domain | NOUVEAU_GEM_DOMAIN_MAPPABLE |
         (domain == NOUVEAU_GEM_DOMAIN_GART ? NOUVEAU_GEM_DOMAIN_COHERENT
                                            : 0),
      .align = 4096,
   };
   int ret =
      drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &request, sizeof(request));
   if (ret)
      return ret;
   buffer->info = request.info;
   if (!buffer->info.handle ||
       (buffer->info.domain &
        (NOUVEAU_GEM_DOMAIN_GART | NOUVEAU_GEM_DOMAIN_VRAM)) != domain ||
       buffer->info.size < bytes)
      return -EINVAL;
   buffer->map = mmap(NULL, buffer->info.size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, buffer->info.map_handle);
   if (buffer->map == MAP_FAILED) {
      buffer->map = NULL;
      return -errno;
   }
   memset(buffer->map, 0, buffer->info.size);
   return 0;
}

static void
release(int fd, struct buffer *buffer)
{
   if (buffer->map)
      munmap(buffer->map, buffer->info.size);
   if (buffer->info.handle)
      drmCloseBufferHandle(fd, buffer->info.handle);
}

static void
method(uint32_t **cursor, uint32_t address, uint32_t value)
{
   *(*cursor)++ = 0x20010000 | (4u << 13) | (address >> 2);
   *(*cursor)++ = value;
}

static inline uint64_t
gob_offset(unsigned x, unsigned y, unsigned pitch, unsigned height)
{
   return ((uint64_t)(y / (8 * height)) * (pitch / 64) * height +
           (x / 64) * height + (y / 8) % height) *
             512 +
          (x % 64 / 32) * 256 + (y % 8 / 4) * 128 + (x % 32 / 16) * 64 +
          (y % 4) * 16 + x % 16;
}

static int
make_config(struct nvenc_backend *s, struct slot *slot, unsigned qp)
{
   if (nvenc_profile_is_hevc(s->profile)) {
      slot->config_qp = qp;
      return 0;
   }
   int ret =
      nvenc_make_config(s->coded_width, s->coded_height, qp, &slot->config);
   if (ret)
      return ret;
   nvenc_h264_surface_cfg_s *surfaces[] = {
      &slot->config.picture.input_cfg,
      &slot->config.picture.refpic_cfg,
      &slot->config.picture.outputpic_cfg,
   };
   for (unsigned i = 0; i < 3; i++) {
      surfaces[i]->sfc_pitch = s->pitch;
      surfaces[i]->sfc_pitch_chroma = s->pitch;
      surfaces[i]->tiled_16x16 = 0;
      surfaces[i]->block_height = 2;
   }
   slot->config.picture.sps_data.profile_idc = nvenc_profile_idc(s->profile);
   slot->config.picture.pps_data.entropy_coding_mode_flag =
      s->profile != NVENC_PROFILE_BASELINE;
   slot->config.picture.pps_data.transform_8x8_mode_flag =
      s->profile == NVENC_PROFILE_HIGH;
   if (s->profile == NVENC_PROFILE_HIGH) {
      slot->config.md.intra_luma8x8_mode_enable = 0x1ff;
      slot->config.md.ip_search_mode |= 2;
   }
   slot->config.slice.disable_deblocking_filter_idc = 0;
   slot->config_qp = qp;
   return 0;
}

static int
channel_open(struct nvenc_backend *s)
{
   memset(&s->channel, 0, sizeof(s->channel));
   s->channel.fb_ctxdma_handle = UINT32_MAX;
   s->channel.tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_NVENC;
   int ret = drmCommandWriteRead(s->fd, DRM_NOUVEAU_CHANNEL_ALLOC,
                                 &s->channel, sizeof(s->channel));
   if (ret)
      return ret;
   s->allocated = true;
   const uint32_t handle = 0xbeefb7 + s->generation;
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_new_v0 object;
   } object = {
      .ioctl = {.route = 0xff,
                .token = s->channel.channel,
                .type = NVIF_IOCTL_V0_NEW},
      .object = {.handle = handle,
                 .object = handle,
                 .oclass = NVC7B7_VIDEO_ENCODER,
                 .route = NVIF_IOCTL_V0_ROUTE_NVIF,
                 .token = handle},
   };
   return drmCommandWrite(s->fd, DRM_NOUVEAU_NVIF, &object, sizeof(object));
}

static int
channel_close(struct nvenc_backend *s)
{
   if (!s->allocated)
      return 0;
   struct drm_nouveau_channel_free request = {.channel = s->channel.channel};
   int ret = drmCommandWrite(s->fd, DRM_NOUVEAU_CHANNEL_FREE, &request,
                             sizeof(request));
   if (ret) {
      mesa_loge("channel free failed: %d", ret);
      return -EIO;
   }
   s->allocated = false;
   return 0;
}

static int
channel_reset(struct nvenc_backend *s)
{
   s->generation++;
   s->recon_idx = 0;
   s->has_keyframe = false;
   for (unsigned i = 0; i < NVENC_SLOTS; i++)
      s->slots[i].busy = false;
   int ret = channel_close(s);
   if (!ret)
      ret = channel_open(s);
   if (ret) {
      mesa_loge("channel reset failed: %d", ret);
      s->poisoned = true;
      return -EIO;
   }
   return 0;
}

void
nvenc_backend_destroy(struct nvenc_backend *s)
{
   if (!s)
      return;
   if (channel_close(s)) {
      s->poisoned = true;
      return;
   }
   for (unsigned i = 0; i < NVENC_SLOTS; i++)
      for (unsigned j = 0; j < SLOT_BUFFERS; j++)
         release(s->fd, &s->slots[i].buffers[j]);
   for (unsigned i = 0; i < SHARED_BUFFERS; i++)
      release(s->fd, &s->shared[i]);
   if (s->fd >= 0)
      close(s->fd);
   pthread_mutex_destroy(&s->mutex);
   free(s);
}

int
nvenc_backend_create(int fd, unsigned width, unsigned height, unsigned qp,
                     unsigned frame_rate_num, unsigned frame_rate_den,
                     enum nvenc_profile profile,
                     struct nvenc_backend **output)
{
   bool hevc = nvenc_profile_is_hevc(profile);
   if (fd < 0 || !output || *output || width < 160 || width > (hevc ? 3840 : 1920) ||
       height < 96 || height > (hevc ? 2160 : 1088) || (width & 1) || (height & 1) || qp > 51)
      return -EINVAL;
   struct nvenc_backend *s = calloc(1, sizeof(*s));
   if (!s)
      return -ENOMEM;
   s->fd = -1;
   pthread_mutex_init(&s->mutex, NULL);
   s->width = width;
   s->height = height;
   unsigned alignment = hevc ? 32 : 16;
   s->coded_width = (width + alignment - 1) & ~(alignment - 1);
   s->coded_height = (height + alignment - 1) & ~(alignment - 1);
   s->sample_bytes = profile == NVENC_PROFILE_HEVC_MAIN10 ? 2 : 1;
   s->pitch = ((width * s->sample_bytes) + 63) & ~63u;
   s->rate_num = frame_rate_num;
   s->rate_den = frame_rate_den;
   s->profile = profile;
   s->level =
      nouveau_nvenc_h264_level(width, height, frame_rate_num, frame_rate_den);
   s->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   int ret = s->fd < 0 ? -errno : 0;
   if (ret)
      goto fail;
   struct stat st;
   char path[256];
   if (fstat(s->fd, &st)) {
      ret = -errno;
      goto fail;
   }
   snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/device",
            major(st.st_rdev), minor(st.st_rdev));
   if (pci_id(path) != 0x2584) {
      ret = -ENODEV;
      goto fail;
   }
   ret = channel_open(s);
   if (ret)
      goto fail;
   for (unsigned i = 0; i < SHARED_BUFFERS; i++) {
      size_t bytes = 4 << 20;
      if (hevc && i <= SHARED_RECON_UV_1) {
         unsigned rows = i & 1 ? ((s->coded_height / 2 + 31) & ~31u) : s->coded_height;
         bytes = (size_t)s->pitch * rows + 65536;
      } else if (i == SHARED_MEPRED_0 || i == SHARED_MEPRED_1) {
         bytes = ((size_t)(s->coded_width / 16) * (s->coded_height / 16) * 64 + 0xffff) & ~0xffff;
         if (bytes < 65536)
            bytes = 65536;
      }
      ret = allocate(s->fd, &s->shared[i], bytes, shared_domain(i));
      if (ret)
         goto fail;
   }
   for (unsigned i = 0; i < NVENC_SLOTS; i++) {
      for (unsigned j = 0; j < SLOT_BUFFERS; j++) {
          size_t size = j <= SLOT_CONFIG ? 4096 : (4 << 20);
         if (hevc && (j == SLOT_INPUT_Y || j == SLOT_INPUT_UV)) {
            unsigned rows = j == SLOT_INPUT_Y ? s->coded_height : ((s->coded_height / 2 + 31) & ~31u);
            size = (size_t)s->pitch * rows + 65536;
         }
         if (hevc && j == SLOT_BITSTREAM) {
            size_t worst = (size_t)s->coded_width * s->coded_height * 2 * s->sample_bytes + 4096;
            if (size < worst)
               size = worst;
         }
         if (j == SLOT_STATUS)
            size = (sizeof(nvenc_stat_data_s) + 4095) & ~(size_t)4095;
         ret = allocate(s->fd, &s->slots[i].buffers[j], size, slot_domain(j));
         if (ret)
            goto fail;
      }
      ret = make_config(s, &s->slots[i], qp);
      if (ret)
         goto fail;
   }
   *output = s;
   return 0;
fail:
   nvenc_backend_destroy(s);
   return ret;
}

struct bits {
   uint8_t data[128];
   unsigned count;
};

static void
put_bits(struct bits *b, unsigned v, unsigned n)
{
   for (unsigned i = n; i; i--) {
      b->data[b->count / 8] |= ((v >> (i - 1)) & 1) << (7 - b->count % 8);
      b->count++;
   }
}

static void
put_ue(struct bits *b, unsigned v)
{
   unsigned n = 1;
   for (unsigned x = v + 1; x > 1; x >>= 1)
      n++;
   put_bits(b, 0, n - 1);
   put_bits(b, v + 1, n);
}

static void
put_se(struct bits *b, int v)
{
   put_ue(b, v <= 0 ? -2 * v : 2 * v - 1);
}

static size_t
nal(struct bits *b, uint8_t type, uint8_t *out)
{
   put_bits(b, 1, 1);
   while (b->count % 8)
      put_bits(b, 0, 1);
   memcpy(out, "\0\0\0\1", 4);
   out[4] = type;
   size_t used = 5;
   unsigned zeros = 0;
   for (unsigned i = 0; i < b->count / 8; i++) {
      uint8_t value = b->data[i];
      if (zeros == 2 && value <= 3) {
         out[used++] = 3;
         zeros = 0;
      }
      out[used++] = value;
      zeros = value ? 0 : zeros + 1;
   }
   return used;
}

static size_t
headers(const struct nvenc_backend *s, unsigned qp, uint8_t *out)
{
   struct bits a = {0}, b = {0};
   const bool high = s->profile == NVENC_PROFILE_HIGH;

   put_bits(&a, nvenc_profile_idc(s->profile), 8);
   put_bits(&a, s->profile == NVENC_PROFILE_BASELINE ? 0xc0 : 0x00, 8);
   put_bits(&a, s->level, 8);
   put_ue(&a, 0);
   if (high) {
      put_ue(&a, 1);
      put_ue(&a, 0);
      put_ue(&a, 0);
      put_bits(&a, 0, 1);
      put_bits(&a, 0, 1);
   }
   put_ue(&a, 0);
   put_ue(&a, 2);
   put_ue(&a, 1);
   put_bits(&a, 0, 1);
   put_ue(&a, s->coded_width / 16 - 1);
   put_ue(&a, s->coded_height / 16 - 1);
   put_bits(&a, 1, 1);
   put_bits(&a, 1, 1);
   const bool crop =
      s->width != s->coded_width || s->height != s->coded_height;
   put_bits(&a, crop, 1);
   if (crop) {
      put_ue(&a, 0);
      put_ue(&a, (s->coded_width - s->width) / 2);
      put_ue(&a, 0);
      put_ue(&a, (s->coded_height - s->height) / 2);
   }
   put_bits(&a, 0, 1);

   put_ue(&b, 0);
   put_ue(&b, 0);
   put_bits(&b, s->profile != NVENC_PROFILE_BASELINE, 1);
   put_bits(&b, 0, 1);
   put_ue(&b, 0);
   put_ue(&b, 0);
   put_ue(&b, 0);
   put_bits(&b, 0, 1);
   put_bits(&b, 0, 2);
   put_se(&b, (int)qp - 26);
   put_se(&b, 0);
   put_se(&b, 0);
   put_bits(&b, 1, 1);
   put_bits(&b, 0, 1);
   put_bits(&b, 0, 1);
   if (high) {
      put_bits(&b, 1, 1);
      put_bits(&b, 0, 1);
      put_se(&b, 0);
   }
   size_t bytes = nal(&a, 0x67, out);
   return bytes + nal(&b, 0x68, out + bytes);
}

static void
upload(const struct nvenc_backend *s, struct slot *slot, const uint8_t *y,
       unsigned y_pitch, const uint8_t *uv, unsigned uv_pitch)
{
   const uint8_t *planes[] = {y, uv};
   const unsigned pitches[] = {y_pitch, uv_pitch};
   for (unsigned plane = 0; plane < 2; plane++) {
      unsigned width_bytes = s->width * s->sample_bytes;
      uint8_t *map = slot->buffers[plane ? SLOT_INPUT_UV : SLOT_INPUT_Y].map;
      for (unsigned row = 0; row < (s->coded_height >> plane); row++) {
         unsigned source_y =
             row < (s->height >> plane) ? row : (s->height >> plane) - 1;
         const uint8_t *source =
            planes[plane] + (size_t)source_y * pitches[plane];
         unsigned x = 0;
         /* Each aligned 16-byte run is contiguous inside a GOB. Avoid
          * recalculating the tiled address and copying one byte at a time.
          * Keep the scalar tail for partial runs and edge replication.
          */
         for (; x + 16 <= width_bytes; x += 16)
            memcpy(map + gob_offset(x, row, s->pitch, 2), source + x, 16);
         for (; x < s->pitch; x++) {
            unsigned pixel_bytes = (plane ? 2 : 1) * s->sample_bytes;
            unsigned source_x = x < width_bytes ? x :
                                  width_bytes - pixel_bytes + x % pixel_bytes;
            map[gob_offset(x, row, s->pitch, 2)] =
               source[source_x];
         }
      }
   }
}

static int
push_frame(struct nvenc_backend *s, struct slot *slot,
           const struct nvenc_surface *surface,
           unsigned out_recon, int ref_recon)
{
   struct buffer *push = &slot->buffers[SLOT_PUSH];
   struct buffer *fence = &slot->buffers[SLOT_FENCE];
   const uint64_t luma = surface ? surface->address + surface->luma_offset
                                 : slot->buffers[SLOT_INPUT_Y].info.offset;
   const uint64_t chroma = surface ? surface->chroma_address + surface->chroma_offset
                                   : slot->buffers[SLOT_INPUT_UV].info.offset;
   const unsigned out_y = out_recon == 0 ? SHARED_RECON_Y_0 : SHARED_RECON_Y_1;
   const unsigned out_uv = out_recon == 0 ? SHARED_RECON_UV_0 : SHARED_RECON_UV_1;

   uint32_t *cursor = push->map;
   method(&cursor, 0, NVC7B7_VIDEO_ENCODER);
   bool hevc = nvenc_profile_is_hevc(s->profile);
   method(&cursor, NVC7B7_SET_APPLICATION_ID, hevc ? 3 : 1);
   method(&cursor, NVC7B7_SET_CONTROL_PARAMS, (hevc ? 4 : 3) | 0x100);
   method(&cursor, NVC7B7_SET_PICTURE_INDEX, slot->seq);
   method(&cursor, NVC7B7_SET_IN_DRV_PIC_SETUP, slot->buffers[SLOT_CONFIG].info.offset >> 8);
   method(&cursor, NVC7B7_SET_IN_CUR_PIC, luma >> 8);
   method(&cursor, NVC7B7_SET_IN_CUR_PIC_CHROMA_U, chroma >> 8);
   method(&cursor, NVC7B7_SET_IN_CUR_PIC_CHROMA_V, chroma >> 8);
   method(&cursor, NVC7B7_SET_OUT_REF_PIC_LUMA, s->shared[out_y].info.offset >> 8);
   method(&cursor, NVC7B7_SET_OUT_REF_PIC_CHROMA, s->shared[out_uv].info.offset >> 8);
   const unsigned r_y = ref_recon >= 0 ? (ref_recon == 0 ? SHARED_RECON_Y_0 : SHARED_RECON_Y_1) : out_y;
   const unsigned r_uv = ref_recon >= 0 ? (ref_recon == 0 ? SHARED_RECON_UV_0 : SHARED_RECON_UV_1) : out_uv;
   for (unsigned i = 0; i < 16; i++) {
      method(&cursor, NVC7B7_SET_IN_REF_PIC0_LUMA + i * 4, s->shared[r_y].info.offset >> 8);
      method(&cursor, NVC7B7_SET_IN_REF_PIC0_CHROMA + i * 4, s->shared[r_uv].info.offset >> 8);
   }
   method(&cursor, NVC7B7_SET_IOHISTORY, s->shared[SHARED_HISTORY].info.offset >> 8);
   method(&cursor, NVC7B7_SET_OUT_ENC_STATUS, slot->buffers[SLOT_STATUS].info.offset >> 8);
   method(&cursor, NVC7B7_SET_OUT_BITSTREAM, slot->buffers[SLOT_BITSTREAM].info.offset >> 8);
   if (!hevc) {
      method(&cursor, NVC7B7_SET_IN_COLOC_DATA, s->shared[SHARED_COLOC].info.offset >> 8);
      method(&cursor, NVC7B7_SET_IN_PARTITION_BUF, slot->buffers[SLOT_BITSTREAM].info.offset >> 8);
      method(&cursor, NVC7B7_SET_OUT_COLOC_DATA, s->shared[SHARED_COLOC].info.offset >> 8);
   }
   if (slot->is_p_frame) {
      const unsigned me_in = ref_recon == 0 ? SHARED_MEPRED_0 : SHARED_MEPRED_1;
      const unsigned me_out = out_recon == 0 ? SHARED_MEPRED_0 : SHARED_MEPRED_1;
      method(&cursor, NVC7B7_SET_IN_MEPRED_DATA, s->shared[me_in].info.offset >> 8);
      method(&cursor, NVC7B7_SET_OUT_MEPRED_DATA, s->shared[me_out].info.offset >> 8);
   }
   method(&cursor, NVC7B7_EXECUTE, 0);
   method(&cursor, NVC7B7_SEMAPHORE_A, fence->info.offset >> 32);
   method(&cursor, NVC7B7_SEMAPHORE_B, fence->info.offset);
   method(&cursor, NVC7B7_SEMAPHORE_C, slot->seq);
   method(&cursor, NVC7B7_SEMAPHORE_D, 0);

   struct bo_entry {
      uint32_t handle;
      uint64_t offset;
      uint32_t domain;
      bool read_only;
   } list[24];
   unsigned count = 0;
#define ADD_BO(buf, dom, ro)                                                 \
   (list[count++] =                                                          \
       (struct bo_entry){(buf)->info.handle, (buf)->info.offset, dom, ro})
   ADD_BO(push, NOUVEAU_GEM_DOMAIN_GART, true);
   ADD_BO(fence, NOUVEAU_GEM_DOMAIN_GART, false);
   ADD_BO(&slot->buffers[SLOT_CONFIG], NOUVEAU_GEM_DOMAIN_GART, true);
   if (surface) {
       list[count++] = (struct bo_entry){surface->handle, surface->address,
                                         NOUVEAU_GEM_DOMAIN_VRAM, true};
      if (surface->chroma_handle != surface->handle)
         list[count++] = (struct bo_entry){surface->chroma_handle, surface->chroma_address,
                                           NOUVEAU_GEM_DOMAIN_VRAM, true};
   } else {
      ADD_BO(&slot->buffers[SLOT_INPUT_Y], NOUVEAU_GEM_DOMAIN_VRAM, true);
      ADD_BO(&slot->buffers[SLOT_INPUT_UV], NOUVEAU_GEM_DOMAIN_VRAM, true);
   }
   ADD_BO(&slot->buffers[SLOT_STATUS], NOUVEAU_GEM_DOMAIN_GART, false);
   ADD_BO(&slot->buffers[SLOT_BITSTREAM], NOUVEAU_GEM_DOMAIN_GART, false);
   ADD_BO(&s->shared[out_y], NOUVEAU_GEM_DOMAIN_VRAM, false);
   ADD_BO(&s->shared[out_uv], NOUVEAU_GEM_DOMAIN_VRAM, false);
   if (ref_recon >= 0) {
      const unsigned ref_y = ref_recon == 0 ? SHARED_RECON_Y_0 : SHARED_RECON_Y_1;
      const unsigned ref_uv = ref_recon == 0 ? SHARED_RECON_UV_0 : SHARED_RECON_UV_1;
      ADD_BO(&s->shared[ref_y], NOUVEAU_GEM_DOMAIN_VRAM, true);
      ADD_BO(&s->shared[ref_uv], NOUVEAU_GEM_DOMAIN_VRAM, true);
   }
   ADD_BO(&s->shared[SHARED_HISTORY], NOUVEAU_GEM_DOMAIN_GART, false);
   ADD_BO(&s->shared[SHARED_COLOC], NOUVEAU_GEM_DOMAIN_GART, false);
   if (slot->is_p_frame) {
      const unsigned me_in = ref_recon == 0 ? SHARED_MEPRED_0 : SHARED_MEPRED_1;
      const unsigned me_out = out_recon == 0 ? SHARED_MEPRED_0 : SHARED_MEPRED_1;
      ADD_BO(&s->shared[me_in], NOUVEAU_GEM_DOMAIN_GART, true);
      ADD_BO(&s->shared[me_out], NOUVEAU_GEM_DOMAIN_GART, false);
   }
#undef ADD_BO
   struct drm_nouveau_gem_pushbuf_bo bos[ARRAY_SIZE(list)] = {0};
   for (unsigned i = 0; i < count; i++) {
      bos[i] = (struct drm_nouveau_gem_pushbuf_bo){
         .handle = list[i].handle,
         .read_domains = list[i].domain,
         .write_domains = list[i].read_only ? 0 : list[i].domain,
         .valid_domains = list[i].domain,
         .presumed = {.valid = 1,
                      .domain = list[i].domain,
                      .offset = list[i].offset},
      };
   }
   struct drm_nouveau_gem_pushbuf_push segment = {
      .bo_index = 0,
      .length = (cursor - (uint32_t *)push->map) * 4,
   };
   struct drm_nouveau_gem_pushbuf submit = {
      .channel = s->channel.channel,
      .nr_buffers = count,
      .buffers = (uintptr_t)bos,
      .nr_push = 1,
      .push = (uintptr_t)&segment,
   };
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
   int ret = drmCommandWriteRead(s->fd, DRM_NOUVEAU_GEM_PUSHBUF, &submit,
                                 sizeof(submit));
   if (ret)
      return ret;
   for (unsigned i = 0; i < count; i++) {
      if (bos[i].presumed.valid && bos[i].presumed.offset == list[i].offset)
         continue;
      if (surface && (list[i].handle == surface->handle || list[i].handle == surface->chroma_handle))
         slot->stale = true;
      else
         return -EIO;
   }
   return 0;
}

static int
surface_wait(int fd, uint32_t handle)
{
   int dmabuf = -1;
   if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &dmabuf))
      return -errno;

   struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_READ,
                                             .fd = -1};
   int ret =
      ioctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) ? -errno : 0;
   close(dmabuf);
   if (ret)
      return ret;

   struct pollfd pfd = {.fd = export.fd, .events = POLLIN};
   ret = poll(&pfd, 1, 5000);
   close(export.fd);
   return ret > 0 ? 0 : (ret == 0 ? -ETIMEDOUT : -errno);
}

static int
submit(struct nvenc_backend *s, const uint8_t *y, unsigned y_pitch,
       const uint8_t *uv, unsigned uv_pitch,
       const struct nvenc_surface *surface, unsigned qp,
       bool is_p_frame,
       struct nvenc_ticket *ticket)
{
   pthread_mutex_lock(&s->mutex);
   int ret = 0;
   struct slot *slot = NULL;
   if (s->poisoned) {
      ret = -EIO;
      goto done;
   }
   for (unsigned i = 0; i < NVENC_SLOTS; i++) {
      if (!s->slots[i].busy) {
         slot = &s->slots[i];
         ticket->slot = i;
         break;
      }
   }
   if (!slot) {
      ret = -EBUSY;
      goto done;
   }
   if (slot->config_qp != qp && (ret = make_config(s, slot, qp)))
      goto done;
   if (surface) {
       ret = surface_wait(s->fd, surface->handle);
      if (!ret && surface->chroma_handle != surface->handle)
         ret = surface_wait(s->fd, surface->chroma_handle);
      if (ret)
         goto done;
   } else {
      upload(s, slot, y, y_pitch, uv, uv_pitch);
   }

   if (!s->has_keyframe)
      is_p_frame = false;

   unsigned out_recon;
   int ref_recon;
   if (is_p_frame) {
      ref_recon = s->recon_idx;
      out_recon = 1 - s->recon_idx;
      s->recon_idx = out_recon;
   } else {
      out_recon = 0;
      ref_recon = -1;
      s->recon_idx = 0;
      s->has_keyframe = true;
   }

   struct nouveau_nvenc_h264_config config = slot->config;
   config.picture.pic_control.pic_type = 3;
   config.picture.pic_control.pic_struct = 0;
   config.picture.pic_control.ref_pic_flag = 1;
   config.picture.pic_control.frame_num = 0;
   config.picture.pic_control.idr_pic_id = s->frames & 0xffff;
   for (unsigned i = 0; i < 8; i++) {
      config.picture.pic_control.l0[i] = -1;
      config.picture.pic_control.l1[i] = -1;
   }
   memset(config.picture.pic_control.temp_dist_l0, 0, 152);
   config.slice.force_intra = 1;
   config.slice.disable_deblocking_filter_idc = 0;
   config.slice.qp_avr = qp;
   config.slice.qp_slice_min = qp;
   config.slice.qp_slice_max = qp;

   const unsigned fault_frame = debug_get_option_nvenc_fault_frame();
   if (fault_frame && ++s->frames == fault_frame) {
      mesa_logw("injecting fault on frame %u", s->frames);
      memset(&config.me, 0, sizeof(config.me));
      memset(&config.md, 0, sizeof(config.md));
      memset(&config.quant, 0, sizeof(config.quant));
      memset(&config.slice, 0, sizeof(config.slice));
   } else if (!fault_frame) {
      s->frames++;
   }

   if (nvenc_profile_is_hevc(s->profile)) {
      s->poc = is_p_frame ? s->poc + 1 : 0;
      struct nvenc_hevc_params params = {
         .width = s->width, .height = s->height,
         .bit_depth = s->sample_bytes == 2 ? 10 : 8, .qp = qp,
         .pitch_bytes = s->pitch, .history_bytes = s->shared[SHARED_HISTORY].info.size,
         .bitstream_bytes = slot->buffers[SLOT_BITSTREAM].info.size,
         .rate_num = s->rate_num, .rate_den = s->rate_den,
         .poc = s->poc, .ref_slot = ref_recon >= 0 ? ref_recon : 0, .idr = !is_p_frame,
      };
      if (!nouveau_nvenc_hevc_config(slot->buffers[SLOT_CONFIG].map, &params)) {
         ret = -EINVAL;
         goto done;
      }
   } else {
      memcpy(slot->buffers[SLOT_CONFIG].map, &config, sizeof(config));
   }
   memset(slot->buffers[SLOT_STATUS].map, 0xcd, 128);
   __atomic_store_n((uint32_t *)slot->buffers[SLOT_FENCE].map, 0,
                    __ATOMIC_RELEASE);
   slot->seq = ++s->sequence ? s->sequence : ++s->sequence;
   slot->qp = qp;
   slot->is_p_frame = is_p_frame;
   slot->stale = false;
   slot->generation = s->generation;
   ret = push_frame(s, slot, surface, out_recon, ref_recon);
   if (ret) {
      channel_reset(s);
      goto done;
   }
   slot->busy = true;
   ticket->generation = slot->generation;
   ticket->seq = slot->seq;
done:
   pthread_mutex_unlock(&s->mutex);
   return ret;
}

int
nvenc_backend_submit(struct nvenc_backend *s, const uint8_t *y,
                     unsigned y_pitch, const uint8_t *uv, unsigned uv_pitch,
                     unsigned qp, bool is_p_frame,
                     struct nvenc_ticket *ticket)
{
   if (!s || !y || !uv || !ticket || qp > 51 || y_pitch < s->width * s->sample_bytes ||
       uv_pitch < s->width * s->sample_bytes)
      return -EINVAL;
   return submit(s, y, y_pitch, uv, uv_pitch, NULL, qp, is_p_frame, ticket);
}

unsigned
nvenc_backend_pitch(const struct nvenc_backend *s)
{
   return s->pitch;
}

int
nvenc_backend_submit_surface(struct nvenc_backend *s,
                             const struct nvenc_surface *surface, unsigned qp,
                             bool is_p_frame,
                             struct nvenc_ticket *ticket)
{
   if (!s || !surface || !ticket || qp > 51 || !surface->handle || !surface->chroma_handle ||
       surface->pitch != s->pitch || (surface->address & 0xff) ||
       (surface->chroma_address & 0xff) ||
       (surface->luma_offset & 0xff) || (surface->chroma_offset & 0xff))
      return -EINVAL;
   mesa_log(MESA_LOG_DEBUG, MESA_LOG_TAG,
            "direct Gallium input: luma GEM %u, chroma GEM %u",
            surface->handle, surface->chroma_handle);
   return submit(s, NULL, 0, NULL, 0, surface, qp, is_p_frame, ticket);
}

int
nvenc_backend_collect(struct nvenc_backend *s,
                      const struct nvenc_ticket *ticket, void *output,
                      size_t capacity, size_t *bytes)
{
   if (!s || !ticket || !output || !bytes || ticket->slot >= NVENC_SLOTS)
      return -EINVAL;
   *bytes = 0;
   struct slot *slot = &s->slots[ticket->slot];
   pthread_mutex_lock(&s->mutex);
   int ret = 0;
   if (ticket->generation != s->generation || !slot->busy ||
       slot->seq != ticket->seq) {
      ret = -EIO;
      goto done;
   }
   pthread_mutex_unlock(&s->mutex);

   volatile uint32_t *fence = slot->buffers[SLOT_FENCE].map;
   const uint64_t deadline = now_ns() + NVENC_TIMEOUT_NS;
   bool signalled = true;
   while (__atomic_load_n(fence, __ATOMIC_ACQUIRE) != ticket->seq) {
      if (now_ns() >= deadline) {
         signalled = false;
         break;
      }
      struct timespec sleep = {.tv_nsec = 50000};
      nanosleep(&sleep, NULL);
   }

   pthread_mutex_lock(&s->mutex);
   if (ticket->generation != s->generation) {
      ret = -EIO;
      goto done;
   }
   if (!signalled) {
      nvenc_pic_stat_s stat;
      memcpy(&stat, slot->buffers[SLOT_STATUS].map, sizeof(stat));
      uint32_t *dwords = (uint32_t *)slot->buffers[SLOT_STATUS].map;
      mesa_loge("STATUS DUMP (32 dwords):");
      for (int i = 0; i < 32; i += 4) {
         mesa_loge("  [%02d..%02d]: 0x%08x 0x%08x 0x%08x 0x%08x",
                   i, i + 3, dwords[i], dwords[i+1], dwords[i+2], dwords[i+3]);
      }
      ret = -ETIMEDOUT;
      channel_reset(s);
      goto done;
   }
   if (slot->stale) {
      slot->busy = false;
      ret = -EAGAIN;
      goto done;
   }
   nvenc_pic_stat_s stat;
   memcpy(&stat, slot->buffers[SLOT_STATUS].map, sizeof(stat));
   if (stat.picture_index != ticket->seq || stat.error_status != 2 ||
       stat.ucode_error_status || (stat.pic_type != 3 && stat.pic_type != 0) ||
       stat.num_slices != 1 || !stat.total_bit_count ||
       stat.total_bit_count > slot->buffers[SLOT_BITSTREAM].info.size * 8) {
      ret = -EIO;
      channel_reset(s);
      goto done;
   }
   uint8_t parameter_sets[256];
   size_t prefix = 0;
   if (nvenc_profile_is_hevc(s->profile)) {
      if (!slot->is_p_frame) {
         const struct nvenc_hevc_params params = {
            .width = s->width, .height = s->height,
            .bit_depth = s->sample_bytes == 2 ? 10 : 8,
            .rate_num = s->rate_num, .rate_den = s->rate_den,
         };
         prefix = nouveau_nvenc_hevc_headers(&params, parameter_sets, sizeof(parameter_sets));
         if (!prefix) {
            ret = -EINVAL;
            slot->busy = false;
            goto done;
         }
      }
   } else {
      prefix = headers(s, slot->qp, parameter_sets);
      if (!prefix) {
         ret = -EINVAL;
         slot->busy = false;
         goto done;
      }
   }
   const size_t coded = (stat.total_bit_count + 7) / 8;
   if (prefix + coded > capacity) {
      ret = -ENOSPC;
      slot->busy = false;
      goto done;
   }
   if (prefix)
      memcpy(output, parameter_sets, prefix);
   memcpy((uint8_t *)output + prefix, slot->buffers[SLOT_BITSTREAM].map,
          coded);
   *bytes = prefix + coded;
   slot->busy = false;
done:
   pthread_mutex_unlock(&s->mutex);
   return ret;
}

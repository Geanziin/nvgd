/*
 * SPDX-License-Identifier: MIT
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 */
#ifndef NOUVEAU_NVENC_BACKEND_H
#define NOUVEAU_NVENC_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NVENC_MAX_OUTPUT ((4u << 20) + 256u)

struct nouveau_nvenc_h264_config;

enum nvenc_profile {
   NVENC_PROFILE_BASELINE,
   NVENC_PROFILE_MAIN,
   NVENC_PROFILE_HIGH,
   NVENC_PROFILE_HEVC_MAIN,
   NVENC_PROFILE_HEVC_MAIN10,
};

static inline bool
nvenc_profile_is_hevc(enum nvenc_profile profile)
{
   return profile == NVENC_PROFILE_HEVC_MAIN || profile == NVENC_PROFILE_HEVC_MAIN10;
}

static inline unsigned
nvenc_profile_idc(enum nvenc_profile profile)
{
   return profile == NVENC_PROFILE_HIGH   ? 100
          : profile == NVENC_PROFILE_MAIN ? 77
                                          : 66;
}
struct nvenc_backend;

struct nvenc_surface {
   uint32_t handle;
   uint64_t address;
   uint32_t luma_offset;
   uint32_t chroma_offset;
   /* Gallium video planes need not share one GEM object. */
   uint32_t chroma_handle;
   uint64_t chroma_address;
   unsigned pitch;
};

struct nvenc_ticket {
   unsigned slot;
   unsigned generation;
   uint32_t seq;
};

int nvenc_make_config(unsigned width, unsigned height, unsigned qp,
                      struct nouveau_nvenc_h264_config *output);

int nvenc_backend_create(int fd, unsigned width, unsigned height, unsigned qp,
                         unsigned frame_rate_num, unsigned frame_rate_den,
                         enum nvenc_profile profile,
                         struct nvenc_backend **output);
int nvenc_backend_submit(struct nvenc_backend *s, const uint8_t *y,
                         unsigned y_pitch, const uint8_t *uv,
                         unsigned uv_pitch, unsigned qp,
                         bool is_p_frame,
                         struct nvenc_ticket *ticket);
int nvenc_backend_submit_surface(struct nvenc_backend *s,
                                 const struct nvenc_surface *surface,
                                 unsigned qp, bool is_p_frame,
                                 struct nvenc_ticket *ticket);
unsigned nvenc_backend_pitch(const struct nvenc_backend *s);
int nvenc_backend_collect(struct nvenc_backend *s,
                          const struct nvenc_ticket *ticket, void *output,
                          size_t capacity, size_t *bytes);
void nvenc_backend_destroy(struct nvenc_backend *s);

#endif

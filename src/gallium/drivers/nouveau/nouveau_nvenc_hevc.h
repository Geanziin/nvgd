/* SPDX-License-Identifier: MIT */
#ifndef NOUVEAU_NVENC_HEVC_H
#define NOUVEAU_NVENC_HEVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOUVEAU_NVENC_HEVC_CONFIG_SIZE 4096

struct nvenc_hevc_params {
   unsigned width, height, bit_depth, qp;
   unsigned pitch_bytes, history_bytes, bitstream_bytes;
   unsigned rate_num, rate_den;
   unsigned poc, ref_slot;
   bool idr;
};

bool nouveau_nvenc_hevc_config(void *output, const struct nvenc_hevc_params *params);
size_t nouveau_nvenc_hevc_headers(const struct nvenc_hevc_params *params,
                                  void *output, size_t capacity);

#endif

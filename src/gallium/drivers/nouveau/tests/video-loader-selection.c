/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_LOADER_DEVICE_PCI 1
#define FREE free
#include "video-loader-policy.inc"
static const char *setting;
static const char *os_get_option(const char *name)
{
   assert(!strcmp(name, "MESA_LOADER_DRIVER_OVERRIDE"));
   return setting;
}

struct mock_device {
   struct {
      int type;
      struct { struct { unsigned vendor_id, chip_id; } pci; } u;
      char *driver_name;
   } base;
};

static char *select_driver(bool video, bool zink, unsigned vendor, unsigned chip,
                          int type, const char *name, const char *option)
{
   struct mock_device object = { .base = {
      .type = type, .u.pci = { vendor, chip }, .driver_name = strdup(name),
   } };
   struct mock_device *ddev = &object;
   setting = option;
#include "video-loader-selection.inc"
   return ddev->base.driver_name;
fail:
   free(ddev->base.driver_name);
   return NULL;
}

static void expect(bool video, bool zink, unsigned vendor, unsigned chip,
                   int type, const char *name, const char *option, const char *expected)
{
   char *selected = select_driver(video, zink, vendor, chip, type, name, option);
   assert(selected && !strcmp(selected, expected));
   free(selected);
}

int main(void)
{
   const unsigned ada_pci_ids[] = {
      0x2684, 0x2685, 0x2689, 0x2702, 0x2704, 0x2705, 0x2709, 0x2717,
      0x2757, 0x2782, 0x2783, 0x2786, 0x2788, 0x2803, 0x2805, 0x2808,
      0x2820, 0x2860, 0x2882, 0x28a0, 0x28a1, 0x28e0, 0x28e1,
   };
   for (unsigned i = 0; i < sizeof(ada_pci_ids) / sizeof(ada_pci_ids[0]); i++) {
      unsigned id = ada_pci_ids[i];
      expect(true, false, 0x10de, id, 1, "zink", NULL, "nouveau");
      expect(false, false, 0x10de, id, 1, "zink", NULL, "zink");
      expect(true, true, 0x10de, id, 1, "zink", NULL, "zink");
      expect(true, false, 0x10de, id, 1, "zink", "zink", "zink");
      expect(true, false, 0x1002, id, 1, "zink", NULL, "zink");
      expect(true, false, 0x10de, id, 0, "zink", NULL, "zink");
   }
   const unsigned other_ids[] = {0x177, 0x192, 0x193, 0x194, 0x196, 0x197,
                                  0x2582, 0x2789, 0x2900, 0xffff};
   for (unsigned i = 0; i < sizeof(other_ids) / sizeof(other_ids[0]); i++)
      expect(true, false, 0x10de, other_ids[i], 1, "zink", NULL, "zink");
   expect(true, false, 0x10de, 0x2584, 1, "zink", NULL, "nouveau");
   expect(false, false, 0x10de, 0x2584, 1, "zink", NULL, "zink");
   expect(true, true, 0x10de, 0x2584, 1, "zink", NULL, "zink");
   expect(true, false, 0x10de, 0x2584, 1, "zink", "zink", "zink");
   expect(true, false, 0x10de, 0x2584, 1, "zink", "", "nouveau");
   expect(true, false, 0x10de, 0x2582, 1, "zink", NULL, "zink");
   expect(true, false, 0x1002, 0x2584, 1, "zink", NULL, "zink");
   expect(true, false, 0x10de, 0x2584, 0, "zink", NULL, "zink");
   expect(true, false, 0x10de, 0x2584, 1, "nouveau", NULL, "nouveau");
   puts("Production video loader policy: GA107 + 23 Ada PCI IDs, chipset numbers rejected, graphics and explicit overrides preserved PASS (fixtures)");
   return 0;
}

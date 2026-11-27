#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the methods actually used by the native NVDEC/NVENC submissions."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[5]
driver = root / 'src/gallium/drivers/nouveau'
headers = root / 'src/nouveau/headers/nvidia'


def definitions(path, prefix):
    return {name: int(value, 16) for name, value in re.findall(
        r'#define ' + prefix + r'_(\w+)\s+\(0x([0-9a-fA-F]+)\)',
        path.read_text())}


for source, old, new, directory in [
    ('nouveau_nvdec.c', ('C5B0', 'C7B0'), 'C9B0', 'classes'),
    ('nouveau_nvenc_backend.c', ('C7B7',), 'C9B7', 'classes'),
]:
    reference = {}
    for name in old:
        reference.update(definitions(headers / directory / f'cl{name.lower()}.h', 'NV' + name))
    candidate = definitions(headers / directory / f'cl{new.lower()}.h', 'NV' + new)
    text = (driver / source).read_text()
    used = set(re.findall(r'NV(?:' + '|'.join(old) + r')_(\w+)', text))
    used -= {'VIDEO_DECODER', 'VIDEO_ENCODER'}
    for name in used:
        assert name in reference and name in candidate, (source, name, 'missing definition')
        assert reference[name] == candidate[name], (source, name, reference[name], candidate[name])
    print(f'{source}: {len(used)} used methods/enums match {new}: PASS')

loader = (root / 'src/gallium/auxiliary/pipe-loader/pipe_loader_drm.c').read_text()
loader = loader.split('nvidia_video_pci_id(')[1].split('\nstatic bool\n')[0]
backend = (driver / 'nouveau_nvenc_backend.c').read_text()
backend = backend.split('nvenc_oclass(')[1].split('\nstatic bool\n')[0]
cases = lambda text: set(re.findall(r'case (0x[0-9a-fA-F]+):', text))
assert cases(loader) == cases(backend), 'video loader / NVENC PCI gates differ'
print(f'loader and NVENC agree on {len(cases(loader))} PCI IDs: PASS')

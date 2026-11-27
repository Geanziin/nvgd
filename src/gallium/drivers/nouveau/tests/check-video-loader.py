#!/usr/bin/env python3
from pathlib import Path
import subprocess

import tempfile

ROOT = Path(__file__).resolve().parents[5]
TMP = tempfile.TemporaryDirectory(prefix='nvgd-video-loader-')
BUILD = Path(TMP.name)
source = (ROOT / 'src/gallium/auxiliary/pipe-loader/pipe_loader_drm.c').read_text()
helper_start = source.index('static bool\nnvidia_video_pci_id(')
helper_end = source.index('\nstatic bool\npipe_loader_drm_probe_fd_nodup', helper_start)
(BUILD / 'video-loader-policy.inc').write_text(source[helper_start:helper_end])
start = source.index('   const char *override = os_get_option("MESA_LOADER_DRIVER_OVERRIDE");')
end = source.index('\n   /* For the closed source AMD', start)
(BUILD / 'video-loader-selection.inc').write_text(source[start:end])
subprocess.run(['cc','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(BUILD),
                str(Path(__file__).with_name('video-loader-selection.c')),'-o',str(BUILD/'video-loader-selection')], check=True)
subprocess.run([str(BUILD/'video-loader-selection')], check=True)

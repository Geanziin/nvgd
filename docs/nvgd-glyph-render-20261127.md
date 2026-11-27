# nvgd: GNOME glyph corruption after Mesa -9

## Observed failure

The AD104 desktop still showed disconnected characters, black rectangles and
incorrect text colors after a fresh login with Mesa -9. The running GNOME
compositor mapped the current library, without a deleted-file mapping. This
rules out the earlier stale-session explanation for this report.

The installed GA107 reference remains Mesa
`1:27.0.0_devel.r231969.g3e604079149-1`. Its system libgallium SHA-256 is
`e308f41d0e0bab76c421894593938fa502300836c2b43c6b9eaa011132d5adf2`.
The reference PC was not upgraded during this investigation.

## Bounded experiments

All AD104 compositor comparisons used separate headless GNOME instances on
private session buses. The active desktop was not restarted or replaced.
The probe's temporary debug access applied only to those private instances.
No recording tests were run.

| Experiment on AD104 | Result |
| --- | --- |
| Plain Clutter/Pango colored glyph nodes | Correct |
| Additional glyph-atlas pressure | Correct |
| 1500 R8 atlas updates and odd-offset copy | No byte mismatches |
| Full GNOME composition with installed Mesa -9 | Corrupt search text |
| Full app grid with prior debug build, same render path as -9 | Disconnected glyph fragments |
| Full GNOME using the GA107 -1 library on AD104 | Correct search text |
| Mesa -9 with `NVGD_COLOR_COMPRESSION=true` | Still corrupt |
| Candidate restoring the -1 native 2D copy/clip path | Correct search text and app labels |

This demonstrates why the earlier 35 image tests did not establish correctness
of the real GNOME composition. The optimized and debug baselines both fail;
the candidate's result is not attributed merely to a different optimization
level. Compression alone does not explain the regression.

## Source change

Restore the native nvc0 2D copy/clip behavior from the known working private-fork
commit `3e604079149`, limited to `nvc0_surface.c`:

- Remove the Turing+ detour to `util_resource_copy_region` for the compatible
  2D-copy case.
- Allow compatible unscaled blits to use the existing 2D engine instead of
  forcing every Turing+ blit through the 3D blitter.
- Restore the existing CPU rectangle intersection helper for unscaled,
  possibly mirrored copies. Scaled/otherwise incompatible blits retain their
  3D path and its own scissor handling.
- Preserve the -9 clear-template conversion and the concurrent-context fix.

The change restores actual fork code rather than changing shader math, font
assets, colors or UI geometry. nvc0/NAK, NVK and the video implementations are
preserved. The lower-level reason the forced 3D composition corrupts these
glyphs is not fully isolated; it remains `residual_unknown` rather than a claim
about an unobserved register or shader instruction.

## Evidence and validation

Lab evidence is in `artifacts/glyph-20261127/`, including diagnostic sources,
command logs and full PNG captures:

| Image | SHA-256 |
| --- | --- |
| Debug baseline, corrupted app grid | `305dc3b140f94f9f8c869fcd739988d6f850fe2e383eb3089de13bd257d3aca7` |
| Candidate, readable app grid | `c3772cae872335fae66e617dfc4f768a9ce0da872099f8f6084c5ce6acb9ab6a` |
| Final optimized -10 package, readable app grid | `916df8103f6205126b18f37df4e22754f3b3d9a809c2e1acde7aa347c1ad2898` |
| Original GA107 library used on AD104 | `6d16354a35a7394a1196446c2b0a143cfd7dc57173efe938cb65d5e92d2b868e` |

The candidate passes the existing 35 texture/fixed-function image cases,
concurrent-context/compute/buffer checks, and the dedicated color/depth/stencil
surface-clear test on AD104. Package -10 carries the same rendering change in
64-bit and 32-bit builds. Full desktop coverage and other Ada devices remain
bounded by the hardware and scenes actually tested.

## Final optimized packages

The final optimized 64-bit package reproduces the corrected full GNOME app
grid (capture above), not only the diagnostic build. Both package libraries
pass the three-layer partial color/depth/stencil clear test on AD104. The
64-bit package also passes all 35 image cases and concurrency, compute and
buffer checks. The isolated candidate passes the same image suite on GA107;
the reference PC's installed Mesa remains -1.

| Package | SHA-256 |
| --- | --- |
| Mesa 64-bit -10 | `b308aa6e9f1799a8e65b548367b7c644a0dbf901fe4e24f41f701c3121e65c95` |
| Mesa 32-bit -10 | `eafb7603f9353300da629aa6700915db505b59bed86ad8eaf3c3670cc8681a39` |

No kernel or codec source changed in this follow-up. The active desktop must
load the installed libraries on its next login; it was not logged out
automatically. Recording tests remain explicitly deferred to the owner.

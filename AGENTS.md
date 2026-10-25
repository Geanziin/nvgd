# AGENTS.md — private nvgd fork

This repository is the owner's private nvgd project, derived from Mesa but
independent of the upstream Mesa community. At the owner's explicit request,
AI assistance is permitted here, including changes to `src/nouveau` and
`src/gallium/drivers/nouveau`, tests, code comments and project documentation.
This authorization replaces the upstream contribution restrictions previously
contained in this file for work in this private fork only. It does not change
any source-file license, copyright notice or third-party project's policies.

Do not open merge requests, post comments or submit contributions to upstream
Mesa/Nouveau GitLab. Private-fork authorization is not upstream permission.

Commits containing AI-written code must include `Generated-by: LLM`.
Commits containing user-written code assisted by AI must include
`Assisted-by: LLM` as appropriate. Preserve other people's work and never
discard external changes without investigating them first.

Work in the foreground; do not use subagents or background jobs unless the
owner explicitly requests them. The owner performs reboots: never reboot the
machine automatically, and explain when a reboot is needed.

Keep OpenGL and Vulkan native through nvc0/NAK and NVK/NAK. Do not replace
them with Zink or ANGLE or disable NAK, GSP 615, P-State or `type=69`.
Keep video paths native through Gallium/DRM and NVDEC/NVENC; do not silently
fall back to software or another graphics API.

Validate changes before enabling capabilities or publishing packages. Record
commands, hashes, logs, coverage and limitations; do not claim universal or
"100%" correctness from a bounded test suite. Kernel/DKMS changes must build
without warnings or errors. Preserve published benchmark evidence.

Commit only the intended files. When packages change, keep the 64/32-bit
recipes and nvgd-repo consistent, and use `gh` to publish release assets.

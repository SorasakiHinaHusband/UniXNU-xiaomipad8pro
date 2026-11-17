# Source attribution

The following source files are copied from
[UniXNU-QcaBluetooth](https://github.com/UniXNU/UniXNU-QcaBluetooth), commit
`effe4ec5c57cd400a5f818b21c9903fef237edbc` (the component pinned by
UniXNU-sheng at `0eea3a2`):

- `patcher/native_ios27.py`, `patcher/native_macos27.py`, `patcher/inspect-ios.py`
  from `tools/`: BSD-2-Clause-Patent. The iPadOS module provides shared Mach-O
  and instruction helpers; this runtime builder supports macOS 27.3 only.
- `userland/BlueTool.c`: BSD-2-Clause-Patent.
- `userland/Type0.c`: BSD-3-Clause, copyright daimile2 and contributors and
  UniXNU contributors. Its original header is retained.

The accompanying licence texts are in `LICENSES/`. The macOS patch includes
that revision's LE extended-advertising-report mask correction. No upstream
firmware, GPL controller core, Apple executable or disassembly is included.

The runtime builder, mount helper, scan probe and boot script are new
UniXNU-piano code, copyright (c) 2026 Leo Zhang, BSD-2-Clause-Patent.

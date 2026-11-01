# Nintendo Switch FFmpeg 7.1 libnx TLS recipe

This directory contains the reproducible `switch-ffmpeg` package recipe used
for the RumbleNX Switch build. It comes unchanged from kickNX 0.5.0. It adds
Nintendo Switch/libnx TLS support so FFmpeg can open Rumble's HTTPS HLS
playlists directly, plus kickNX's validated H.264-reference and NVTEGRA
hardware-frame transfer guards (the `kicknx_patch_*.py` scripts keep their
original names and markers).

The recipe downloads the official FFmpeg 7.1 source archive and verifies its
SHA-256 hash. Generated source trees, packages and static libraries are not
committed to the repository.

## Build in the devkitPro MSYS2 shell

From this directory:

```bash
makepkg -Csf --noconfirm
```

Extract the generated package into RumbleNX's local dependency root:

```bash
mkdir -p ../../../.rumblenx-build/ffmpeg-local-root
bsdtar -xf switch-ffmpeg-7.1-12-any.pkg.tar.zst \
  -C ../../../.rumblenx-build/ffmpeg-local-root
```

The normal RumbleNX build scripts automatically detect that local root. A custom
location can instead be supplied through `RUMBLENX_FFMPEG_ROOT`; it must point to
the directory containing `include/` and `lib/`.

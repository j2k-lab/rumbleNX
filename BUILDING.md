# Building RumbleNX for Nintendo Switch

## Supported playback stack

RumbleNX uses the Switch playback stack that kickNX 0.5.0 validated:

```text
MPV:    switch-libmpv 0.39.0-4   (devkitPro's package)
FFmpeg: switch-ffmpeg 7.1-12     (built from this repository)
```

The FFmpeg recipe in `scripts/switch/ffmpeg-libnx` adds libnx TLS support and
kickNX's guards for NVTEGRA H.264 references, decode maps and hardware-frame
transfers. MPV is devkitPro's stock package. Using a different FFmpeg may
compile, but it is not the validated playback stack.

The recommended way to build is the Docker image in this repository, which
provides devkitPro, MPV and every other library. You only build the guarded
FFmpeg yourself.

## 1. Build the guarded FFmpeg package

This step uses the devkitPro MSYS2 shell (on Linux, a devkitPro install with
`makepkg`). You only need to repeat it when the FFmpeg recipe changes.

```bash
cd scripts/switch/ffmpeg-libnx
rm -rf src pkg
makepkg -Csf --noconfirm
```

Expected package:

```text
switch-ffmpeg-7.1-12-any.pkg.tar.zst
```

Extract it into the repository-local dependency root:

```bash
cd ../../..
rm -rf .rumblenx-build/ffmpeg-local-root
mkdir -p .rumblenx-build/ffmpeg-local-root
bsdtar -xf scripts/switch/ffmpeg-libnx/switch-ffmpeg-7.1-12-any.pkg.tar.zst \
  -C .rumblenx-build/ffmpeg-local-root
```

The `.rumblenx-build` directory is ignored by Git. CMake picks it up
automatically; the build log then shows
`Using RumbleNX FFmpeg override: .../.rumblenx-build/ffmpeg-local-root/...`.

## 2. Build RumbleNX with Docker (recommended)

Install Docker Desktop (Windows, macOS) or Docker Engine (Linux).

Build the image once from the repository root. Rebuild it only when the
`Dockerfile` changes:

```bash
docker build -t rumblenx-switch-builder .
```

The image is based on `devkitpro/devkita64:latest`, so the library versions
come from devkitPro at the time the image is built. Check that it has the
validated MPV:

```bash
docker run --rm rumblenx-switch-builder dkp-pacman -Q switch-libmpv
```

It should report `switch-libmpv 0.39.0-4`.

Then build RumbleNX. The repository is mounted into the container, so the output
lands in your checkout.

PowerShell:

```powershell
docker run --rm -v "${PWD}:/workspace" -w /workspace rumblenx-switch-builder bash build-switch.sh
```

Linux, macOS or Git Bash:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace rumblenx-switch-builder bash build-switch.sh
```

In Git Bash on Windows, prefix the command with `MSYS_NO_PATHCONV=1` so Git
Bash does not rewrite `/workspace` into a Windows path.

## 3. Build RumbleNX without Docker

Install a current devkitPro Nintendo Switch environment with `devkitA64`,
`libnx`, CMake, Ninja, pkg-config and the packages the `Dockerfile` installs
(`switch-libmpv`, `switch-curl`, `switch-libwebp`, `switch-glfw`, `switch-mesa`
and the others listed there). Borealis and LunaSVG source are included in the
repository.

Windows PowerShell:

```powershell
.\build-switch.ps1
```

To use an FFmpeg package extracted elsewhere:

```powershell
.\build-switch.ps1 -FfmpegRoot "C:\path\to\switch-portlibs"
```

Linux or another POSIX shell:

```bash
./build-switch.sh
```

Or with a custom dependency root:

```bash
RUMBLENX_FFMPEG_ROOT=/path/to/switch-portlibs ./build-switch.sh
```

## Output

```text
build_switch/RumbleNX.nro
build_switch/RumbleNX.elf
```

Keep each NRO together with its matching ELF. Atmosphere crash addresses can
only be symbolicated accurately with the ELF from the exact same build.

## Update checks

The About page has a *Check for updates* button that asks GitHub for the
newest release of [j2k-lab/rumbleNX](https://github.com/j2k-lab/rumbleNX).
A fork can point it at its own repository, or hide the button with an empty
value:

```bash
RUMBLENX_RELEASE_REPOSITORY=owner/name ./build-switch.sh
RUMBLENX_RELEASE_REPOSITORY= ./build-switch.sh
```

With Docker, pass it into the container with
`-e RUMBLENX_RELEASE_REPOSITORY=owner/name`. CMake users can set the
`RUMBLENX_RELEASE_REPOSITORY` cache variable directly. An existing build
directory keeps the value it was configured with until you change it.

## Dependency warning

When no custom FFmpeg root is available, CMake uses the FFmpeg libraries
reported by the installed `mpv.pc`. The project may compile, but that build does
not include the TLS and NVTEGRA guards.

## Optional: patched MPV 0.36 recipe

`scripts/switch/mpv` holds an older MPV 0.36.0-7 recipe with an extra guard
for frame side data. It is not part of the validated stack; it is kept for
reference. To try it, in devkitPro MSYS2:

```bash
cd scripts/switch/mpv
rm -rf src pkg build
makepkg -Csf --noconfirm
pacman -U --noconfirm switch-libmpv-0.36.0-7-any.pkg.tar.zst
```

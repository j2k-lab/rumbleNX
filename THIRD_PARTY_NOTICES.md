# Third-party notices

This file summarizes the principal upstream projects and third-party libraries used by RumbleNX. Copyright headers and license files inside third-party source directories remain authoritative.

## kickNX

- Upstream: `j2k-lab/kickNX` (by j2k)
- Role: Direct project foundation. RumbleNX is a port of kickNX 0.5.0: the player, controller and touch navigation, portrait mode, chat composer, themes, build system and guarded FFmpeg/NVTEGRA recipe come from kickNX; its Kick-specific code was removed and replaced with Rumble support.
- License: GNU General Public License version 3 (GPL-3.0-only)

## twiNX

- Upstream: `DomazinUS/twiNX`
- Role: Direct project foundation; provided the initial Nintendo Switch client structure, MPV/Borealis video playback pipeline, and controller-first navigation.
- License: GNU General Public License version 3 (GPL-3.0-only)

kickNX is a direct fork of twiNX, adapted for Kick streaming and modern Switch homebrew.

## Switchfin

- Upstream: `dragonflylee/switchfin`
- Role: Original Nintendo Switch platform foundation, Borealis UI architecture, and MPV wrapper base inherited through twiNX.
- License: Apache License 2.0

A copy of Apache-2.0 is stored at `LICENSES/Apache-2.0.txt`. Existing upstream copyright and license headers have been preserved.

## Borealis

- Upstream: `dragonflylee/borealis`
- Vendored commit: `7f03684805befffb1be18a1b2bdb81fa66945af2`
- Role: Controller-first, hardware-accelerated user-interface framework (NanoVG / OpenGL).
- License: Apache License 2.0

The upstream license and NOTICE remain in `library/borealis/`; the NOTICE is also preserved at `LICENSES/NOTICE-Borealis.txt`.

## LunaSVG

- Upstream: `sammycage/lunasvg`
- Vendored commit: `f924651b85cac47dbe15f51a4aa320461fc1d07b`
- Role: SVG rendering for icons and UI elements.
- License: MIT License

The upstream license remains in `library/lunasvg/LICENSE` and is copied to `LICENSES/MIT-LunaSVG.txt`.

## mpv

- Role: Media playback engine powering video streaming, live caching, and audio decoding.
- Upstream license: GPL version 2 or later; LGPL builds are possible only when configured without GPL-only files.

The Nintendo Switch build links against devkitPro's `switch-libmpv`. The combined RumbleNX distribution is provided under GPL-3.0-only.

## FFmpeg

- Recipe: FFmpeg 7.1 with Nintendo Switch patches (`switch-ffmpeg`)
- Role: Video/audio demuxing, decoding, color conversion, and hardware NVTEGRA decoding.
- Build configuration: `--enable-gpl`

The reproducible package recipe and hardware decoding patch scripts are maintained in `scripts/switch/ffmpeg-libnx/`. GPL-2.0 and GPL-3.0 license texts are included under `LICENSES/`.

## Noto Emoji

- Upstream: `googlefonts/noto-emoji` (monochrome Noto Emoji)
- Role: `resources/font/emoji.ttf`, inherited from kickNX, renders emoji in chat and the emoji picker.
- License: SIL Open Font License 1.1

## Rumble service documentation

RumbleNX contains no code from these projects, but its Rumble support follows the public behaviour they document:

- `yt-dlp/yt-dlp` (Unlicense) - Rumble embed and playback endpoints
- cocorum (Python library for Rumble) - Rumble chat stream, chat messages and `service.php` sign-in, including Rumble's salted password hashing

## Other dependencies

RumbleNX also utilizes libcurl, libnx, devkitPro libraries, WebP, nlohmann/json, and additional components inherited from twiNX, Switchfin, and Borealis. Their source directories, package metadata, and upstream license notices remain applicable:
- **Material Design Icons:** `resources/material/LICENSE.txt` (Apache-2.0)
- **libcurl:** MIT/X derivate license
- **libnx:** ISC License

## Combined-project license

The root `LICENSE` contains the GNU General Public License version 3 (GPL-3.0-only). This licensing choice covers the combined RumbleNX distribution, the kickNX code it is based on and original RumbleNX code while preserving the more permissive licenses of inherited Apache-2.0 and MIT components.

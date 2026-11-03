# Video encoder backends

## FFmpeg software backend

`VideoEncoderBackend::Software` remains the reference path. It converts CPU
BGRA8, NV12 or YUV420P input into the encoder's YUV420P frame format and uses
the FFmpeg software encoder (libx264 for H.264 when available).

## NVIDIA NVENC backend

`VideoEncoderBackend::NVENC` selects FFmpeg's named hardware encoders:

- H.264: `h264_nvenc`
- HEVC: `hevc_nvenc`
- AV1: `av1_nvenc`

There is deliberately no silent fallback to a software encoder. If the
requested NVENC encoder is absent from the FFmpeg build, initialization fails
with an actionable error. The driver, GPU generation, codec support and
FFmpeg build must all support the requested combination.

The initial adapter configures the low-latency `p4` preset and CBR or VBR rate
control. It currently accepts CPU-backed frames and uses FFmpeg's NVENC
integration to perform hardware compression. It does **not** yet import the
retained D3D11 capture texture directly; the CPU-to-GPU upload remains in the
path. Native-only D3D11 frames are therefore rejected by this adapter's
current input path.

## Not yet implemented

- Direct D3D11 texture import and synchronization for NVENC. FFmpeg must expose
  a D3D11VA hardware-frame configuration for the selected NVENC encoder; the
  existing BGRA capture texture also needs a GPU-side conversion to an NVENC-
  compatible format such as NV12 before it can be submitted.
- Runtime GPU/driver capability enumeration and automatic backend selection.
- AMD AMF and Intel oneVPL/QSV adapters.
- Hardware-specific quality, latency and sustained-load validation.

The FFmpeg adapter exposes `FFmpegVideoEncoder::queryCapabilities()` so the
application can list encoder implementations compiled into the linked FFmpeg
build. Each entry also reports whether that encoder advertises D3D11VA or CUDA
hardware-frame input through FFmpeg's `AVCodecHWConfig` interface. This is still
a build-time inventory only: it does not probe a physical GPU, driver state,
matching D3D11 device identity, pixel-format compatibility or runtime codec
support. A configured hardware backend is not
proof that a compatible GPU is present. Applications should treat encoder
initialization as the definitive availability check and preserve the software
backend as an explicit, user-visible fallback.

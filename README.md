# VLC 3 Chromecast smooth fix

VLC 3.0 often stutters on a Chromecast, and after a seek it recovers slowly.
This plugin for VLC 3.0 on Windows (64-bit) fixes the causes in VLC. It adds
one file, `libchromecast_smooth_fix_plugin.dll`, and replaces no VLC file.

**Set the conversion quality to High.** See
[Recommended setting](#recommended-setting).

## Issues

Fixed by the plugin (causes found in VLC 3.0.24):

| Issue | Cause | Fix |
|---|---|---|
| Stutter in compatible H.264 MKV files, which VLC sends unchanged | The Matroska reader gives B-pyramid frames decode times that go back. The muxer rejects them: 369 of 802 frames lost in a 30 s test. | Correct decode times before the muxer. 0 frames lost. |
| Stutter and slow recovery after a seek in converted files | x264 encodes on the CPU at approximately real-time speed. | NVENC encodes on an NVIDIA GPU at approximately 5x real time. A restart after a seek takes approximately 30 ms. |
| Slow conversion, mostly of 4K and 10-bit video | In a conversion, a GPU-decoded frame comes back to the CPU and goes through more conversion passes. | Software decoding for all conversions: up to 4x faster. Normal playback keeps GPU decoding. |
| 5.1 sound becomes stereo after a seek | After each seek, the Chromecast module forces MP3, which has 2 channels only. | Keeps Vorbis 5.1. |
| Video wider than 16:9 is squeezed, for example 2.35:1 shows as 1.34:1 | The width and the height are scaled separately to 1920x1080, and the pixel aspect is inverted. | One scale for both directions (3840x1632 becomes 1920x816), with any encoder. |

Not fixed (limits of VLC or of the Chromecast):

- A seek starts a new stream, so the TV shows "buffering" for a short time.
- The output is at most 1080p and 30 fps. VLC converts faster video to
  24 fps.
- VLC sends no subtitles to a Chromecast. The message `cannot continue
  streaming due to errors with codec subt` is about this only.
- If the device name ends in "(Chromecast)", VLC treats it as a
  first-generation Chromecast and also converts HEVC video.
- Without an NVIDIA GPU, x264 encodes. With High, it can be slower than
  real time, for example for 1080p video with film-like noise. All other
  fixes still work.
- The messages `Failed to create video converter` and `Too high level of
  recursion` come from VLC format tests and have no effect.
- If **Playback > Renderer** stays on "Scanning", see
  [vlc-mdns-windows-fix](https://github.com/omahasdev/vlc-mdns-windows-fix).

## Recommended setting

1. Open **Tools > Preferences** and select **All** at the bottom left.
2. Go to **Stream output > Sout stream > Chromecast**.
3. Set **Conversion quality** to **High**.
4. Click **Save**.

| Conversion quality | Video size | Audio with more than 2 channels |
|---|---|---|
| High | Up to 1080p, best quality | Vorbis 5.1 |
| Medium | Up to 1080p | Vorbis 5.1 |
| Low, Low CPU | Up to 720p | MP3 stereo |

With NVENC, High is fast enough. With Low and Low CPU, the plugin keeps
MP3, because these settings ask for it.

## Install

Requirements: Windows 10 or 11 x64 and VLC 3.0.x 64-bit. For NVENC: an
NVIDIA GPU with driver 531 or newer.

**Prebuilt DLL.** The [Releases](../../releases) have a DLL for each
supported VLC version, with the install steps. To verify that this
repository built the file, run
`gh attestation verify libchromecast_smooth_fix_plugin.dll --repo omahasdev/vlc-chromecast-smooth-fix`.

**Build and install.** This also needs Git for Windows and Internet for
the first build.

1. Close VLC.
2. Run `install.cmd`.
3. Accept the UAC prompt. The window shows the result, and `install.log` in
   the work folder keeps it.

The script downloads and verifies the
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) toolchain, the VLC SDK
and the NVENC API headers. It builds the DLL, copies it to `plugins\codec`,
refreshes the VLC plugin cache, and deletes `libnvenc_plugin.dll`, the
first version of this plugin. The same commit gives the same SHA-256 as the
release.

`prepare.cmd` only downloads the build inputs and needs no administrator
rights. To keep the build files outside the repository, copy
[`examples/vlc-chromecast-smooth-fix.cmd`](examples/vlc-chromecast-smooth-fix.cmd)
and change its paths.

**Update and remove.** After a VLC 3.0.x update, run `install.cmd` again.
To remove the plugin, run `uninstall.cmd`.

## Check that it works

1. Cast a file with **Playback > Renderer**.
2. Open **Tools > Messages** and set the verbosity to 2.
3. Look for these lines:
   - `video DTS correction in front of avformat`: in each cast
   - `software decoding for conversions`: when VLC converts
   - `NVENC H.264`: when VLC converts the video
   - `Chromecast audio kept as Vorbis`: after a seek, when VLC converts the
     audio
   - `uniform scaling to width`: when VLC makes video wider than 16:9
     smaller

If you see `could not write frame`, VLC still drops frames.

## How it works

VLC sends media to a Chromecast through its **stream output** ("sout").
For each file, the Chromecast module builds this chain:

```
transcode{...}  ->  chromecast-proxy  ->  std{mux=avformat{mux=matroska}, access=chromecast-http}
```

`transcode` converts only what the Chromecast cannot play, with the encoder
settings, the maximum size and the audio codec of the conversion quality.
`avformat` writes Matroska, which the TV reads over HTTP.

The plugin answers to the names of three VLC modules with a higher priority.
Where a part does not apply, it refuses, and VLC uses its own module.

| Name | Applies to | Action |
|---|---|---|
| `transcode` | All conversions | Selects software decoding |
| `transcode` | Chains that end in `chromecast-proxy` | Changes forced MP3 to Vorbis and adds `width=W` for one scale, then opens the real `transcode` at the video stream, when the source size is known |
| `x264` | H.264 encoding, when NVENC is available | Encodes with NVENC: High profile, no B-frames, a keyframe every 2 s, constant quality with a peak rate |
| `avformat` | Matroska and WebM output | Keeps 4 video frames and gives each frame the next display time, moved back by 4 frame durations, as its decode time |

The software decoding, the encoder and the muxer also apply to **Media >
Convert/Save**.

## Speed

A conversion has two steps. VLC decodes the source, then resizes the
frames on the CPU and encodes them to H.264. Each step can run on the CPU or
on the GPU:

- Decode on the GPU (D3D11VA) or on the CPU (software)
- Encode on the CPU (x264) or on the GPU (NVENC)

Test with an RTX 4070 and an i5-13500: a 20 s clip converted to 1080p H.264
and Vorbis with the Chromecast settings. Each time is the shorter of 2 runs
and includes the VLC start. Less than 20 s is faster than real time.

| Decode on | Encode with | 1080p HEVC, film-like noise | 4K HEVC 10-bit | Used by |
|---|---|---|---|---|
| GPU | x264 (CPU), High | 22.4 s | 31.5 s | VLC without the plugin |
| GPU | x264 (CPU), Low CPU | 10.6 s | 26.1 s | VLC without the plugin |
| CPU | x264 (CPU), High | 20.7 s | 16.2 s | The plugin, without an NVIDIA GPU |
| CPU | x264 (CPU), Low CPU | 9.1 s | 7.8 s | The plugin, without an NVIDIA GPU |
| GPU | NVENC (GPU), High | 5.0 s | 24.6 s | The plugin, with software decoding off |
| CPU | NVENC (GPU), High | **4.0 s** | **6.0 s** | The plugin, with an NVIDIA GPU |

## Options

The options are in **Tools > Preferences > All > Input / Codecs > Video
codecs > Chromecast smooth fix**. The defaults are correct for most uses.

| Option | On (default) | Off |
|---|---|---|
| Use NVENC for x264 requests | NVENC encodes H.264 | x264 encodes on the CPU |
| Software decoding for conversions | Conversions decode on the CPU | Conversions use the GPU decoder |
| Correct video DTS for Matroska output | No frames lost in remuxed MKV | VLC drops frames |
| Keep Vorbis audio for Chromecast after a seek | 5.1 stays Vorbis 5.1 | MP3 stereo after a seek |
| Keep the aspect ratio of converted Chromecast video | Wide video keeps its shape | Wide video is squeezed |

The NVENC values below apply when a request does not set them. The
Chromecast module sets its own preset and CRF, so for Chromecast output
only the peak bit rate has an effect.

| Option | Default | Use |
|---|---|---|
| Peak bit rate (kbit/s) | 0 | 0 selects 12000 above 1280x720, and 6000 for 1280x720 and less |
| Quality (CRF-like) | 23 | 0 (best) to 51 |
| Preset | p5 | x264 preset name or NVENC p1 to p7 |

## License

LGPL 2.1 or later. The build downloads the NVENC API headers from FFmpeg
[nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) (MIT).

# A/B kit: about:config switches to compare on a Lumia

The port ships one configuration, but most of what it does at feature level 9_3
(Lumia 550, 650, 950) and on the HP Elite x3 can be switched off or changed in
`about:config`. This page lists those switches, what each one isolates, and the
note line or tool that shows the result, so a comparison on a phone can be run
the same way each time.

## How to run a comparison

1. Change **one** switch at a time. Two at once tell you nothing about either.
2. When the table says **restart**, close the app completely (swipe it away in
   the task switcher) and start it again. **Live** switches act on the next page
   load or the next video, as the row says.
3. Play the **same clip** and scroll the **same long page** in both runs. For
   video, a fixed 720p30 YouTube clip of at least three minutes, inline and
   fullscreen, works well.
4. Pull the results from the PC:
   - `python tools/phone.py logs` for the note log (`gecko-notes.log`), where
     every line named below appears;
   - `python tools/phone.py mem` for app memory and GPU memory.
5. YouTube's *Stats for nerds* shows the codec (`avc1` or `vp09`/`av01`) and the
   dropped frames.
6. For the 9_3 video flicker, count the flashes of an old frame during one
   minute of the same clip.
7. **Reset** in `about:config` restores the shipped value.

Every note line is capped per run, so a line that already appeared earlier in
the run may not appear again. Start a fresh run when a row depends on a
once-per-run line.

## Rendering (feature level 9_3)

| Switch | Shipped | Try | Restart / live | What it isolates | Where to read it |
|---|---|---|---|---|---|
| `gfx.webrender.software.d3d11.gpu-shading` | `true` (build-appx.sh) | `false` | restart | SWGL shading pixels on the GPU into render-target tiles, against SWGL shading on the CPU with the D3D11 compositor. | `gfx: SWGL shades pixels on the GPU ...` at startup; `gfx: GPU shading -- N GPU draws, M CPU draws ...` every 300 frames; GPU memory in `phone.py mem`. |
| `gfx.webrender.software.d3d11.gpu-shading.warmup` | `2` | `0`, `1` | restart | The idle probe that creates the not-yet-used 9_3 pixel shaders before a frame needs them. `0` off; `1` probe, record refusals, release what was accepted; `2` probe and keep. | `gfx: GPU programs -- A accepted / R refused of P probed ...`; `gfx: pixel shaders made during frames -- ...` (frequent with `0`, quiet with `2`); `gfx: GPU programs refused: ...`. |
| `gfx.webrender.software.d3d11.gpu-shading.warmup-delay-ms` | `4000` | `1000`, `10000` | restart | How long after the first GPU-shaded frame the probe starts looking for idle time. | Timestamp of `gfx: GPU program probe starting -- ...` against the first frame; `... s after the first GPU frame` at the end of the `gfx: GPU programs` line. |
| `gfx.webrender.software.d3d11.upload-mode` | `4` | `1`-`3` | live (every frame) | How CPU-drawn tiles reach the GPU. **Only with `gpu-shading` = `false`**; while GPU shading is on, the mode stays at staging and this pref is ignored. | Scrolling smoothness; `phone.py mem`. |
| `gfx.webrender.software.d3d11` | `true` (build-appx.sh) | `false` | restart | The D3D11 compositor against SWGL's own CPU composite. Expected to be slow; a baseline only. | Scrolling smoothness; the `gfx:` startup lines. |
| `gfx.webrender.gecko-w10m.tile-partial-update` | `true` | `false` | restart | Picture-cache tiles redrawn only where they changed, against a changed tile redrawn whole. A test for specks on Qualcomm's driver. | Visible specks while scrolling. |
| `gfx.webrender.texture-cache.small` | `true` (build-appx.sh) | `false` | restart | WebRender's small texture cache. | GPU memory in `phone.py mem`. |

Partial present is deliberately not in this kit: `gfx.webrender.max-partial-present-rects`
stays as it is. `gfx.webrender.gecko-w10m.direct-back-buffer` and
`gfx.webrender.gecko-w10m.video-overlay` act only at feature level 10 and above,
so they have no effect on a Lumia. The SWGL environment variables
(`SWGL_GPU_TRACE`, `SWGL_GPU_CPU`, `SWGL_GPU_DEBUG`) are for developers, not
for about:config.

## Video: decode and display

| Switch | Shipped | Try | Restart / live | What it isolates | Where to read it |
|---|---|---|---|---|---|
| `media.wmf.gecko-w10m.frame-sync` | `1` | `2`, then `0` | live (next frame) | At 9_3, when decoding runs on the compositor's device: `1` waits after each video-processor conversion (as before); `2` also waits for the decode to finish before converting, the candidate fix for old frames; `0` waits for nothing (diagnostic only, expect the old stale frames back). | `media: frame sync N -- ...` once; in mode 2, `media: WxH, 300 frames; waiting for the decode before converting took ...`; `video: N composites drew the video; ... older conversion ..., ... rewritten ...`; flashes per minute. |
| `gfx.webrender.gecko-w10m.video-whole-window` | `false` | `true` | live | `false` redraws WebRender's dirty area plus this and the previous video frame's rectangle, so partial redraw stays on during video. `true` composites the whole window after any frame that drew a video (the behaviour of build 170). | `gfx: video frames redraw the dirty area and the video, not the whole window` once; `; composited X of the screen a frame` on the 300-frame `gfx: compositor tiles` line (about 0.3-0.5 inline, 1.0 with `true`); `gfx: WebRender's dirty rect left out a video surface ...` must not appear. |
| `gfx.direct3d11.reuse-decoder-device` | `true` (build-appx.sh) | `false` | restart | Decoders sharing the compositor's D3D11 device, against a device of their own. The key flicker comparison. | `media: WxH, 300 frames converted; waiting for the GPU to finish ...` appears only with `true`; flashes per minute. |
| `media.wmf.low-latency.enabled` | `true` | `false` | live (next video) | The H.264 decoder's low-latency mode, which may put frames out in the wrong order. Try it when the counters below are above 0. | `media: N of the last 300 decoded frames came out earlier than the frame before them (low latency ...)`; `media: decoder output went back in time ...`. |
| `media.wmf.gecko-w10m.reorder-guard` | `true` | `false` | live (next frame) | Dropping a decoded frame that goes back in time, and decoding later videos without low-latency mode after three such frames. | `media: decoder output went back in time (... ms after ... ms) -- frame dropped ...`; `media: the H.264 decoder puts frames out of order in low-latency mode -- later videos decode without it`. |
| `media.wmf.gecko-w10m.output-error-retries` | `0` | `1`, `2` | live | Retrying an unexpected decoder output failure instead of handing the error on. Raise it only if `media: decoder error in ...` lines show transient errors. | `media: H.264 output failed (...); retrying, ...`; `media: H.264 output failed N times in a row (...) -- handing the error on`. |
| `media.wmf.gecko-w10m.capabilities-from-probe` | `true` | `false` | live (next query) | H.264 `mediaCapabilities.decodingInfo` answered from the once-per-run DXVA probe, against a decoder built and thrown away for each query. | `media: H.264 capability queries answered from the DXVA probe, no decoder built ...`; with `false`, decoder init lines tagged `[capability query]` on every watch page. |
| `media.wmf.gecko-w10m.h264-query-size-cap` | `true` | `false` | live (next query) | Page queries (isTypeSupported, canPlayType, decodingInfo) answering no for H.264 at or above the smallest size the GPU's decoder refused. Decoder creation is never affected. | `media: DXVA H.264 on this GPU ...: ... -> refused from WxH`; `media: H.264 WxH reported unsupported to the page -- ...`. With this and `capabilities-from-probe` both `false`, the probe does not run at all. |
| `media.hardware-video-decoding.enabled` | `true` (build-appx.sh) | `false` | restart | DXVA against Microsoft's software H.264 decoder. With the Media Source gate the codec stays H.264, so only the decoder changes. | `gfx: hardware video decoding is ...` at startup; CPU and dropped frames. |
| `media.video-queue.hw-accel-size` | `6` on 1.5 GB or less (low-memory defaults) | `3`, `10` | live (next video) | How many hardware-decoded frames are queued ahead of the compositor. | Dropped frames in Stats for nerds; `phone.py mem`. |
| `media.wmf.dxva.max-videos` | `1` (build-appx.sh) | -- | live | One DXVA video at a time. **Do not raise it on the Elite x3**: a second DXVA session overflowed the Adreno driver's stack. | -- |

## Codecs: Media Source steering

| Switch | Shipped | Try | Restart / live | What it isolates | Where to read it |
|---|---|---|---|---|---|
| `media.mediasource.vp9.enabled` | `false` (build-appx.sh) | `true` | live (reload the page) | Media Source turning VP9 down on every site, so the site plays its H.264 ladder through DXVA. Plain `<video src>` and canPlayType keep VP9. | `media: MSE turns down vp9 -- ...` once per run; Stats for nerds (`avc1` against `vp09`); CPU and `phone.py mem`. |
| `media.mediasource.av1.enabled` | `false` (build-appx.sh) | `true` | live (reload the page) | The same for AV1. Do not leave `vp9` = `false` with `av1` = `true` beyond a test: sites that offer AV1 would move to the heavier codec. | `media: MSE turns down av1 -- ...` once per run. |
| `media.prefer-non-ffvpx` | `false` | `true` | live (next video) | ffvpx (plain C in this build) against libvpx (NEON intrinsics) for VP8 and VP9. Opus and Vorbis move to libopus and libvorbis as well. Test with a direct VP9 `.webm`, or with both Media Source prefs `true` and `gecko.h264ify.hosts` empty. | Dropped frames and CPU against `false`. |
| `gecko.h264ify.hosts` | `youtube.com,youtube-nocookie.com` (build-appx.sh) | empty | restart | The YouTube page hook, which answers before the engine does and carries the 720p30 ceiling and the fullscreen gear fix. Empty leaves YouTube to the engine gate alone. | `gecko: page hooks injected into ...` in the log; with it empty, Stats for nerds should still show `avc1`, with no 720p30 ceiling. |
| `gecko.video.max-height`, `gecko.video.max-framerate` | `0` (auto: 720 and 30 on 1.5 GB or less) | e.g. `480`, `1080`; `60` | restart | The video ceiling the YouTube hook applies. | `gecko: page hooks injected into ... (video up to ...)`; Stats for nerds. |

No steering at all: both Media Source prefs `true` **and** `gecko.h264ify.hosts`
empty. With `privacy.resistFingerprinting` on, the engine gate stays quiet by
upstream design and only the YouTube hook steers.

## Startup and shaders (HP Elite x3, hardware WebRender)

| Switch | Shipped | Try | Restart / live | What it isolates | Where to read it |
|---|---|---|---|---|---|
| `gfx.webrender.program-binary-disk` | `true` | `false` | restart | The WebRender program-binary cache in the profile's `shader-cache`. The master switch for the cache comparison. | `gfx: webrender shader cache is on` / `... off -- <reason>`; `gfx: shader cache at <dir> -- N of M startup programs read in X ms ...`; frame 2 time (`frame 2: begin` to `rendered`). |
| `gfx.webrender.gecko-w10m.shader-warmup-early` | `true` | `false` | restart | The shader warmup moved into the render thread's idle time before the compositor exists, against upstream timing (after the renderer is made). Compare three warm launches each way. | `gfx: shader warmup done -- N steps, X ms on the render thread, started early, before the compositor` (or `with the renderer`); frame 2 time; main-thread phase lines. Cold launches skip warmup. |

On the Lumias the shader cache line reads `off` with WebRender's reason; these
two rows matter only on the Elite x3.

## WebGL (feature level 9_3)

| Switch | Shipped | Try | Restart / live | What it isolates | Where to read it |
|---|---|---|---|---|---|
| `webgl.gecko-w10m.feature-level-9` | `true` | `false` | restart | WebGL 1 on ANGLE's own 9_3 device, against no WebGL at all at 9_3 (getContext fails in the page, nothing is loaded). Compare page-load time on sites that only probe WebGL. | `webgl: feature level 9_3 -- ...` once at startup; `webgl: EGL display #N up in X ms ...` per display; `webgl: last 600 frames read back, ...`. |
| `webgl.forbid-software` | `true` | `false` | live (next WebGL context) | Whether ANGLE may fall back to WARP when the hardware display fails. Also answers whether the phone ships WARP at all. | `webgl: EGL display #N failed in X ms (...), WARP tried / skipped / failed`. |

## Memory

| Switch | Shipped | Try | Restart / live | What it isolates | Where to read it |
|---|---|---|---|---|---|
| `gecko.lowmem.enabled` | `true` (acts on phones with 1.5 GB or less) | `false` | restart | The low-memory defaults the autoconfig sets on small phones (JavaScript heap, caches, video queue, painting). | `gecko: low-memory defaults for a N MB phone -- ... set` at startup; `phone.py mem`; the one-minute address-space note. |

The app's ceiling on a phone with a gigabyte is 390 MB. Compare `phone.py mem`
at the same point of the same page in both runs, not across pages.

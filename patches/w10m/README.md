# The engine port as patches

These files are the commits of the branch `w10m-port` of
<https://github.com/Computershik73/firefox>, one per commit, over the tag named in
`engine/release.txt`. `tools/apply-patches-w10m.sh` (or `.ps1`) replays them with
`git am`; `tools/export-patches-w10m.sh` regenerates them from the branch and checks
that they rebuild it exactly. Do not edit them by hand -- commit to the branch and
export.

The first thirteen are the port as of 155.16.0.86, one subsystem each:

| Patch | What it touches |
|---|---|
| 0001 Build system | `--enable-uwp`, the ARM32 Windows toolchain, what is built for it |
| 0002 Notes | the engine's line to the shell's log |
| 0003 Platform | mfbt, mozglue, NSPR, XPCOM and IPC in an app container |
| 0004 xptcall | XPCOM's calling glue for the ARM32 Windows ABI |
| 0005 SpiderMonkey | JIT code through the `*FromApp` allocators |
| 0006 Startup | XRE started by the shell, one process, the profile in LocalState |
| 0007 Widget | the headless widget as the shell's view |
| 0008 Graphics | D3D11, ANGLE on a SwapChainPanel, WebRender |
| 0009 Media | Media Foundation and DXVA, codecs' ARM32 Windows builds |
| 0010 libvpx/ffvpx configuration | **generated** |
| 0011 WebRTC | nrappkit and two libwebrtc sources |
| 0012 WebRTC NEON | **generated** |
| 0013 windows-sys | **generated** |

The rest are the work since, in order.

## Moving to a newer Firefox

Apply the hand-written patches with `git am -3` and resolve what conflicts. The three
generated ones are better made again than rebased -- they are long, mechanical and
tied to the exact upstream files:

| Patch | Made by |
|---|---|
| 0010 | `tools/gen-libvpx-win-arm-rtcd.sh`, `tools/gen-ffvpx-win32-arm-config.py` |
| 0012 | `tools/patch-libwebrtc-win-arm32-neon.py` |
| 0013 | `tools/patch-windows-rs-arm32.py` |

Every file keeps the line endings it has upstream (LF), so the patches show only
real changes.

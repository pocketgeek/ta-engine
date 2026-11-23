# Streaming to YouTube

TAK Engine can broadcast the game straight to YouTube, with no extra software.

![The YouTube Streaming panel: stream key, Remember Key, resolution, frame rate, bitrate, encoder, Save a Local Copy and Start Streaming](img/streaming.jpg)

## Quick start

1. Open the panel: press **F9** (rebindable in Settings → Controls) at the title screen or in a game, or choose
   **Settings → YouTube Streaming** at the title screen or **Esc → YouTube
   Streaming** during a game.
2. Copy the stream key from YouTube Studio and click **Paste** beside the key field
   (Ctrl+V also pastes a key; Ctrl+A clears the field).
3. Leave the video settings at their defaults or change them (below).
4. Click **Start Streaming**.
5. Watch the preview and status in YouTube Studio and make the broadcast public
   there. See [YouTube's encoder setup guide](https://support.google.com/youtube/answer/2907883).

**Closing the panel does not stop the stream.** **Back**, Esc or F9 closes it, and a small badge
in the corner shows the stream state while it runs. Returning to the title screen
keeps it running too. Stop it with **Stop Streaming**, or by exiting the application.

The key is masked on screen and is never written to the settings file or any other
file, and it is left out of encoder and network diagnostics. It stays in memory for
that session only, unless you tick [Remember Key](#remembering-the-key). The video
settings are remembered between sessions (see below).

## Video settings

Click a row to step to the next value; right-click steps back. The settings cannot
be changed while a stream runs.

| Setting | Choices | Default |
| --- | --- | --- |
| Resolution | 720p, 1080p, 1440p, 4K (3840×2160), **FULL** (the whole window), **MAX 3840** | MAX 3840 |
| Frame rate | 30 or 60 FPS | 60 |
| Bitrate | 3,000, 6,000, 9,000, 12,000, 20,000, 30,000, 45,000, 60,000 or 80,000 Kbps, up to [YouTube's maximum](#youtubes-bitrate) | 6,000 |
| Encoder | Automatic or CPU | Automatic |

- **MAX 3840** scales down to at most 3840 pixels wide while keeping the window's
  shape, without adding a 16:9 border or enlarging a smaller window. A 7680×2160
  window sends 3840×1080. The panel shows it as `MAX 3840 / W X H`.
- **FULL** uses the actual window size when you press Start Streaming, including
  high-DPI scaling. It can exceed what YouTube accepts even when your GPU can
  encode it; use MAX 3840 or 4K if YouTube reports an unsupported resolution.
- Dimensions are rounded down to even pixels and fixed when the stream starts.
  Stop and restart to change them; resizing the window meanwhile letterboxes the
  picture. The supported range is 2–8192 pixels per axis, subject to the encoder's
  limits.
- Choosing a resolution raises a lower bitrate to at least 6,000 Kbps, 12,000 Kbps
  for 1440p or 30,000 Kbps for 4K, but never past [YouTube's maximum](#youtubes-bitrate);
  you can still change it afterwards.
- The game remembers the panel's **resolution, frame rate, bitrate and encoder**
  (the `streamResolution`, `streamFps`, `streamBitrate` and `streamCpuEncoder` lines
  in the settings file) and saves them whenever you change one, so the panel opens
  with them next time. The bitrate saved is the one on screen, including a value
  that choosing a resolution raised. A value that is not one of the panel's choices
  (or one of YouTube's figures below) falls back to the default (MAX 3840, 60, 6,000 Kbps, AUTOMATIC). **Defaults** in
  Options leaves all four alone, like Save a Local Copy.

### YouTube's bitrate

Above YouTube's bitrate for the stream's size, frame rate and codec, YouTube can
accept the connection and then never start the broadcast, while the panel shows
**LIVE**. So the panel never goes above YouTube's figure: **BITRATE** steps stop at
it, and the row reads `- YOUTUBE MAX` there. A remembered bitrate above it, or one
left above it by a change of resolution, frame rate or encoder (or by resizing the
window under FULL or MAX 3840), is lowered to it, and the bottom line says so:
`BITRATE LOWERED TO YOUTUBE'S 17000 KBPS MAXIMUM`. **Recommended**, beside the
bitrate, fills that figure in.

The figures are YouTube's recommended bitrates from its
[live encoder settings](https://support.google.com/youtube/answer/2853702) page, in
Kbps. YouTube gives each a minimum and a recommended value and no maximum, so the
recommended value, the top of its range, is the limit:

| Stream | H.264, 30 FPS | H.264, 60 FPS | HEVC, 30 FPS | HEVC, 60 FPS |
| --- | --- | --- | --- | --- |
| 2160p (4K) | 42,000 | 50,000 | 30,000 | 35,000 |
| 1440p | 21,000 | 34,000 | 15,000 | 24,000 |
| 1080p | 14,000 | 17,000 | 10,000 | 12,000 |
| 720p | 8,000 | 8,000 | 6,000 | 6,000 |
| 480p and 360p | 4,000 | 4,000 | 3,000 | 3,000 |

- The row is the first one at or above the picture's **short side**, its height for
  a landscape window; the 2160p row above that, the 360p row below. An ultrawide
  stream goes by its height: MAX 3840 from a 7680×2160 window sends 3840×1080, a
  1080p stream (17,000 Kbps at 60 FPS in H.264). A 1600×900 window is 1080p.
- The codec is the one the encoder will use: **Automatic** encodes HEVC above 4096
  pixels on either axis and H.264 otherwise; **CPU** is always H.264.

### Encoders

**Automatic** tries the available hardware H.264 encoders first. Above 4096 pixels
on either axis it then tries hardware HEVC, to keep the native size. If no hardware
encoder supports that size it reports a failure rather than silently overloading
the CPU; choose **CPU** explicitly or a smaller resolution. At smaller sizes it falls
back to the bundled x264 CPU encoder, and the status line says so.

| Platform | Hardware H.264 and HEVC encoders |
| --- | --- |
| Linux | NVIDIA NVENC; Intel/AMD VA-API |
| Windows x64 | NVIDIA NVENC; AMD AMF; Intel Quick Sync |
| macOS | Apple VideoToolbox |

Windows ARM64 builds include x264 CPU encoding. The pinned FFmpeg version does
not support NVENC on Windows ARM64, and Qualcomm hardware encoding is not
implemented. AMF and Quick Sync are compiled in but require a compatible native
driver; their availability on Windows ARM64 has not been validated.

Hardware encoding needs a working GPU driver that supports the chosen codec and
resolution. If the hardware fails during encoding, the stream restarts on the CPU,
up to 4096 pixels per axis; larger sizes report an error.

### Intel and AMD hardware encoding on Linux

On Linux, Intel and AMD encoding goes through the VA-API library installed on your
system (`libva.so.2` and `libva-drm.so.2`) and its driver; the game does not carry a
copy of libva. Without libva, Automatic uses NVENC or the CPU, as on a computer
without VA-API hardware. libva is not a package dependency, but most desktops already
have it.

If the status line shows **LIVE - CPU FALLBACK** on an Intel or AMD computer, check
VA-API outside the game with `vainfo` (Fedora: package `libva-utils`; Debian and
Ubuntu: `vainfo`). It must list `VAEntrypointEncSlice` or `VAEntrypointEncSliceLP`
for an H.264 profile such as `VAProfileH264Main` or `VAProfileH264High`. Entries with
only `VAEntrypointVLD` are decoding, and the game cannot encode with them.

| Hardware | Driver package |
| --- | --- |
| Intel Broadwell and newer | Fedora: `intel-media-driver` from RPM Fusion nonfree (it installs into `/usr/lib64/dri-nonfree`, where the system libva looks); Debian and Ubuntu: `intel-media-va-driver-non-free` |
| AMD | Fedora: `mesa-va-drivers-freeworld` from RPM Fusion free (Fedora's own `mesa-va-drivers` does not encode H.264 or HEVC); Debian and Ubuntu: `mesa-va-drivers` |

To see what libva does when the game opens the encoder, start the game from a
terminal with libva's own messages switched on:

```sh
LIBVA_MESSAGING_LEVEL=2 takclient
```

When you press Start Streaming the terminal shows a `TAK VA-API: using system
.../libva.so.2 and .../libva-drm.so.2` line naming the files it loaded (or a `TAK
VA-API: system libva unavailable: ...` line saying why not), then libva's
`libva info:` lines: its VA-API version, each driver file it tries, and whether that
driver started. `LIBVA_DRIVER_NAME` (for example `iHD`) and `LIBVA_DRIVERS_PATH`
work as they do for `vainfo`.

### The status lines

The panel shows the encoder actually opened, the connection state, encoded frames,
missed output frames (**DROPPED**), superseded or busy capture submissions
(**REPLACED**), and the bitrate actually being sent (**RATE**).

**UPLOAD CAN'T KEEP UP - LOWER THE BITRATE** appears on the bottom line, and in the
corner badge while the panel is closed, when the encoder keeps waiting for your
connection to take the video: in three of any five seconds it waited over half the
second, or DROPPED rose while it waited. It clears after five seconds without that.
RATE under the set bitrate is not a warning by itself: some hardware encoders send
much less on a still picture. Frames DROPPED while the connection keeps up mean a
busy computer instead; try a lower resolution or frame rate. A replaced capture does not by
itself mean an output frame was missed. Rendering slowdowns skip late frames
without forcing a reconnect, and with VSync on, a refresh-rate pacing fallback
keeps streaming from uncapping presentation.

## What the stream contains

The stream carries the game view, HUD, cursor and mixed game audio, including the
title and lobby music. A differently shaped game window is letterboxed.

### The cursor

With **Hardware Cursor** on (the default), the pointer stays a hardware cursor on
your screen while you stream. The operating system draws that cursor, so it is not
in the captured picture; the stream's encoding worker draws the same cursor back
into each frame instead: the same art and animation frame, the fight-move tint,
your **Cursor Size**, and the hotspot on the pointer. It is drawn after the frame
is resized to the stream size, so it keeps the share of the picture it has on
screen, and inside a letterboxed picture it never spills into the black bars. It is
clipped at the picture's edges, and it is left out while the pointer is outside the
game window. Where the picture is read back a frame or two late (OpenGL,
Direct3D 11), each frame keeps the cursor it was captured with, so the pointer
never runs ahead of the picture. This costs the game nothing per frame beyond
handing the worker the cursor picture; the worker blends a few hundred pixels.
While the setup panel is open, the stream shows the plain arrow the panel uses.

With Hardware Cursor off, the game draws its cursor into the frame, so the stream
captures it as part of the picture and nothing is added. In that mode the title
screen's cursor is drawn after the capture and is not in the stream.

The setup panel, title-screen account and settings overlays, and the connection
badges are drawn **after capture** and stay off the stream. During loading and
modal screens the last captured frame stays on the stream.

Microphone input, desktop capture, YouTube account login
and broadcast scheduling are not included.

## Saving a local copy

Tick **Save a Local Copy**, the box above **Start Streaming**, to keep a copy of the
broadcast on your own disk as well. It is off by default, and the game remembers
whether you ticked it (the `streamLocalCopy` line in the settings file). **Defaults**
in Options leaves it alone. The box is greyed out while a stream runs: the choice
applies when you next press **Start Streaming**.

The copy is the **same encoded video and audio** that goes to YouTube, written to a
second file as it is sent. Nothing is encoded twice, so a copy costs disk writes,
not extra CPU or GPU time, and it has the stream's resolution, frame rate, bitrate
and codec (H.264, or HEVC for very wide streams; AAC audio). Files go into a
**videos** folder inside the game's user folder, the folder that holds `settings.ini`
and your replays, and are named after the local time the connection opened:
`TAK-YYYYMMDD-HHMMSS.mp4`. The folder is created when the first copy starts.

| Platform | Videos folder |
| --- | --- |
| Windows | `%APPDATA%\TAKengine\TAKingdoms\videos\` |
| macOS | `~/Library/Application Support/TAKengine/TAKingdoms/videos/` |
| Linux | `~/.local/share/TAKengine/TAKingdoms/videos/` (or under `$XDG_DATA_HOME`) |

The file is a **fragmented MP4**: a fragment is written every two seconds, at each
keyframe, so if the game crashes or the computer loses power the file still plays up
to the last finished fragment. Stopping normally finishes the file. While saving,
the panel's status shows **SAVING LOCAL COPY** and the file name, and the corner
badge shows it while the panel is closed. After **Stop Streaming** it shows **LOCAL
COPY SAVED** and the file's full path.

The copy only records what is actually going out. Nothing is saved while the stream
is connecting or reconnecting, and every connection gets its own file: after a
reconnect, or a restart on the CPU encoder, a new file begins (a `-2` suffix is added
if two would share a name).

A problem with the copy never stops the stream. If the folder cannot be created or
written, the status shows **LOCAL COPY FAILED** and the stream carries on without
a copy. If writing fails part-way, for example because the disk is full, the status
shows **LOCAL COPY STOPPED - WRITE FAILED** and the stream carries on; the file
keeps everything up to its last finished fragment. If the game has no user folder at
all, **Start Streaming** asks you to untick the box instead.

## Remembering the key

**Remember Key**, the box under the key field, is off by default. Ticked, it keeps
the key in your operating system's password store and fills the key field, still
masked, the next time the game starts. Only the on/off choice is saved in the
settings file. The key itself never goes into settings or any other file, and
**Defaults** in Options leaves the choice alone.

While the box is ticked, the stored key follows the key field. It is saved when you
tick the box, paste a key, start streaming, or close the panel (quitting with the
panel open counts). Characters you type are saved at the next of those moments, not
one at a time. If the field is empty at one of those moments, the stored key is
deleted instead, so it is never left behind stale. **Ctrl+A** empties the field and
deletes the stored key at once. **Unticking deletes the stored key** immediately;
the key stays in the field until you exit. If the store refuses to delete it (for
example, you cancel its unlock prompt), the box ticks itself again and the bottom
line says so, because the key is still stored. A remembered key only fills an empty
field: a key you entered before it loaded is kept, and saved at the next of those
moments.

| Platform | Where the key is stored |
| --- | --- |
| Windows x64 and ARM64 | Credential Manager, under Windows Credentials: the generic credential `TAK Engine/YouTube stream key`, kept for your account on this computer |
| macOS | Your login keychain: a password item with service `TAK Engine` and account `YouTube stream key`, visible in Keychain Access |
| Linux | The Secret Service: KWallet, GNOME Keyring, KeePassXC or another provider of `org.freedesktop.secrets`. The item is labelled `TAK Engine: YouTube stream key`, with attributes `service` = `TAK Engine` and `account` = `YouTube stream key` |

On Linux the game loads libsecret (`libsecret-1.so.0`) when it first needs the store;
it is not a package dependency. If libsecret is missing, or nothing on the session
bus provides the Secret Service, the box is greyed out with **NO SYSTEM PASSWORD
STORE**. There is no fallback to a file. Install libsecret and enable a provider
(KWallet's Secret Service integration on KDE, GNOME Keyring, or KeePassXC with its
Secret Service integration switched on), then open the panel again. While no store
is available the box cannot be changed; a choice saved earlier waits for a session
where the store is back.

With the box unticked, nothing contacts the store until you open the panel, and then
only to check that one exists. That check never opens or unlocks a wallet. With the
box ticked, the store is checked and the key loaded in the background as the game
starts. Loading or saving can make the system ask you to unlock a locked wallet or
keychain; the game keeps running meanwhile, and a refused or failed request is shown
on the panel's bottom line. If the remembered key cannot be loaded, nothing is saved
or deleted until a later load succeeds: the panel tries again each time it opens.

Running the Windows build under **Proton or Wine** uses Wine's emulation of
Credential Manager. Wine keeps the entry in the prefix's `user.reg` file, encrypted
with a key stored beside it, so anyone who can read the prefix can recover the key.
That is less secure than native Windows or the Linux Secret Service.
On Linux, prefer the native build if you want to remember the key.

## Connection details

Uploads use RTMPS with certificate and hostname verification, H.264 or HEVC video, AAC
stereo audio at 44.1 kHz / 128 Kbps, and two-second video keyframes. After a
connection failure, the worker retries three times, waiting 2, 4 and 8 seconds.
A connection that stayed live for a minute starts a fresh set of three retries
when it drops, so drops far apart never end the stream. Stop interrupts network
I/O; an unsuccessful stream reports failure in the game. The primary YouTube
ingest endpoint is fixed in the client.

While retrying, the status line names the reason (for example
**RECONNECTING 1/3 - CONNECTION DROPPED**). After the last retry it shows one of:

| Status | Meaning |
|---|---|
| **FAILED - TLS CERTIFICATE NOT TRUSTED** | The secure connection was refused because the server's certificate did not verify (a network that intercepts HTTPS, or a wrong system clock). |
| **FAILED - CANNOT CONNECT - CHECK KEY, NETWORK** | The connection to YouTube could not be opened or set up for the stream. Check the key and the network. |
| **FAILED - CONNECTION DROPPED - CHECK NETWORK** | The stream went live, but the upload kept stalling or being closed. |
| **FAILED - CHECK KEY, CONNECTION OR ENCODER** | The encoder or the stream could not be set up. |

The reason never includes the key, and FFmpeg's own messages are not shown.

## Performance and dependencies

Encoding, scaling, audio resampling and networking run on a separate worker.
The video queue holds only the latest frame; the audio ring has a fixed size.
Slow uploads cannot grow an unlimited queue or block the mixer/simulation.

The renderer shrinks each captured frame toward the stream size on the GPU
before reading it back, so a 7680×2160 window streamed at MAX 3840 copies a
3840×1080 image, not the whole window:

| Renderer | Capture |
| --- | --- |
| OpenGL (what **Auto** picks on Linux and Windows) | GPU scaling, two pixel buffers and nonblocking fence checks |
| Direct3D 11 (Windows) | GPU half-size reduction when the stream is half the window or smaller, then a ring of staging textures read only once the GPU has finished |
| Direct3D 9 (Windows) | GPU scaling to the stream size, then a small readback that waits for the GPU |
| Metal, software | SDL's portable full-size readback, which waits for the GPU on the render thread |

The worker finishes any remaining resize with its own multithreaded scaler (an
area average when shrinking, bilinear when enlarging); colour conversion for
encoders other than NVENC runs after the resize, at the stream size. On the
7680×2160, RTX 5070 Windows test machine, a MAX 3840 / 60 FPS / 6,000 Kbps NVENC
stream encodes all 60 frames a second at about the set bitrate with the game
still at 240 FPS, under Direct3D 9, Direct3D 11 and OpenGL alike. If streaming
still reduces game performance, lower the resolution or frame rate, or choose
hardware encoding.

No FFmpeg executable or additional DLL/shared-library bundle is required by
players. FFmpeg, x264, OpenSSL and the Windows oneVPL dispatcher are built as static
archives. NVIDIA and AMD SDK headers add no runtime library bundle. On Linux, VA-API
uses the system libva, loaded when a stream starts. Installed GPU drivers and native
OS frameworks remain external, just as SDL's display/audio drivers do.

## Development checks

`tools/build-ffmpeg-bink.sh` builds both Bink playback and streaming support.
Its pinned dependencies are built by `tools/build-stream-deps.sh`. Linux needs
Python 3, Ninja, Perl's standard modules (Fedora: `perl-core`), and patch in
addition to the regular build tools. Perl configures OpenSSL during the build;
it is not a game dependency. Windows streaming uses Schannel; Linux and macOS
streaming use the same static OpenSSL as multiplayer TLS. FFmpeg's OpenSSL hostname
checking is patched explicitly for the pinned FFmpeg 7.1 source
(`tools/ffmpeg-tls-hostname.patch`, part of the FFmpeg cache keys in CI). OpenSSL does
not read the macOS keychain, so on macOS each connection attempt writes the system trust
anchors (the ones multiplayer TLS also imports) to a private temporary file, passes it to
FFmpeg as its CA file and deletes it once the attempt has connected or failed. On Linux
the first distribution CA bundle found is used. Certificate verification is never turned
off. macOS does not use Apple's deprecated SecureTransport, whose FFmpeg 7.1 code does not
pass non-blocking reads down to the socket: RTMP's periodic check for server messages
would block until the 5-second timeout and every RTMPS upload would drop after a few
frames.

CMake requires the media/codec/TLS/loader archives by full path; CI additionally
checks executable imports. On Linux the FFmpeg build compiles against pinned libva
headers, and the prefix's `libva.a` is `tools/vashim.c`: each libva function FFmpeg
calls forwards to the system `libva.so.2`/`libva-drm.so.2`, opened with `dlopen` on
first use. When they cannot be loaded every call fails with a VA error and
`vaGetDisplayDRM` returns no display, so FFmpeg moves on to the next encoder. Functions
newer than libva 2.0 are optional (`vaSyncBuffer` reports unimplemented, `vaMapBuffer2`
falls back to `vaMapBuffer`). `vashim_test` checks this: `vashim_unavailable` runs
against a fake `libva.so.2` and `libva-drm.so.2` with no functions and expects a clean failure and a CPU
recording; `vashim_system` and `vashim_system_loader` (skipped without libva or a DRM
render node) open the system libva, and the latter gives it a fake driver that exports
only `__vaDriverInit_1_23`, which only a libva 2.23 or newer loader finds. Linux CI also
fails if `takclient` contains a libva driver loader. `stream_test` tests H.264/AAC output, timestamps,
configuration validation, stopping, failure and restart without any network. It also
records with a local copy and reads the MP4 back box by box (the shipped FFmpeg has no
MP4 reader): fragmented, an H.264/HEVC video track and an AAC track, the expected
duration, and exactly as many video frames as the stream itself carried. A simulated
full disk must stop only the copy, leaving the frames already fragmented readable,
and an unwritable folder must fail only the copy. Built against an FFmpeg without the
MP4 muxer, that part reports itself and CTest shows the test as skipped; CI does not
allow the skip. Rebuild the FFmpeg prefix with `tools/build-ffmpeg-bink.sh` to get it.
`stream_capture_test` exercises capture with an open private setup panel; with
`cursor` as its fifth argument (`stream_capture_test gl "" 640 480 cursor`) it
checks that a cursor reaches the encoded picture at the right place and size through
that renderer's capture path, and is gone once cleared. `streamcursor_test` checks the
cursor stamping itself (position, size, letterbox, clipping and alpha) without a GPU,
and `stream_test libx264 cursor` checks it through the encoding worker.
`stream_key_test` drives Remember Key through the panel against a fake store, and
checks that the saved settings never contain the key. `credstore_test` makes a round
trip through the real password store under a test-only name (service
`TAK Engine Test`, account `credstore round-trip`) with dummy values, and is skipped
when the session has no usable store. All are registered with CTest. In a debug build,
`TAK_CREDSTORE=none` shows the greyed-out box, `TAK_CREDSTORE=memory` keeps the key in
process memory only, and `TAK_CREDSTORE=test` uses the real store under the test name. For a GPU capture check:

```sh
SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl \
  ./build-o2/stream_capture_test gl /tmp/capture.flv
./build-o2/stream_test h264_nvenc
```

`streaming_rtmps` (CTest, not on Windows; also run by macOS CI) uploads over RTMPS for
10 seconds to a local Python TLS proxy in front of `stream_network_test --sink`, a plain
RTMP receiver built from the same static FFmpeg. It needs only `python3` and `openssl`
(it is skipped without `openssl`), and fails unless the stream stays LIVE without a
reconnect and the receiver gets nearly all of it (megabytes of FLV with libx264), which is what catches a stall like the SecureTransport one above. The
proxy's certificate is issued by a throwaway root CA that the client is given as its CA
file. The check then offers a certificate from that root for another host name, and the
right host name with no test CA (the client keeps its real trust store, which does not
hold the throwaway root); both must be refused with the TLS CERTIFICATE reason. On macOS
`stream_network_test --system-roots` must also export the system trust anchors as a bundle
that parses back cleanly (anchors OpenSSL cannot decode are left out, as multiplayer TLS
skips them) and includes GTS Root R1 and GlobalSign Root CA, which YouTube's ingest certificates chain to. It never
contacts YouTube:

```sh
cmake --build build-o2 --target stream_network_test
python3 tools/check-stream-rtmps.py build-o2/stream_network_test
```

The optional local network check uses a test-only build of the backend, a local
TLS proxy, and a local FFmpeg receiver. It verifies reconnect, saves a local copy
of each connection and checks with `ffprobe` that both files open with H.264 video,
AAC audio and a plausible duration, and rejects a trusted certificate for the wrong
hostname with the TLS CERTIFICATE reason. It never contacts YouTube:

```sh
cmake --build build-o2 --target stream_network_test
python3 tools/check-stream-network.py build-o2/stream_network_test
```

The external `ffmpeg`, `ffprobe` and `openssl` commands used by this check are development
test tools, not engine runtime dependencies.

`streaming_pacing` (`stream_test libx264 pacing [srcW srcH outW outH fps]`) feeds
7680×2160 frames at 60 FPS into a small 60 FPS stream and fails if more than 10%
of the frame slots are dropped once the encoder has started: the worker's resize
must keep pace (swscale's single-threaded bilinear needed about 50 ms per such
frame). It skips on machines with fewer than 4 hardware threads, where the
encoder, the scaler and the test's own feeder crowd each other. `rgbascale_test`
checks the scaler against a floating-point reference.

Encoder slowdowns skip late video frames without reconnecting. Audio catches up
independently; reconnects are reserved for connection/write failures. Title-screen
account and settings overlays are excluded from the broadcast. Title and lobby music is captured at the current master/music volume, then
audio switches to the game mixer when the match starts. Menu click sounds and
movie audio use separate players and are not captured. During loading
and modal screens, the last captured frame remains on the stream.

OpenGL capture downscales on the GPU before asynchronous readback when the
stream is smaller than the window. Frame buffers transfer to the encoder worker
without an extra full-frame copy on all render backends. The OpenGL path falls
back to full-size readback when framebuffer scaling is unavailable.

Native ultrawide streaming uses hardware HEVC when H.264 rejects its width.
YouTube lists HEVC as an accepted RTMPS codec in its
[encoder settings](https://support.google.com/youtube/answer/2853702?hl=en).
FFmpeg CPU SIMD paths are enabled for efficient RGB-to-YUV conversion. NASM is a
build-only dependency on x86; it adds no shipped runtime library.

NVIDIA capture feeds RGB directly to NVENC, which performs color conversion on
the GPU. Other hardware and CPU encoders use the SIMD-enabled FFmpeg converter.

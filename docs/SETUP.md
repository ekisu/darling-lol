# Setup and portability

<a id="nixos-package"></a>

## Nix package

See [prerequisites](NIX.md#prerequisites) before starting, and the
[Nix setup guide](NIX.md) for local checkout usage, updates, scripts, and runtime
details.

Run the experimental rootless package on x86_64 Linux:

```sh
nix --extra-experimental-features 'nix-command flakes' run github:lucannez64/darling-lol
```

To install permanently, adding the launcher to your user profile:

```sh
nix profile add github:lucannez64/darling-lol#default
# Older Nix/Lix versions without `profile add`:
# nix profile install github:lucannez64/darling-lol#default
```

Then run `darling-lol` or use the desktop application menu. See
[permanent installation](NIX.md#install-permanently) for details.

First launch builds Darling and guides you through Riot client installation.
NixOS is not required; other Linux distributions with Nix installed are also
supported.

## Native setup notes

The patch manifest pins Darling to `60ba801decee7a00782f74f6be4c8ffb013f79ff` and records every changed submodule's revision. `scripts/prepare.py` checks each revision before applying patches and never resets an existing checkout to make a patch apply. Re-running it accepts an already-applied patch.

Git LFS must be available before fetching the upstream submodules. The pinned `darling-swift` submodule's `.lfsconfig` points at the retired `git-lfs.darlinghq.org`, which no longer accepts connections; `prepare.py` redirects that submodule's LFS requests to GitHub, which still serves the objects. Without that override a fresh `git submodule update` blocks with no output right after `src/external/sqlite` is checked out. The local development checkout had removed prebuilt Swift files from its working directory; that deletion is deliberately not exported. A fresh upstream checkout keeps its original Swift assets and licensing. Do not install Git LFS pointer text as a library.

`configure.py` renders `@PROJECT_ROOT@`, `@HOST_HOME@`, `@GUEST_HOME@`, `@GUEST_USER@`, `@DPREFIX@` and `@DARLING_SDK@` into ignored local copies. A build-directory symlink preserves the original scripts' relative build layout. The public source files remain machine-independent.

```sh
python3 scripts/configure.py --guest-user my-linux-user
# A custom SDK can be rendered for advanced setups:
python3 scripts/configure.py --sdk /usr/local/libexec/darling
```

The normal wrappers currently support the default `~/.darling` prefix and `/usr/local` base installation. The configurator exposes a custom prefix for developers, but several archived tools assume the default layout. Custom prefixes/install locations have not been tested end to end.

The build uses Unix Makefiles because the incremental runtime helpers read CMake `flags.make` and `link.txt`. Ninja is not a drop-in replacement for those helpers.

The supported build is x86-64 only (`TARGET_i386=OFF`). The patched OpenDirectory uses Objective-C ivars that the 32-bit fragile ABI cannot compile. Keep the empty `CMAKE_BUILD_TYPE` used by `scripts/build.sh`: selecting `Release` introduces `NDEBUG` while objc4 defines `OBJC_IS_DEBUG_BUILD=1`, causing a debug-macro mismatch. The base patch requests the macOS 10.15 Objective-C runtime code generation to avoid undefined `__NSArray0__struct` / `__NSDictionary0__struct` symbols with newer Clang. Indium handles both the LLVM 22 and LLVM 23 branch opcode names.

`scripts/build.sh` compiles the whole tree with `-O3 -march=native -mtune=generic`, so the binaries only run on CPUs with the build machine's instruction set. Keep `-mtune=generic`: clang 23 tuning for AMD Zen (`-mtune=native` or `-mtune=znver*`) miscompiles Darwin thread-local variables, which crashes launchd at boot (see [troubleshooting](TROUBLESHOOTING.md#launchd-crashes-at-boot-after-changing-compiler-flags)). Two kernel files keep `-O0` because they depend on the unoptimized stack layout: `bsdthread_register.c` and `sigexc.c`.

Linux 5.14 or newer is required for `MADV_POPULATE_WRITE`, used when Darwin's allocator reuses lazily reclaimable pages. The memory installer checks this requirement; do not bypass it.

The runtime installer requires native `pkg-config` entries for `x11`, `x11-xcb`, `xcb-shm` and `xext`, plus the base Darling SDK.

`scripts/build.sh` and `scripts/install-runtime.sh` start with `scripts/preflight.py`, which checks the host versions the build and the runtime need: Python, CMake, Git LFS, the kernel, LLVM and its command line tools, the Vulkan API level, and the X display. A required check that fails stops immediately with one actionable line; the display check only warns, because a build does not need one. Run it directly with `python3 scripts/preflight.py` (`--json` for a machine-readable report), and set `RCLOL_SKIP_PREFLIGHT=1` to bypass the gate for one run.

Run configuration, builds and the per-user runtime installation as your ordinary account. Use sudo only for the base CMake installation and the incremental server installer. Running the whole build or per-user install under sudo can create root-owned files and configure the wrong guest home. `scripts/install-runtime.sh` initializes the default user prefix and creates `~/.darling/private/tmp` with mode `1777`, so the installer can write guest temporary files. It then shuts down Darling before replacing the paired runtime: first exit the game and both clients manually.

The normal launcher starts neither an Electron inspector nor client API/UI helpers. It clears inherited `DYLD_INSERT_LIBRARIES`, retains Darling runtime library/framework overrides, supplies an absolute log directory to the launcher, and selects the League product. The log-directory/Chromium argument shim, empty-autofill hook and optional native VPX bridge are no longer injected or built by runtime installation. Existing copies on disk are inactive and may remain. Child-process log-directory rewriting and Chromium feature overrides previously supplied by the shim are removed; client startup without those workarounds needs manual validation. The first-time installer uses `--skip-to-install` and omits the installed client's `--app-root`, avoiding an immediate exit or inaccessible intro screen. The project ships no inspector hooks, client API helpers, input automation or client binary patches. Keep account data, logs and captures private.

## Application-menu diagnostics and recovery options

`scripts/create-desktop-entry.sh` creates a `Darling Launcher` application-menu
entry that runs `scripts/desktop-run.sh`, preserving the normal launcher behavior
and saving stdout/stderr in `build/desktop-launch.log` with mode 0600. Logs can
contain private session details; do not attach raw launcher logs to a public
issue. The helper also removes the `league-of-legends.*` entry and icons written
by older versions, but only when it can prove this project created them. The
entry keeps `StartupWMClass=RiotClientServices`: Darling derives `WM_CLASS` from
the basename of `argv[0]`, so that is the window class of the client the entry
starts, while `Riot Client` is only its bundle display name.
Region/locale remain the existing `RCLOL_REGION`/`RCLOL_LOCALE` options.

`RCLOL_CDN_IPV4_ONLY` defaults to `1` in the Riot launcher and can be set to `0`
to disable the CDN-only resolver workaround. `RCLOL_RECOVER_HIDDEN_CLIENT=1`
opts in to Hyprland-only recovery of a default-prefix Riot session with no
visible window; it is off by default. See [troubleshooting](TROUBLESHOOTING.md#riot-exits-and-league-reports-that-it-lost-its-connection)
for the observed failures, recovery guards and NSURLSession limitations.

`RCLOL_RECOVER_STALE_CLIENT=1` opts in to partial runtime recovery before
launch. If `darlingserver` is alive, no game is running, no managed client
window is visible and stale Riot/League/CEF processes at least 60 seconds old
remain, the launcher stops only that client tree and reuses the runtime. It
recognizes `LeagueClientUx`, `Riot Client` and `RiotClientServices` windows and
preserves `darlingserver`, launchd/mldr, sockets and `.init.pid`; if X11/Xwayland
properties cannot be inspected it does nothing. See
[troubleshooting](TROUBLESHOOTING.md#client-remains-running-without-a-window).

`RCLOL_CLEAN_STALE_HELPERS=1` runs a preflight before the match helper starts.
It removes `close-league-client-in-game.py` and
`hide-league-client-in-game.py` processes of this prefix whose parent is
`init`/`systemd`, which means the launcher that started them has exited. A
helper of a live launcher, another prefix, the game, Riot/League/CEF and
`darlingserver` are preserved. It fails closed when a command line,
environment, stat entry or parent cannot be read. See
[troubleshooting](TROUBLESHOOTING.md#multiple-client-in-game-helpers).

`RCLOL_CLIENT_IN_GAME_TRICKS=1` opts in to automatic client handling during a
match. It reads `lol-user-experience` from the installed client's
`LCULocalPreferences.yaml`: with `potatoModeEnabled: true`, it closes the client
UI when `closeLeagueClientDuringGame: true`, and hides the managed
`LeagueClientUx`/`Riot Client` X11 windows when that value is false. Missing or
invalid preference values, disabled client optimizations and unreadable files
select no action. The low-level `RCLOL_CLOSE_CLIENT_IN_GAME=1` and
`RCLOL_HIDE_CLIENT_IN_GAME=1` switches remain available when the automatic
switch is not set. The feature is off by default, requires X11/Xwayland, and
does not patch client binaries, CEF or the game. The hide helper exits after ten
minutes without a client or match once it has seen one. See
[the troubleshooting measurement](TROUBLESHOOTING.md#automatic-client-handling-during-a-match)
for the measured CPU effect and limitations.

`RCLOL_FILTER_HOST_ENV=1` opts in to removing host desktop-session variables before
the guest starts: `WAYLAND_DISPLAY`, `XDG_SESSION_*`, `XDG_CURRENT_DESKTOP`,
GNOME/KDE/GDM, systemd, SSH agent, terminal, toolkit (`QT_*`, `GTK_*`, `GDK_*`),
Flatpak/Snap/AppImage and exported shell functions, and `SHELL` becomes `/bin/zsh`.
It is off by default. Variables that Darling or the host graphics and audio libraries read
are always kept: `DISPLAY`, `XAUTHORITY`, `XDG_RUNTIME_DIR` and the XDG base directories,
D-Bus, PulseAudio/PipeWire/ALSA, `DRI_PRIME`, `MESA_*`, `__GL_*`, `__NV_*`, `VK_*`,
`LD_PRELOAD` and `LD_LIBRARY_PATH`. List names to keep anyway in `RCLOL_KEEP_HOST_ENV`,
for example `RCLOL_KEEP_HOST_ENV=WAYLAND_DISPLAY`. The launcher always closes inherited
file descriptors above stderr before the guest starts.

## Updating an existing runtime

`./scripts/update.sh` updates everything after `git pull`: `build.sh`, then the base install, then `install-runtime.sh`. When a pull changes a patch, `prepare.py` finds the earlier version applied in that component (its copy in `build/applied-patches`, or a version from this repository's history), reverts it, and applies the new one. It never resets a repository; if neither version matches, it stops and names the repository with other edits. The steps below update single components instead.

These commands assume the pinned, patched source tree and its configured Makefiles already exist. From the repository root, build the two components with one worker to reduce resource contention:

```sh
cmake --build build/darling-src/build --target darlingserver system_kernel --parallel 1
python3 build/runtime/tools/build-riot-kqueue-runtime.py
sudo python3 tools/install-riot-darling-server.py
python3 tools/install-riot-memory-runtime.py
```

These two Python installers derive paths from the checkout/configuration and may be run directly from `tools/`; tokenized framework builders still require their rendered copies. The server installer defaults to `/usr/local/bin/darlingserver`, saves a checksum-named backup under `build/backups/darlingserver`, and preserves active server mappings. The CMake tree is configured without an optimization level, so `build-riot-kqueue-runtime.py` relinks `build/riot-libsystem_kernel.dylib` with the per-call paths rebuilt at `-O2`; the memory installer installs that relink by default into the guest `RuntimeLibraries` and the per-prefix bootstrap copy, and never overwrites it with a different library. It leaves the system SDK alone. Each replacement is staged and verified before installation; the memory installer prepares every destination before replacing any. Replacement is atomic per file, not a transaction across all files.

Memory backups default to **`build/memory-runtime-backups`**, separate from the potentially root-owned directory created by the sudo server installer. If a custom backup directory is unwritable, choose another with `--backup-dir /path/to/writable/backups`. See [permission troubleshooting](TROUBLESHOOTING.md#memory-installer-permission-denied). Neither installer stops or starts any process.

### Real CPU topology and the Foundation overlay

The kernel relink also reports the host's real CPU counts for `hw.*`/`machdep.cpu.*` (previously a fixed 8 logical / 8 physical MacBookPro16,1 configuration, which made the game create 7 worker threads on a 12-thread host). The memory installer above installs that library; no extra step is needed for the sysctl side. The rebuild also produces the commpage fix (`kNumCPUs`, physical/logical counts) as `build/riot-mldr`, whose SDK copy is root-owned:

```sh
sudo install -m755 build/riot-mldr /usr/local/libexec/darling/usr/libexec/darling/mldr
```

The optimized Foundation overlay is built and installed per user:

```sh
python3 build/runtime/tools/build-riot-foundation-runtime.py
python3 tools/install-riot-foundation-runtime.py
```

The installer seeds `~/.darling/Users/<user>/RuntimeFrameworks/Foundation.framework` from the SDK (the same overlay mechanism CoreFoundation/AppKit use), replaces `Versions/C/Foundation` atomically after staging a checksum-named backup in `build/foundation-runtime-backups`, and prints JSON; `--binary`/`--backup-dir` override the defaults. Do not run it while a client is mid-update; a restart is required for activation. To A/B the old fixed CPU counts without reinstalling, run `touch /tmp/rclol-fixed-cpu-count` before starting a guest process and `rm /tmp/rclol-fixed-cpu-count` to go back to the real topology (the flag is read once per process). **The in-game FPS effect of both changes is unmeasured.** The topology change can add worker threads to the clients as well; if clients become busier during matches, use the opt-out flag for a controlled comparison. Rollbacks: `build/memory-runtime-backups` and `build/foundation-runtime-backups` hold the previous libraries; the pre-change kernel artifact is `build/perf-research/impl-cpu/riot-libsystem_kernel.dylib.before`.

### Optimized libc, libm, libc++ and string-primitive overlays

Four more libraries have optimized overlays. They are built from the configured tree without a full CMake rebuild:

```sh
sh build/runtime/tools/build-riot-libs-runtime.sh
python3 build/runtime/tools/install-riot-libs-runtime.py
```

The builders recompile libsystem_c (618 of its 641 objects), libc++ and libc++abi (38 and 18) and the libsystem_platform string primitives at `-O3 -march=native -mtune=generic`, each object with its own configured defines, includes and per-file options, and relink with the configured link line. `-fno-strict-aliasing` is added for this old BSD C and no `-DNDEBUG`, `-ffast-math` or LTO is. Files whose correctness depends on frame layout, on running in a forked child, or on the floating-point environment keep their configured flags: the fork paths and the stack guard handler in libc and the frame-pointer backtrace walker. libsystem_m is not rebuilt: its sources are built at `-O0` upstream, and an `-O2` rebuild made the Riot client UI render a black window. The libkqueue objects libsystem_c also links belong to the kernel library's target and are left alone. Each builder stops while a League game process is running, because compiling would skew the FPS the user measures. `scripts/install-runtime.sh` builds and installs these overlays, then runs `tools/check-darwin-tlv-calls.py` on everything in `RuntimeLibraries` and `RuntimeFrameworks` and fails if any binary has the thread-local miscompile.

The installer writes the four files into `~/.darling/Users/<user>/RuntimeLibraries`, the directory the launcher puts in `DYLD_LIBRARY_PATH`; dyld searches those directories by leaf name before the install path, which is why an overlay there replaces the prefix and SDK copies of `libsystem_c.dylib`, `libc++.1.dylib`, `libc++abi.dylib` and `libsystem_platform.dylib` without touching either. Every replaced file is backed up first (`build/libs-runtime-backups`), the staged file is verified against the built artifact, and the rename is atomic. The installer never stops a process, so a running guest keeps the library it already loaded; exit the game and both clients and restart the namespace for activation. One command restores the previous state:

```sh
python3 build/runtime/tools/install-riot-libs-runtime.py --rollback
```

`--rollback` restores every file this installer replaced and removes the ones it created, so the SDK copy is used again. The policy and the rollback are checked offline by `python3 tools/riot-libs-installer-offline-check.py`, and each artifact is compared with the library it replaces by `python3 tools/riot_overlay_verify.py <artifact> <effective>` (same install name, versions, dependencies, re-exports, segment protections and exported symbols, no new undefined symbol). **The in-game FPS effect of these overlays is unmeasured.** The per-call effect of the string primitives is measured on the host (`build/riot-platform-memory-bench`); see [testing](TESTING.md#optimized-library-overlays-and-string-primitives).

### Metal and Indium overlays

The Metal wrapper and Indium share a private blit interface, so install them as a pair:

```sh
python3 build/runtime/tools/build-riot-indium-runtime.py
python3 build/runtime/tools/build-riot-metal-runtime.py
sh build/runtime/tools/install-riot-metal-pair.sh
```

The installer copies `build/riot-Metal` and `build/riot-libindium.dylib` into the guest `RuntimeFrameworks/Metal.framework` and `RuntimeLibraries`. It backs up the pair it replaces in `build/metal-pair-backups`; a repeated install keeps the older backup. `sh build/runtime/tools/install-riot-metal-pair.sh --rollback` restores that backup. `tools/install-riot-gpu-runtime.sh` still builds and installs the whole GPU set, including these two. Activation needs the namespace restart below.

The hand-off experiments (`metal-present-dispatch-wait`, `metal-deferred-blit` and `metal-worker-lease`) are **off by default**; two matches with them enabled had lower FPS. The table also lists diagnostics and tuning controls with their own defaults. Lazy event-loop wake-ups are **on by default** and `metal-no-lazy-wakeup` disables them. Files are read once per game process at startup unless an entry specifies a different refresh interval.

| Guest file | Effect when present |
|---|---|
| `/tmp/metal-present-dispatch-wait` | a presenting commit waits only for its own present dispatch on the submit thread, instead of the whole submit queue |
| `/tmp/metal-deferred-blit` | blit encoders are recorded on the game thread and replayed on the submit thread (only while deferred render encoding is on) |
| `/tmp/metal-worker-lease` | the Vulkan command buffer is set up by the first thread that records into it (the submit thread) instead of when `MTLCommandBuffer` is created |
| `/tmp/metal-skip-cpu-devices` | software Vulkan devices (lavapipe) are skipped while a hardware device exists |
| `/tmp/metal-gpu` | diagnostic: selects the Vulkan device used for Metal by index or case-insensitive name substring instead of the loader's first device. The OpenGL context that presents does not follow it; see [hybrid laptops](TROUBLESHOOTING.md#hybrid-graphics-laptops-amdintel-igpu--nvidia). `METAL_INDIUM_GPU` is the environment equivalent; the file wins |
| `/tmp/metal-all-devices` | a `VkDevice` is created for every supported Vulkan device, as before 2026-10-16. Without it only the selected device gets one; on a machine with lavapipe installed this also drops lavapipe's idle rasterizer threads |
| `/tmp/metal-no-drawable-vram` | drawables of a layer with `framebufferOnly` off (League sets it off) go back to host-visible system RAM, as before 2026-10-16. Without the file they are allocated in GPU memory like `framebufferOnly` drawables; nothing maps drawable memory from the CPU. On the RX 6600 this gave about 20 FPS more in a practice-tool fight and the same FPS when idle, not yet measured in an ARAM. `/tmp/metal-device.log` (with `DARLING_METAL_DEVICE_LOG=1`) shows `drawable-vram=on` and `storage=vram` |
| `/tmp/metal-no-lazy-wakeup` | every completion registration signals the event loop's wake-up semaphore from the CPU again, as before 2026-10-18. The handshake is per device. Without the file a registration signals only while the poller has announced a blocking wait (a spinning poller re-reads the registrations on its next pass, and its spin ends when one arrives). On the NVIDIA proprietary driver each `vkSignalSemaphore` is an RM control ioctl that waits on a driver-wide lock, about 0.3 ms each and ~25 per League frame, and it was the largest kernel cost of the submit worker. Read once per game process; the wait log counts `wakeupSignalled` and `wakeupSkipped` |
| `/tmp/metal-submit-batch-us` | the submit batch window in microseconds (500–100000, default 500); read once per game process, so set it before the game starts. A presenting commit and every CPU wait still flush the batch at once. On the NVIDIA proprietary driver every `vkQueueSubmit2` is an RM control ioctl that waits on a driver-wide lock (about 0.4 ms each in a League match); with the default window a frame took about 8.5 submits, with `5000` about 2.5 (RTX 4090: 89 → 113 FPS, same match) |
| `/tmp/metal-submit-batch-max` | command buffers per batch (1–256, default 16), read once like the window; `64` lets one batch hold a whole frame |
| `/tmp/metal-debug-waits` | diagnostic: counts how often and how long threads block at the hand-off waits and in `nextDrawable`, every 5 s in `/tmp/metal-trace.log`; the first check writes a `waits enabled` line |

#### Startup configuration (environment)

Switches that the game used to re-read from guest files while it ran are now read once per game process, at their first use. Some of these lookups ran on every frame or every few dozen command buffers. Each one also has an environment variable. A guest `access()` or `fopen()` of a missing file costs about 28 µs under Darling, 9 µs when the file exists (RX 6600 host, 2026-10-22). The checks removed here added up to roughly 60–75 µs per frame at 300 frames per second, the presenting commit's on the game thread. That is an estimate from call counts, not a measured frame-rate change.

The value is resolved in this order:

- the variable, when it is set and not empty: `0` means off, anything else on; numbers are decimal and clamped to the range shown, and text that is not a whole number keeps the default;
- otherwise the former guest file, checked once, with its former meaning (present = on, or the number in it), so creating the file before the game starts still works;
- otherwise the default, unchanged.

A change, by file or by variable, takes effect in the next game process: restart the game, not the whole runtime. Set the variables in the environment that starts the client (for example before `scripts/run.sh` or `darling-lol`); the launcher passes the environment through to the game.

| Variable | Former guest file | Default | Effect |
|---|---|---|---|
| `DARLING_METAL_ASYNC_NO_FRAME_SYNC` | `/tmp/metal-async-no-frame-sync` | off | no frame-end wait at all (A/B only) |
| `DARLING_METAL_NO_BINDING_REUSE` | `/tmp/metal-no-binding-reuse` | off | turns binding reuse off |
| `DARLING_METAL_RETIRE_TINY` | `/tmp/metal-retire-tiny` | off | small command buffers also retire on the worker |
| `DARLING_METAL_NO_SUBMIT_BATCH` | `/tmp/metal-no-submit-batch` | off | turns submit batching off |
| `DARLING_METAL_SUBMIT_BATCH_US` | `/tmp/metal-submit-batch-us` | 500 | batch window, 500–100000 µs |
| `DARLING_METAL_SUBMIT_BATCH_MAX` | `/tmp/metal-submit-batch-max` | 16 | command buffers per batch, 1–256 |
| `DARLING_METAL_NO_LAZY_WAKEUP` | `/tmp/metal-no-lazy-wakeup` | off | every completion registration signals the wake-up semaphore |
| `DARLING_METAL_ASYNC_UPLOADS` | `/tmp/metal-async-uploads` | off | texture uploads without the synchronous wait |
| `DARLING_METAL_NO_LAZY_PASS` | `/tmp/metal-no-lazy-pass` | off | render passes begin at creation, not at the first draw |
| `DARLING_METAL_ASYNC_ENCODE` / `DARLING_METAL_NO_ASYNC_ENCODE` | `/tmp/metal-async-encode` / `/tmp/metal-no-async-encode` | on in the game process only | deferred render encoding on in any process / off in the game |
| `DARLING_METAL_OUTLINE_PAIRS` / `DARLING_METAL_OUTLINE_PAIRS2` | `/tmp/metal-outline-pairs` / `/tmp/metal-outline-pairs2` | 0 / 16 | outline chain passes kept, 0–16; other values keep the default |
| `DARLING_METAL_SKIP_<1180, 512, 1024, 1080, 242, UI, SCENE>` | `/tmp/metal-skip-<1180, 512, 1024, 1080, 242, ui, scene>` | off | diagnostic: skip the draws of render passes of that size |
| `DARLING_METAL_SNAPSHOT` | (none) | off | diagnostic: watch for the `/tmp/metal-snapshot` trigger file, a few times a second, while the game runs |

Not converted, on purpose:

- switches that were already read once per process (the rest of the table above, the direct-presentation and paced-commit switches);
- diagnostic triggers that are only polled while a startup diagnostic is on: `/tmp/metal-seqlog` under GPU timing, the draw-limit and state-flag experiments under `/tmp/metal-debug-draws`;
- the frame-interval capture trigger `/tmp/metal-frame-interval-capture`, checked once per 128 presented frames until it fires. Benchmarks arm it during a running game;
- output files such as the logs, captures and `/tmp/indium-submit-stats.log`, written once per 1024 presented frames.

#### Direct Vulkan presentation (on through the launcher)

With direct presentation on, a `CAMetalLayer` presents its drawables through a Vulkan swapchain created directly on the layer's X11 window (`VK_KHR_xlib_surface`), instead of copying each frame into the OpenGL texture that the compositor draws on the main thread. The swapchain images are the drawables, so there is no copy per frame. The present runs on the submit thread, after the frame's command buffer, and the main thread no longer composites Metal frames. With `displaySync` off (League turns it off), the swapchain uses `IMMEDIATE`, then `MAILBOX` if that is missing, then `FIFO`. With `displaySync` on it uses `FIFO`. The device needs `VK_EXT_swapchain_maintenance1`, and the instance `VK_KHR_xlib_surface`. When a requirement is missing, the layer stays on the legacy path and the reason goes to `/tmp/metal-direct-present.log`. Possible reasons include a window size that differs from the drawable size, a pixel format that is not a surface format, too few images or no opaque composite alpha. A layer whose present fails also goes back to the legacy path. The legacy frame-end wait (`drain@commit`) is kept on both paths.

`tools/run-riot-client.sh` turns on direct presentation and paced direct commits for every client and game it starts: it sets `DARLING_METAL_DIRECT_PRESENT=1` and `DARLING_METAL_DIRECT_ASYNC_COMMIT=1`, and the game inherits both from the client. To turn one off, start the client with the variable set to `0` or empty, for example `DARLING_METAL_DIRECT_ASYNC_COMMIT=0 sh scripts/run.sh`. The launcher passes that on as `0`. Each value is read once per game process, so a change applies to the next game, and restarting the game is enough. Paced commits only apply to directly presented frames, so with `DARLING_METAL_DIRECT_PRESENT=0` both are off.

Outside the launcher (a standalone probe, a guest shell) both are off unless the variable or a guest file below turns them on. A non-empty variable wins over the guest files: `0` means off, anything else on. Without it, `/tmp/metal-direct-present` and `/tmp/metal-direct-async-commit` turn the features on. A libindium built with `-DINDIUM_DIRECT_PRESENT_DEFAULT_ON=1` / `-DINDIUM_DIRECT_ASYNC_COMMIT_DEFAULT_ON=1` (the replay A/B variants) starts with them on; `/tmp/metal-no-direct-present` / `/tmp/metal-no-direct-async-commit` then turn them off.

It changes QuartzCore (`patches/cocotron.patch`), Indium (`patches/metal--deps--indium.patch`) and Metal (`patches/metal.patch`). Drawables gained an appended virtual, so install QuartzCore and libindium from the same build. `tools/install-riot-gpu-runtime.sh` builds and installs the whole GPU set.

| Guest file | Effect when present |
|---|---|
| `/tmp/metal-direct-present` | enables direct presentation when `DARLING_METAL_DIRECT_PRESENT` is not set (read once per game process) |
| `/tmp/metal-no-direct-present` | disables it in a default-on libindium when the variable is not set; ignored otherwise |
| `/tmp/metal-direct-async-commit` | when `DARLING_METAL_DIRECT_ASYNC_COMMIT` is not set: a presenting commit whose drawables are all direct skips the frame-end drain and waits only for the previous presenting commit (see below). A libindium built with `-DINDIUM_DIRECT_ASYNC_COMMIT_DEFAULT_ON=1` enables it unless `/tmp/metal-no-direct-async-commit` exists |
| `/tmp/metal-direct-diag` | diagnostic: durations and counts of the presentation sites of both paths, every 5 s in `/tmp/metal-direct-diag.log` (n, mean, max and log2-bucket p50/p95). Without the file the counters cost one predictable branch |

`/tmp/metal-direct-present.log` records swapchain creation, activation, fallback and retirement once per transition, never per frame.

The frame-end drain exists for legacy presentation: the worker queues the present to the main thread, which must see it before the game pumps its run loop. A direct present is issued on the worker itself, so for a frame that presents only direct drawables the drain is pacing, not correctness. With `/tmp/metal-direct-async-commit` such a commit posts its work and waits only until the previous presenting commit's task has finished. The worker can still be replaying, submitting and presenting the current frame while the game records the next one, never more than one presenting frame behind. A frame that presents a legacy drawable, or a mix, keeps the full drain. `/tmp/metal-async-no-frame-sync`, which removes the frame-end wait altogether, takes precedence. `waitUntilCompleted`, texture uploads and the other explicit waits still drain the worker first. At exit, a handler registered at the first paced commit waits at most 1 s for the queue, so the last frame is not presenting while the process tears down. The wait is counted as `presentPacing@commit` by `/tmp/metal-debug-waits`. Drawables gained `presentsIndependently()`, an appended virtual, so libindium and QuartzCore must come from the same build (Metal is unaffected).

Measured on the same setup as direct presentation (RX 6600, KWin DP-3 1920×1080@144, League 16.21 replay, fixed scene). Arms used the same QuartzCore and differed only in the libindium default. Against direct presentation with the drain, presentation requests per second were +11.4%, +5.4% and +4.1% in three paired runs (mean +6.9%). The queue-interval p50 was 4–9% shorter in each pair. The drain arm varied more between runs, 8% against 2%. With `/tmp/metal-debug-waits`, `presentPacing@commit` blocked 6 times in a replay where `drain@commit` had blocked about 10,000 times. KWin scanned out the game's buffers in both arms. Displayed frame rate, input latency and a full match are not measured.

Measured on an RX 6600 (RADV, Mesa 26.2.4) in a League 16.21 replay, in fullscreen 1920×1080 at 144 Hz on KWin 6.7.5 with `AllowTearing` at its default (on). Base and candidate both had the CPU address shadow. Presentation requests per second were +2.1%, +9.6% and +7.0% in three paired runs. That is the request rate, not the frame rate shown on screen. The DRM plane state showed KWin scanning out the game's own buffers on both paths. With direct presentation the scanout buffer changed about as often as the game presented (~294 flips/s, `IMMEDIATE`). The legacy path's flips stayed locked to the 144 Hz refresh. Nested gamescope (Wayland or SDL backend) made KWin composite again. A practice game through the Riot client also ran with the direct path active. Not covered yet: losing the window or the X connection during a game, NVIDIA and Intel drivers, apps with several Metal layers, and a full ranked or normal match.

Create these files from inside the guest, for example `darling shell touch /tmp/metal-debug-waits`, or while Darling is shut down. The guest `/tmp` is `~/.darling/private/tmp` seen through Darling's overlay mount. A file created on the host in that directory while Darling runs may stay invisible to guest processes that already looked for it. The same applies to removing a file. The older `/tmp/metal-no-present-dispatch-wait`, `/tmp/metal-no-deferred-blit` and `/tmp/metal-no-skip-cpu-devices` files are ignored.

### Game-process switches

The kernel library decides these once when a process starts, and only for the League game process (`.../LeagueofLegends.app/Contents/MacOS/LeagueofLegends`). The Riot and League clients keep the server path. Each switch is a host file, so it takes effect at the next game start without reinstalling:

| Path | Default in the game | Host file |
|---|---|---|
| Local pthread mutexes (first-fit) | on | `/tmp/rclol-no-local-mutex` turns it off |
| Local condition variables | on | `/tmp/rclol-no-local-condvars` turns it off |
| Local Mach semaphores | on | `/tmp/rclol-no-local-semaphores` turns it off |
| Core isolation of the main thread | off | `/tmp/rclol-core-isolation` turns it on (it lowered FPS) |
| Region-query pacing (50 µs) | on | `/tmp/rclol-region-pace-us` overrides the interval |
| Main-thread RPC reply polling | off | `/tmp/rclol-rpc-spin-us` (µs, at most 500) turns it on |

The former opt-in files `/tmp/rclol-local-mutex`, `/tmp/rclol-local-condvars` and `/tmp/rclol-local-semaphores` are ignored. Each game start appends its decisions to `/tmp/rclol-game-decisions.log`, for example `local_semaphores=1` and `core_isolation=0 errno=-1`. `-all` variants (`/tmp/rclol-local-mutex-all` and the like) enable a local path in every process; they exist for probe tests and are not for normal play.

An on-disk update is not activation. Existing processes keep their old executable/library mappings, and Darling's active overlay can expose an old library even to a new guest process. After manually exiting the game and both Riot/League clients, restart the entire namespace:

```sh
darling shutdown
sh scripts/run.sh
```

`darling shutdown` terminates the guest namespace; never run it during a match or while applications have work to save. Reopening only the client is insufficient. `scripts/run.sh` renders the current launcher templates for the next launch. No FPS/RAM improvement has yet been established for these incremental changes; see [status](STATUS.md) and [standalone checks](TESTING.md).

For GL resolvers, multi-window presentation and the LaunchServices crash fix, use the full paired installation from the README after exiting clients. It rebuilds the native GL wrapper and installs both GL library copies, as well as the LaunchServices absolute subframework path used by Electron. Copying only the OpenGL framework or only the flat LaunchServices override leaves an old library reachable.

Build/install steps need substantial time and storage. All 29 component patches were checked against their pinned base indexes. The runtime has been exercised on the new RX 6600 PC, but an entirely fresh checkout/build/install remains to be validated on another machine; patch applicability alone does not establish distro/driver support. Use `JOBS=2 ./scripts/build.sh` if the default `nproc` parallelism exceeds your available RAM. See [troubleshooting](TROUBLESHOOTING.md) before changing compiler flags or prefix ownership.

## Committing from a new PC

Building does not require a Git author identity, but committing does. If Git reports `Author identity unknown`, configure your own name and email in this repository (use your GitHub noreply address if desired):

```sh
git config user.name "Your Name"
git config user.email "your-commit-email"
```

Omitting `--global` keeps these settings local to this checkout. Commit source patches, tools and docs; keep `build/`, binaries, backups, captures and account data ignored.

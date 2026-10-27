# Fork harvest (2026-10-21)

Scanned all 44 forks of `lucannez64/darling-lol`; six had commits upstream lacks.
Large raw patches are not stored here — the SHAs below reproduce them:
`https://github.com/lucannez64/darling-lol/compare/main...<owner>:main.patch`

| Fork | Ahead | Verdict |
|---|---|---|
| `t1stm` (`0a00fc7ac6`) | 111 | Presentation/FPS work (PR #94, closed), XAUTHORITY/session-bus fix, stale prefix `libsystem_kernel` cleanup, platform UUID as provisioning UDID. Its `docs/reviews/pr41-cpuid-security-validation.md` is the useful part and is copied here. |
| `DevWebeloper` (`5f13d67a`) | 6 | Stealth line: SMBIOS generation + Metal GPU spoof + env sanitization. Patch kept in this directory. |
| `QuinsZouls` | 78 | Packaging/CI only; not relevant. |
| `nicolai455` | 13 | Nix flake/rootless; not needed on this machine. |
| `mohsen-ameli` | 9 | Launcher icons, post-install client fix, fullscreen game window (PR #6 open). Raw patch dropped; see fork. |
| `Andbal23` | 1 | Metal GPU name sanitizer (env override, VRAM from Vulkan) + root certificate provisioning. Patch kept here. |

Already present in this repository, so not ported:
- SMBIOS/UUID generation: `tools/configure-darling-platform.py` (enforces provisioning UDID == IOPlatformUUID).
- Root certificates: `tools/ensure-darling-root-certificates.py`.
- Environment filtering: `tools/run-riot-client.sh` unsets the DARLING_*/RCLOL_* knobs.
- GPU name masking: `patches/metal.patch` already maps NVIDIA to the MacBookPro16,1 AMD identity.

Worth borrowing later (not applied):
- `sanitizeGpuName()` from Andbal23: `DARLING_METAL_GPU_NAME` override and VRAM read from Vulkan memory heaps instead of a constant.
- `DARLING_METAL_VRAM_MB` knob for `recommendedMaxWorkingSetSize`.

CPUID faulting notes: `pr41-cpuid-validation.md` lists the failures of the earlier PR #41
candidate (inconsistent leaf 0xD, AVX-512 advertised without a host, lost SIGSEGV handler,
`ENODEV` on Zen 3). `pr41-cpuid-ryzen5500.json` is the recorded run. Our implementation in
`patches/xnu.patch` (`cpuid_fault.c`) avoids those: it hooks the existing sigexc handler
instead of replacing it, masks AVX-512 in leaves 7, 0xD and 0x80000001, and leaves unknown
leaves to the real CPU.

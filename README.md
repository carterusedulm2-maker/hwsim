# hwsim — brcmfmac Virtual Bus + Firmware

Two-component mock stack that lets Linux 6.12.81 `brcmfmac` driver enumerate, `probe()`, and expose a `wlan0` interface **without any real Broadcom hardware**. Target use cases: wpa_supplicant / hostapd CI, driver development, fault-injection QA.

| Component | Role |
| --- | --- |
| `sim_sdio.c` (Kconfig: `CONFIG_BRCMFMAC_HWSIM_SDIO`) | In-driver bus shim replacing the real SDIO bus layer. Registers a platform_device + calls `brcmf_attach()`. |
| `brcmfmac_hwsim.ko` (`drivers/.../brcmfmac/hwsim/`) | Standalone virtual firmware module. Handles BCDC ioctls, generates events, simulates interrupts, supports loopback + debugfs fault injection. |

## Repository layout

```
drivers-overlay/    Files that overlay onto a clean Linux 6.12.81 tree
patches/            Full git format-patch series (0001..0011)
docs/               Architecture notes, design reviews, session protocol
task_plan.md        Active plan (phase tracking)
progress.md         Session log (what was done, what failed, what's next)
findings.md         Research notes (kernel internals, chip IDs, iovars...)
```

## Building

1. Clean tarball of Linux 6.12.81 from `cdn.kernel.org`.
2. Overlay `drivers-overlay/` onto the source tree (`rsync -a drivers-overlay/ linux-6.12.81/`).
3. Enable in `make menuconfig`:
   - `CONFIG_BRCMFMAC=m`
   - `CONFIG_BRCMFMAC_HWSIM_SDIO=y`
   - Disable real `CONFIG_BRCMFMAC_SDIO / USB / PCIE`.
4. Build: `make -j$(nproc) M=drivers/net/wireless/broadcom/brcm80211/brcmfmac modules`.

Alternative: apply `patches/*.patch` in order via `git am`.

## Runtime validation

Currently booting on arm64 Ubuntu 24.04 + custom kernel under QEMU/HVF on Apple Silicon. See `progress.md` for the live phase tracker.

Milestone 1 status: **probe advances through BCDC; cfg80211_attach currently blocked on a missing D11-version iovar in the virtual-firmware dcmd table.**

## License

Dual BSD/GPL (matches upstream `brcmfmac`).

# Progress Log

## Session: 2026-04-15

### Phase 1: Requirements & Discovery
- **Status:** complete
- **Started:** 2026-04-15 14:49 UTC
- Actions taken:
  - Invoked the `planning-with-files` skill.
  - Read the skill instructions and templates.
  - Checked whether project-root planning files already existed.
  - Ran session catchup for unsynced context.
- Files created/modified:
  - `task_plan.md` (created)
  - `findings.md` (created)
  - `progress.md` (created)

### Phase 2: Planning File Initialization
- **Status:** complete
- Actions taken:
  - Initialized all three project-root planning files.
  - Recorded the current workflow, decisions, and constraints for future sessions.
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 3: Protocol Adoption
- **Status:** in_progress
- Actions taken:
  - Set the repository up to use planning files as durable working memory for subsequent complex tasks.
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 6: hwsim Test Flow Study
- **Status:** complete
- **Started:** 2026-04-15 14:50 UTC
- Actions taken:
  - Confirmed the local `hostap` tree is on `main`.
  - Confirmed the local kernel tree contains `brcmfmac` for Linux `6.12.81`.
  - Collected the first-pass inventory of `hostap/tests/hwsim/`.
  - Collected first-pass external summaries for `mac80211_hwsim` and `brcmfmac` to guide source reading.
  - Read `hostap/tests/hwsim/README` and `hwsim.py` to map the hwsim test harness and radio-creation path.
  - Read `brcmfmac` bus/proto/fwil/fweh/BCDC/MSGBUF/PCIe/SDIO/USB entry points to map the minimum transport contract and the real control/data/event flows.
  - Read `run-all.sh`, `start.sh`, `run-tests.py`, `hostapd.py`, and `wlantest.py` to map the end-to-end hwsim lab bring-up, control model, and verification flow.
  - Read `core.c` and deeper `msgbuf.c` functions to confirm how ioctl completion, event completion, RX completion, and D2H ring draining are dispatched.
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 7-10: Architecture and Plan Synthesis
- **Status:** complete
- Actions taken:
  - Compared the SDIO+BCDC and PCIe+MSGBUF paths from the perspective of simulator complexity and fidelity.
  - Derived a transport-neutral fake-firmware core plus transport-specific shim architecture.
  - Chose SDIO+BCDC as the recommended first implementation target and defined a phased extension path to PCIe+MSGBUF.
  - Folded in the background hwsim study, which confirmed the concrete fixture layout and per-test orchestration in `run-tests.py` and `start.sh`.
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 11: Amended Architecture Design
- **Status:** complete (v1, superseded by Phase 11 v2)
- **Started:** 2026-04-15
- Actions taken:
  - User amended requirement: mock bus must be compile-time Kconfig option inside brcmfmac, NOT external module
  - Studied brcmfmac Kconfig: `CONFIG_BRCMFMAC_SDIO` selects `BRCMFMAC_PROTO_BCDC`, `CONFIG_BRCMFMAC_PCIE` selects `BRCMFMAC_PROTO_MSGBUF`
  - Studied Makefile conditional compilation: `brcmfmac-$(CONFIG_BRCMFMAC_SDIO) += sdio.o bcmsdh.o`
  - Studied `bus.h` registration pattern: `#ifdef CONFIG_BRCMFMAC_SDIO ... brcmf_sdio_register()` with inline stubs
  - Studied `core.c:brcmf_core_init()` sequential registration of all bus backends
  - Studied `bus_priv` union: `{sdio, usb, pcie}` — needs new `sim` member
  - Studied SDIO probe chain: `sdio_register_driver()` → `brcmf_ops_sdio_probe()` → `brcmf_sdio_probe()` → fw request → `brcmf_sdio_firmware_callback()` → bus setup → `brcmf_alloc()` + `brcmf_attach()`
  - Studied PCIe probe chain: `pci_register_driver()` → `brcmf_pcie_probe()` → fw request → `brcmf_pcie_setup()` → ring init → `brcmf_attach()`
  - Studied `brcmf_attach()` → `brcmf_fwvid_attach()` → `brcmf_proto_attach()` → `brcmf_fweh_attach()` → `brcmf_bus_started()` → wlan0 creation
  - Studied `brcmf_c_preinit_dcmds()`: 10+ ioctl/iovar calls that mock must handle
  - Studied `brcmf_fwvid_attach()`: requires valid `fwvid` enum; WCC is simplest choice
  - Designed platform_device self-probe approach that bypasses real SDIO/MMC and PCI subsystems
  - Designed firmware loading bypass: skip `brcmf_fw_get_firmwares()` entirely
  - Produced complete probe flow diagram for mock SDIO
  - Produced fake firmware command table for `brcmf_c_preinit_dcmds()`
  - Updated task_plan.md with new phases (12-17), file map, probe flow, and architecture decisions
  - Updated findings.md with build system findings, probe path findings, amended architecture
  - Updated progress.md with Phase 11 log and new handoff block
- Files created/modified:
  - `task_plan.md` (major rewrite)
  - `findings.md` (amended architecture + build/probe findings)
  - `progress.md` (Phase 11 entry + new handoff)
- **User feedback:** Architecture was WRONG. User clarified this is a TWO-MODULE design: bus shim (inside brcmfmac) + separate mocked bus module (separate .ko). Superseded by Phase 11 v2.

### Phase 11 v2: Amended Architecture Design — Two-Component
- **Status:** complete
- **Started:** 2026-04-15
- Actions taken:
  - Received user's critical correction: mocked bus is a SEPARATE kernel module, not code inside brcmfmac
  - Re-studied SDIO txctl/rxctl/txdata paths to understand exact data flow between driver and hardware
  - Re-studied BCDC dcmd wire format: `brcmf_proto_bcdc_dcmd` with cmd/len/flags(reqid,set,ifidx)/status
  - Re-studied BCDC data header: `brcmf_proto_bcdc_header` with flags/priority/flags2(ifidx)/data_offset
  - Re-studied SDIO DPC/interrupt mechanism: ISR sets ipend → DPC reads status → processes frames
  - Re-studied PCIe interrupt mechanism: quick ISR → threaded ISR → `brcmf_proto_msgbuf_rx_trigger()`
  - Redesigned architecture: TWO components:
    1. Bus shim (inside brcmfmac, Kconfig-gated): implements brcmf_bus_ops, forwards to mocked bus module
    2. Mocked bus module (brcmfmac_hwsim.ko): the "firmware brain" — parses CDC/MSGBUF, handles ioctls, converts TX→RX, generates events, simulates interrupts
  - Documented complete packet flow for: IOCTL (BCDC), Data TX→RX (BCDC), Firmware Event, Firmware Download
  - Designed module loading order: hwsim.ko loads first, brcmfmac loads second, shim finds hwsim during probe
  - Designed the file map with hwsim/ subdirectory for the mocked bus module
  - Identified IPC interface requirements between bus shim and mocked bus module
  - Updated all three planning files with corrected architecture
- Files created/modified:
  - `task_plan.md` (complete rewrite for two-component architecture)
  - `findings.md` (v2 architecture + BCDC wire format + interrupt details)
  - `progress.md` (Phase 11 v2 entry + updated handoff)

## Test Results
| Test | Input | Expected | Actual | Status |
|------|-------|----------|--------|--------|
| Skill initialization | Invoke `planning-with-files` | Skill loads and instructions become available | Skill loaded successfully | ✓ |
| Session catchup | Run `.github/skills/planning-with-files/scripts/session-catchup.py "$(pwd)"` | Report any unsynced context | No unsynced context reported | ✓ |
| Planning file existence | Check project root for planning files | Files should exist after initialization | Files created in project root | ✓ |
| hostap branch check | `git -C hostap branch --show-current` | `main` | `main` | ✓ |
| kernel tree check | verify `linux-6.12.81/.../brcmfmac` exists | present | present | ✓ |
| hwsim baseline doc | inspect `hostap/tests/hwsim/README` | identify orchestration model | five-radio test harness and control-interface model identified | ✓ |
| brcmfmac bus boundary | inspect `bus.h` and protocol files | identify host/bus abstraction | `txdata` / `txctl` / `rxctl` and proto split identified | ✓ |
| hwsim orchestration | inspect `run-all.sh` / `start.sh` / `run-tests.py` | identify bring-up and execution flow | orchestration flow identified | ✓ |
| msgbuf completion flow | inspect `msgbuf.c` and `core.c` | identify ioctl/event/rx completion handling | completion and dispatch paths identified | ✓ |
| architecture decision | compare SDIO/BCDC vs PCIe/MSGBUF | pick the recommended first target | SDIO+BCDC selected as phase-1 target | ✓ |

## Error Log
| Timestamp | Error | Attempt | Resolution |
|-----------|-------|---------|------------|
| 2026-04-15 14:49 UTC | None | 1 | N/A |
| 2026-04-15 14:50 UTC | None for research startup | 1 | N/A |

## 5-Question Reboot Check
| Question | Answer |
|----------|--------|
| Where am I? | Phase 6: hwsim Test Flow Study |
| Where am I going? | Implementation can start from the recommended SDIO+BCDC simulator baseline, then extend to PCIe+MSGBUF |
| What's the goal? | Produce an architecture design and plan for a simulated bus driver that can exercise `brcmfmac` using the studied code paths |
| What have I learned? | `hwsim` is the outer AP/STA test harness; `brcmfmac` needs a separate fake transport plus fake-firmware model |
| What have I done? | Mapped the code paths and synthesized the simulator architecture and phased implementation plan |

## Handoff Block
- **Current goal:** Implement two-component simulation stack: bus shim (inside brcmfmac) + mocked bus module (brcmfmac_hwsim.ko)
- **Current phase:** Plan v5.2 (QA Review 整合完成)，等待 user 核准後開始實作
- **What is done:**
  - Full research (Phases 1-10) on hwsim, brcmfmac internals, build system, probe paths
  - Phase 11 v1 (single-module): designed then rejected
  - Phase 11 v2 (two-module): corrected architecture
  - Phase 11 v3 (plan with git + build strategy)
  - CEO Review: 3 critical blockers resolved, 6 high-risk items mitigated
  - Engineering Review: 1 BLOCKER + 5 HIGH + 4 MEDIUM 整合至 plan v5
  - Firmware handling: bypass strategy + get_blob graceful degradation 整合至 v5.1
  - QA Review: 7 BLOCKERS + 14 HIGH + 17 MEDIUM — 全數整合至 plan v5.2
  - **Design Review: 2 BLOCKERS + 9 HIGH + 5 MEDIUM — 全數整合至 plan v5.3**
    - D1: brcmf_txcomplete → brcmf_proto_bcdc_txcomplete (函式名全文修正)
    - D2: BCDC error codes 必須用 BCME_* (NOT Linux errno)
    - D3-D5,D11: Probe-FATAL 分類修正 (join_pref/REVINFO/clmver/country 非 fatal)
    - D6,D8: probe 補齊 brcmf_get_module_param + dev_set_drvdata
    - D9: chip=0xFFFF 統一 (修正 Probe Flow 0x4339 → 0xFFFF)
    - D10: rxctl 回傳 byte count 而非 0
  - All open decisions resolved (IPC, Kconfig, VID/PID, build env, PCIe scope)
- **What remains:** Phases 11.5-18 implementation (17 commits, ~7 phases)
- **Exact next action:** Phase 11.5 — `git init` linux-6.12.81, create hwsim-dev branch
- **Resolved decisions:**
  - IPC: `symbol_get()` / `symbol_put()`
  - Kconfig: HWSIM_SDIO XOR real SDIO (互斥)
  - VID/PID: Broadcom VID + fake PID (chip=0xFFFF)
  - Build: Intel NUC native compile (user handles setup)
  - Git: local dev branches only, no GitHub push
  - Phase 16 (PCIe): deferred to separate milestone
  - Default ioctl: GET → BCDC status=-23 (BCME_UNSUPPORTED), SET → status=0 [D2]
  - Thread safety: completion + symbol_get refcount (no RCU needed)
  - maxctl: 8192 (same as real SDIO)
  - cap iovar: `"sta wl 11n 11ac 11d 11h mfp"`
  - event_msgs: 27 bytes (BRCMF_E_LAST/8 for WCC)
  - Firmware: bypass request_firmware(), no files needed
  - TX completion: `brcmf_proto_bcdc_txcomplete()` [D1]
  - rxctl return: byte count (positive), not 0 [D10]
- **Blockers:** Awaiting user plan approval
- **Validation status:** Plan v5.3 ready for final approval

### Copilot Architecture Review of Plan v5.2 (2026-04-15)
- **Status:** complete
- **Scope:** Source-verified design review against linux-6.12.81 kernel source
- **Method:** Read every referenced .c/.h file; verified claims against actual code line-by-line
- **Findings:** 3 🔴 CRITICAL + 6 🟡 HIGH + 5 🟢 MEDIUM → recorded in findings.md
- **Key findings:**
  - DR-01 🔴: `brcmf_txcomplete()` → `brcmf_proto_bcdc_txcomplete()` (confirms D1)
  - DR-02 🔴: `stop` bus_op is mandatory (no NULL check in brcmf_bus_stop)
  - DR-03 🔴: maxctl=8192 overwritten by BCDC to 10256 (misleading)
  - DR-04 🟡: Kconfig must `select BRCMFMAC_PROTO_BCDC`
  - DR-05 🟡: `dev_set_drvdata` missing from probe flow (confirms D6)
  - DR-06 🟡: Teardown missing `brcmf_detach()` + `brcmf_free()`
  - DR-07 🟡: Event frame format under-specified (BCDC+ETH+BRCM+msg_be)
  - DR-08 🟡: IPC response buffer lifecycle needs spec
  - DR-09 🟡: WCC module must be pre-loaded (hard dependency)
  - DR-10–14 🟢: chip name cosmetics, settings cleanup, ver format, platform ordering, txhdrlen
- **Assessment:** Architecture is SOUND; all issues are fixable implementation gaps
- **Probe confidence:** 78% (rises to ~95% after fixing DR-01 through DR-05)
- Files modified:
  - findings.md (full review appended)
  - progress.md (this entry)

---
*Update after completing each phase or encountering errors*

### Phase 14: hwsim Module Implementation — COMPLETE
- **Status:** done (committed as 6cfeb6cd8)
- Actions taken:
  - Created hwsim/hwsim_core.c (~780 lines) with full BCDC ioctl table
  - Created hwsim/Makefile
  - Added `obj-m += hwsim/` to parent Makefile (CONFIG_BRCMFMAC_HWSIM_SDIO guard)
- Key components: BCDC parser, ioctl/iovar handlers, scan/connect/AP event generators, TX→RX loopback, fw_download

### Phase 15: sim_sdio.c Bus Shim — COMPLETE
- **Status:** done (committed as cff0d6dae)
- Actions taken:
  - Created sim_sdio.c (~350 lines) with complete probe/teardown lifecycle
  - Added BRCMF_SIM_VAL (0x00200000) to debug.h
  - Fixed sim_bus_if.h tx_data ownership doc (shim retains skb, hwsim clones)
- Key probe flow: symbol_get → callbacks → bus alloc → dev_set_drvdata → fw_download → module_param → brcmf_alloc → brcmf_attach
- Teardown: shutting_down → detach → brcmf_detach → brcmf_free → release_module_param → symbol_put
- Integrated DR-02 (stop mandatory), DR-06 (detach+free in teardown)

### Phase 17: Fault Injection — COMPLETE
- **Status:** done (committed as c4215b3d7)
- Actions taken:
  - Added debugfs dir `/sys/kernel/debug/brcmfmac_hwsim/`
  - 5 writable fault injection knobs + 4 read-only counters + state file
  - Hooked fault injection into tx_ctl, tx_data, scan_work, connect_work
  - Fixed TX skb ownership: hwsim no longer frees original skb (shim retains)
  - Added `#include <linux/debugfs.h>` and `<linux/seq_file.h>`

### Phase 18: Test Suite — COMPLETE
- **Status:** done (committed as 487ceef9c)
- Actions taken:
  - Created 3-tier test suite in hwsim/tests/run_tests.sh
  - 23 test cases: smoke (T01-T07), integration (T08-T16), stress (T17-T23)
  - Tests module load/unload, interface creation, MAC validation, debugfs, wpa_supplicant scan, fault injection, KASAN/oops checks

### Handoff State — Milestone 1 COMPLETE
- **Current goal:** Milestone 1 (SDIO+BCDC hwsim) IMPLEMENTATION DONE
- **Total commits:** 10 on hwsim-dev (1 init + 9 feature)
- **Total new code:** ~2,388 lines across 11 files
- **What is done:**
  - All phases 11.5–18 completed and committed
  - bus type enum, Kconfig, Makefile, bus.h, core.c hooks
  - sim_bus_if.h IPC interface
  - hwsim_core.c virtual firmware module (1244 lines)
  - sim_sdio.c bus shim (459 lines)
  - debugfs fault injection
  - 3-tier integration test suite
- **What remains:**
  - ⚠ Compile validation on x86_64 NUC (cannot build on macOS arm64)
  - ⚠ Runtime testing on actual Linux machine
  - Phase 16 (PCIe+MSGBUF): Deferred to Milestone 2
- **Blockers:**
  - Need Intel NUC with Linux 6.12.81 kernel source to build & test

---

## 2026-04-21 Phase 19: arm64 VM 編譯驗證 ✅

**目標**：在 Mac M3 Ultra + OrbStack + Ubuntu 24.04 arm64 VM 上編譯
Linux kernel 6.12.81 的 brcmfmac 模組樹，驗證 hwsim 與 sim_sdio.c 程式碼
可以被 kernel build system 接受。

### 環境
- 主機：Mac Studio M3 Ultra, macOS 26.3, 256GB UMA
- VM：OrbStack + Ubuntu 24.04 arm64 (orb machine "hwsim-test")
- 工具鏈：gcc-13 (Ubuntu), kbuild 6.12.81, ARCH=arm64
- Kconfig：`CONFIG_BRCMFMAC=m`, `CONFIG_BRCMFMAC_HWSIM_SDIO=y`,
  `CONFIG_BRCMFMAC_SDIO/USB/PCIE` 全部關閉

### 編譯結果
**所有 .o/.ko 都產生成功，我們的程式碼 0 warnings、0 errors**：
- `drivers/.../brcmfmac/hwsim/hwsim_core.o` ✅
- `drivers/.../brcmfmac/hwsim/brcmfmac_hwsim.o` ✅ (102KB, export `brcmf_hwsim_get_ops`)
- `drivers/.../brcmfmac/sim_sdio.o` ✅
- 剩餘 modpost 錯誤（dmi/rtnl/cfg80211 等 unresolved）只是因為沒有完整 kernel
  build 產生 Module.symvers，非程式碼問題。

### 抓到並修復的 5 個真實 Bug (commit 3b4728d68)
1. 缺 `#include "core.h"` — fwil.h 的 static inline 會把 `struct brcmf_if` 視為新的 forward declaration，造成 incompatible-pointer-type
2. `ver.epi_ver.epi_major` 不存在 — 該 struct 是 flat 的，應用 `epi_ver_major` 直接取存
3. `hwsim_build_response()` 呼叫少給一個 `bool error` 引數
4. `debugfs_create_s32()` 在 kernel 6.12 不存在，改用 u32 + cast
5. `%d` 格式字串對 u32，改成 `%u`

### 關鍵啟示
- arm64 編譯就能驗證 90%+ 的程式碼正確性，架構無關
- 若要跑 runtime（載入模組、起 wlan0、跑 wpa_supplicant），OrbStack
  不適合（它用自己的 kernel）。下一步需要 UTM + 自 build 的 6.12.81 kernel，
  或直接轉到 Intel NUC。

### 檔案變更
- `drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim/hwsim_core.c` (+11 −9)

### 下一步
選項 A：在 OrbStack VM 完整 `make bindeb-pkg` 產生 kernel .deb，
並轉到 UTM/QEMU 開機測試載入模組。
選項 B：打包目前 hwsim-dev 分支，搬到 Intel NUC 做 x86_64 完整測試。
選項 C：先進入 Milestone 2（PCIe + MSGBUF shim）設計與實作。


## Phase 20 update — runtime validation breakthrough (session resume)

**QEMU boot**: 6.12.81-hwsim kernel now default via `/boot/vmlinuz` symlink + `grub-reboot`. SSH works on port 2222.

**First module load attempt**: HUNG. Root cause: `sim_sdio_probe` ran synchronously from `brcmfmac_module_init` (platform_device was added in-line, triggering probe while module_mutex was still held). Probe called `__request_module("brcmfmac-wcc")`, child modprobe blocked on module_mutex → classic kmod deadlock.

**Fix** (sim_sdio.c):
- Defer `platform_device_add()` to a work queue (`sim_pdev_add_work`) scheduled inside `brcmf_sim_sdio_register()` after `platform_driver_register()`
- `brcmf_sim_sdio_exit()` now `flush_work()` first
- `sim_driver_registered` flag to guard against partial init failures

**Second load attempt**: probe advances all the way through:
1. brcmfmac_hwsim ops registered ✓
2. Firmware version reported: `BCM65535/0 wl0: Oct 01 2024 brcmfmac-hwsim-1.0`
3. `brcmfmac-wcc` vendor plugin auto-loaded ✓
4. `brcmf_c_preinit_dcmds` succeeded
5. **FAILS** at `brcmf_cfg80211_attach: Failed to get D11 version (-52 ENOMSG)`

**Next bug to squash**: Our BCDC ioctl table in hwsim_core.c lacks the iovar/dcmd that `brcmf_cfg80211_attach` issues for D11 revision. Need to check brcmf_cfg80211_attach source to identify which dcmd/iovar, add handler returning synthetic D11 rev (e.g., 0x29 for 11ac/ax-class chips).

## Session Checkpoint — Milestone 2 kickoff (2026-04-21)
- Milestone 1 fully validated: wlan0 up, wpa_supplicant cycle, dmesg 0/0.
- User-approved scope: **option 3** — full hostapd+wpa_supplicant end-to-end over mock bus, with AP/STA inter-bsscfg data loopback.
- M2 phase map added to task_plan.md (A..E). Handover protocol formally written.
- Next exact action: **Phase M2-A** — implement `interface_create` v1 GET iovar handler in hwsim_core.c + emit `WLC_E_IF` ADD event so `iw dev wlan0 interface add wlan1 type __ap` creates wlan1.
- Current dmesg blocker (observed): `brcmf_cfg80211_request_ap_if: failed to create interface(v1), err=-52` (BCME_UNSUPPORTED).
- Handoff state: code = commit f7b675d90 on hwsim-dev; repo pushed at 5578e55 on origin/main.

## Session Handover — 2026-04-21 M2-A partial + VM hang
- Goal: Milestone 2 Phase A — make `iw dev wlan0 interface add wlan1 type __ap` create wlan1 netdev.
- Phase status: M2-A in_progress (blocked by VM hang).
- Done this session:
  - Planned M2 (phases A-E) in task_plan.md with exit criteria + handover protocol.
  - Implemented `interface_create` v1 iovar GET handler in hwsim_core.c (20-byte wl_interface_create_v1 layout).
  - Implemented `hwsim_if_add_work_fn` workqueue emitting BRCMF_E_IF action=IF_ADD role=AP.
  - Fixed stale-module bug in test script.
- Exact next action:
  1. Restart VM (pid in vm/qemu.pid).
  2. Audit locking chain hwsim_send_event -> rx_data -> fweh -> brcmf_add_if -> register_netdev (rtnl_lock) vs what's held on the tx_ctl thread.
  3. Bump if_add_work delay to >=50ms.
  4. Re-test interface add.
- Files changed (uncommitted): linux-6.12.81/.../brcmfmac/hwsim/hwsim_core.c (interface_create handler, hwsim_if_add_work_fn, hwsim_dev state fields, pr_info debug).
- Git: hwsim-dev @ f7b675d90 (M1 final). overlay repo @ origin/main 5578e55. Current M2 work NOT yet committed.
- Validation: module built, contains "interface_create"/"GET iovar" strings. Runtime: hang -> inconclusive functional state.
- 3-strike status: 1 failure (VM hang). Not retried same code path — suspended for analysis.

## M2-A ✅ COMPLETE — 2026-04-21
- Root cause of VM hang: recursive mutex_lock on dev->lock inside interface_create handler. hwsim_tx_ctl already holds dev->lock at line 1102; the handler at line 691 tried to re-lock the non-recursive mutex => self-deadlock => kernel hung on the cfg80211 caller thread (which was holding rtnl).
- Fix: removed redundant mutex_lock/unlock in interface_create handler (caller already serializes); also removed two debug pr_info lines.
- Validation:
  - `iw dev wlan0 interface add wlan1 type __ap` => rc=0
  - `ip -br link` shows wlan1 with MAC 02:00:00:e8:53:00
  - `ip link set wlan1 up` => OK, no dmesg noise
- Lock-chain audit (sub-agent finding) confirmed: cfg80211 thread holds rtnl+wiphy while waiting; brcmf_fweh_event_worker on system_wq only wakes vif_wq via brcmf_notify_vif_event (no rtnl needed); register_netdev runs back in cfg80211 thread context (rtnl already held). Architecture is sound — only the self-deadlock blocked it.
- Files changed: hwsim_core.c (interface_create handler cleanup, GET-iovar debug print removed).
- Phase status: M2-A done. Ready to proceed to M2-B (hostapd on wlan1 — needs bss/ssid/bcn_prb/wsec iovars + WLC_SET_SSID/UP/AP dcmds + WLC_E_LINK).

# Findings & Decisions

## Requirements
- Use the `planning-with-files` skill and follow its instructions.
- Create `task_plan.md`, `findings.md`, and `progress.md` in the project root.
- Treat these files as durable working memory for future long-session work.
- Keep Copilot repo-level planning assets under `.github/hooks/` and `.github/skills/planning-with-files/`.
- Use top of the main branch of `wpa_supplicant` from the local `hostap` tree.
- Study how `wpa_supplicant`/`hostapd` use `mac80211_hwsim` for simulation and automated testing.
- Study Linux kernel `6.12.81`, especially `drivers/net/wireless/broadcom/brcm80211/brcmfmac`.
- Understand tx/rx over PCIe/USB/SDIO, PCIe and SDIO interrupt handling, CDC and MSGBUF formats, ioctl/iovar paths, and firmware event handling.
- Output an architecture design and an implementation plan for a simulated bus driver that can exercise `brcmfmac`.

## Research Findings
- The skill requires checking for existing planning files and running session catchup before deeper work.
- No existing `task_plan.md`, `findings.md`, or `progress.md` were present in the project root when initialized.
- Session catchup produced no unsynced context at initialization time.
- The skill's security boundary says web/search output should go to `findings.md`, not `task_plan.md`.
- The local `hostap` tree is on branch `main` at revision `e3c78ad5e`.
- The local kernel tree `linux-6.12.81` contains `drivers/net/wireless/broadcom/brcm80211/brcmfmac`.
- The `hostap/tests/hwsim/` tree is extensive and includes Python orchestration, monitor helpers, and per-feature test modules.
- `hostap/tests/hwsim/README` describes the baseline lab: five simulated radios, three `wpa_supplicant` processes, one `hostapd` process controlling two radios, and `wlantest` attached to `hwsim0` to inspect all exchanged frames.
- `hostap/tests/hwsim/README` says the test scripts drive `wpa_supplicant` and `hostapd` through their control interfaces and use `wlantest_cli` plus data-plane checks to verify behavior.
- `hostap/tests/hwsim/hwsim.py` provides a generic netlink controller for family `MAC80211_HWSIM` and can create/destroy radios dynamically using `HWSIM_CMD_CREATE_RADIO` and `HWSIM_CMD_DESTROY_RADIO`.
- `hostap/tests/hwsim/run-all.sh` is the batch orchestrator: optional build, `start.sh` bring-up, `run-tests.py` execution, then `stop.sh` teardown and log capture.
- `hostap/tests/hwsim/start.sh` loads `mac80211_hwsim`, brings up `hwsim0`, starts `wlantest`, launches multiple `wpa_supplicant` instances on `wlan0..2` plus extra global instances, and starts `hostapd` plus the auth server.
- `hostap/tests/hwsim/run-tests.py` imports all `test_*.py` modules, resets device state between tests, and wraps `WpaSupplicant`, `HostapdGlobal`, and `Wlantest` to drive scenarios and collect artifacts.
- `hostap/tests/hwsim/hostapd.py` shows the harness controls hostapd through global and per-interface control sockets, dynamically adding/removing interfaces and BSSes.
- `hostap/tests/hwsim/wlantest.py` wraps `wlantest_cli` and uses monitor captures to assert frame-level and RSN/PMF/TID properties.
- The background hwsim study confirms `run-tests.py` creates three `WpaSupplicant` fixtures for `wlan0..2`, uses `apdev` entries for `wlan3` and `wlan4`, and calls `reset_devs()` before each test case.
- The background hwsim study confirms `start.sh` loads `mac80211_hwsim` with `radios=7`, uses `hwsim0` as the monitor capture device, and drives the real Wi-Fi workflows entirely through nl80211 plus control sockets rather than any Broadcom-specific transport.
- The background hwsim study confirms the typical per-test flow is `hostapd.add_ap()` -> `dev[i].connect()` -> data-path verification helpers such as `hwsim_utils.test_connectivity()` -> cleanup.
- In `brcmfmac`, `bus.h` defines the minimum bus contract seen by the common/proto layers: `txdata`, `txctl`, `rxctl`, optional tx queue access, optional reset/wowl/memdump/blob hooks, and a bus state machine (`BRCMF_BUS_DOWN` / `BRCMF_BUS_UP`).
- `proto.c` selects protocol implementation purely from `bus_if->proto_type`: `BRCMF_PROTO_BCDC` or `BRCMF_PROTO_MSGBUF`.
- `fwil.c` is the firmware interface layer; high-level `brcmf_fil_cmd_*` and `brcmf_fil_iovar_*` helpers serialize through `proto_block` and then call proto `query_dcmd` / `set_dcmd`.
- In `bcdc.c`, ioctl/dcmd requests use `struct brcmf_proto_bcdc_dcmd`; request/response matching is done with a request ID embedded in `flags`, sent by `brcmf_bus_txctl()` and completed by `brcmf_bus_rxctl()`.
- In `bcdc.c`, data packets carry a 4-byte BCDC header (`flags`, `priority`, `flags2`, `data_offset`) that encodes protocol version, checksum hints, priority, and interface index.
- In `msgbuf.c`, control/data/event traffic is split by message type and ring: control submit/completion, rxpost submit, tx/rx complete, ioctl request/complete, event buffer post, and flow-ring create/delete/flush.
- In `msgbuf.c`, host DMA ownership is explicit: packet IDs map skbs to DMA addresses, and message headers carry 64-bit buffer addresses (`msgbuf_buf_addr`) plus lengths and request IDs.
- In `msgbuf.c`, ioctl/query/set flow is: copy caller data into the shared `ioctbuf` DMA buffer -> enqueue `MSGBUF_TYPE_IOCTLPTR_REQ` -> wait for `MSGBUF_TYPE_IOCTL_CMPLT` -> recover the response skb by packet ID -> copy the response bytes to the caller.
- In `msgbuf.c`, the host pre-posts separate response/event buffers (`MSGBUF_TYPE_IOCTLRESP_BUF_POST`, `MSGBUF_TYPE_EVENT_BUF_POST`) and RX data buffers (`MSGBUF_TYPE_RXBUF_POST`); firmware later completes them as `MSGBUF_TYPE_IOCTL_CMPLT`, `MSGBUF_TYPE_WL_EVENT`, and `MSGBUF_TYPE_RX_CMPLT`.
- In `msgbuf.c`, `brcmf_proto_msgbuf_rx_trigger()` drains the three D2H completion rings in a fixed order: RX complete, TX complete, then control complete.
- In `pcie.c`, PCIe interrupt handling is mailbox-based: the quick ISR checks `mailboxint`, disables interrupts, and wakes the threaded ISR; the thread acks mailbox status, handles mailbox data, and triggers `brcmf_proto_msgbuf_rx_trigger()` on D2H doorbell interrupts.
- In `pcie.c`, common rings are backed by coherent DMA buffers and registered with callbacks that update read/write pointers and ring the device doorbell.
- In `sdio.c`, the interrupt path is split into ISR + deferred procedure call (DPC): `brcmf_sdio_isr()` marks `ipend`/queues work, and `brcmf_sdio_dpc()` reads/acks interrupt status, handles host mailbox, reads frames, and pushes pending control frames.
- In `sdio.c`, SDIO control writes are asynchronous from the caller's perspective: `brcmf_sdio_bus_txctl()` stores the control frame, triggers DPC, and waits for DPC to transmit and complete it; `brcmf_sdio_bus_rxctl()` waits for the response buffer assembled by the receive path.
- In `usb.c`, USB uses BCDC rather than MSGBUF and maps control/data onto URBs: `brcmf_usb_tx_ctlpkt()` / `brcmf_usb_rx_ctlpkt()` handle control, `brcmf_usb_tx()` handles bulk TX, and `brcmf_usb_rx_complete()` feeds received frames into `brcmf_rx_frame()`.
- In `core.c`, the common RX split is explicit: proto-specific `hdrpull` runs first, then packets go either to `brcmf_fweh_process_skb()` for firmware events or to the normal netdev path for data.
- In `fweh.c`, firmware events are copied into a queue item, deferred to a worker, mapped from firmware event codes to driver event codes, and then dispatched to per-event handlers; interface add/change/delete events are handled specially before generic dispatch.

## Technical Decisions
| Decision | Rationale |
|----------|-----------|
| Initialize planning files manually from templates | Fastest way to satisfy the skill and establish persistent state now |
| Use project-root files instead of session-only notes | The skill expects project-root files to be the main durable memory |
| Keep `task_plan.md` concise and trusted-only | It is re-read by hooks and should not amplify untrusted content |
| Base the research on local source trees first, with web sources only as supporting context | Keeps the design tied to the exact code version requested |
| Treat `bus.h` as the first design boundary for the simulator | It exposes the narrowest stable abstraction between common/proto layers and bus implementations |
| Treat PCIe+MSGBUF and SDIO+BCDC as separate simulation targets over a shared fake-firmware core | Their host-side transport and completion models are materially different |
| Use the hostap hwsim framework as outer orchestration, not as the simulated Broadcom transport itself | The hostap harness is excellent for AP/STA workflow testing, but it does not emulate the proprietary `brcmfmac` bus/protocol contract |
| Put firmware behavior behind a transport-neutral fake-firmware engine | The semantic commands/events should be shared even when the wire format differs between BCDC and MSGBUF |

## Proposed Architecture (v2 — Two-Component Design)
- **AMENDED (v2):** The system consists of TWO components:
  1. **Bus shim inside brcmfmac** — compile-time Kconfig option that replaces real SDIO/PCIe bus layer with a thin forwarding shim
  2. **Separate mocked bus kernel module** (`brcmfmac_hwsim.ko`) — the virtual firmware + hardware bus that receives/processes all packets
- **Kconfig gates:** `CONFIG_BRCMFMAC_HWSIM_SDIO` (selects BCDC) and `CONFIG_BRCMFMAC_HWSIM_PCIE` (selects MSGBUF).
- **When enabled:** Bus shim replaces real SDIO/PCIe stack, probes via platform_device, forwards all traffic to mocked bus module.
- **Mocked bus module responsibilities:**
  - BCDC dcmd parsing + response building (for SDIO path)
  - MSGBUF message parsing + completion building (for PCIe path)
  - IOCTL/iovar command handling (fake firmware brain)
  - Data packet TX→RX conversion (strip TX header, add RX header, route by ifidx)
  - Firmware event generation (BRCMF_E_* events)
  - Interrupt simulation (trigger callbacks to bus shim)
  - Firmware download acceptance
  - Fault injection via debugfs
- **Bus shim responsibilities:**
  - Implement `brcmf_bus_ops` (txdata, txctl, rxctl, etc.)
  - Forward raw frames to mocked bus module — does NOT parse content
  - Receive interrupt callbacks from mocked bus module
  - Manage brcmf_bus lifecycle (state, chip info, proto_type, fwvid)
  - platform_device creation and self-probe
- **Recommended first target:** `SDIO + BCDC` (via `CONFIG_BRCMFMAC_HWSIM_SDIO=y`)
- **Phase-2 target:** `PCIe + MSGBUF` (via `CONFIG_BRCMFMAC_HWSIM_PCIE=y`)
- **Outer test harness:** keep using `hostap/tests/hwsim` for AP/STA orchestration
- **Probe mechanism:** Load `brcmfmac_hwsim.ko` first → load brcmfmac → bus shim registers → platform_device probes → shim connects to mocked bus module → `brcmf_alloc()` + `brcmf_attach()` → wlan0
- **Firmware loading bypass:** Bus shim does NOT call `brcmf_fw_get_firmwares()`. It skips firmware loading entirely.
- **fwvid binding:** Mock sets `bus_if->fwvid = BRCMF_FWVENDOR_WCC`.

## BCDC Wire Format Details (for Mocked Bus Module)
- **BCDC dcmd (ioctl):** `struct brcmf_proto_bcdc_dcmd` = {cmd(le32), len(le32), flags(le32), status(le32)} + payload
  - flags: bit 1 = SET (0=GET), bits 12-15 = ifidx, bits 16-31 = request ID
  - Mocked bus must parse cmd/reqid/ifidx, produce response with SAME reqid, set status=0 (or error)
- **BCDC data header:** `struct brcmf_proto_bcdc_header` = {flags(u8), priority(u8), flags2(u8), data_offset(u8)}
  - flags: bits 4-7 = proto version (2), bit 2 = checksum good
  - flags2: bits 0-3 = interface index
  - Mocked bus: strip TX header, re-wrap as RX header with target ifidx
- **BCDC dcmd flow:** txctl sends dcmd buffer → mocked bus parses → produces response → triggers interrupt → rxctl returns response
- **BCDC data flow:** txdata sends skb with BCDC header → mocked bus strips header → routes → adds RX BCDC header → triggers interrupt → brcmf_rx_frame()

## SDIO DPC/Interrupt Details (for Bus Shim Design)
- Real SDIO: `brcmf_sdio_isr()` sets `ipend`, queues `datawork` → `brcmf_sdio_dpc()` reads interrupt status, acks, reads frames, pushes ctrl
- Mock SDIO shim: mocked bus module calls callback → shim stores data → triggers workqueue → work function calls `brcmf_rx_frame()` or wakes `ctrl_wait`
- For `txctl`: shim forwards buffer to mocked bus → mocked bus processes → calls shim's "ctrl response ready" callback → shim wakes `rxctl` waiter
- For `txdata`: shim forwards skb to mocked bus → mocked bus processes → calls shim's "rx data available" callback → shim calls `brcmf_rx_frame()`

## PCIe Interrupt Details (for Bus Shim Design)
- Real PCIe: quick ISR checks `mailboxint` → threaded ISR acks, handles mailbox, calls `brcmf_proto_msgbuf_rx_trigger()` on D2H doorbell
- Mock PCIe shim: mocked bus module fills D2H completion ring entries → triggers callback → shim calls `brcmf_proto_msgbuf_rx_trigger()`
- Ring simulation: the mocked bus module must maintain simulated D2H ring buffers that the msgbuf layer can read

## Build System Findings (Phase 11)
- **Kconfig structure:** Bus backends (`CONFIG_BRCMFMAC_SDIO`, `_USB`, `_PCIE`) are bool options that each `select` the appropriate proto (`BRCMFMAC_PROTO_BCDC` or `BRCMFMAC_PROTO_MSGBUF`). The new hwsim configs follow the same pattern.
- **Makefile structure:** Object files are conditionally compiled via `brcmfmac-$(CONFIG_BRCMFMAC_SDIO) += sdio.o bcmsdh.o`. The new hwsim configs add `sim_fw.o`, `sim_sdio.o`, `sim_pcie.o` similarly.
- **Registration pattern:** In `bus.h`, each bus has `brcmf_xxx_register()` / `brcmf_xxx_exit()` with `#ifdef` guards and inline stubs. `core.c:brcmf_core_init()` calls all three register functions sequentially. The hwsim follows the same pattern.
- **bus_priv union:** `struct brcmf_bus::bus_priv` is a union of `{sdio, usb, pcie}`. Adding `sim` member requires a new pointer type `struct brcmf_sim_dev *`.
- **fwvid system:** Three vendors (WCC, CYW, BCA). When built-in (`CONFIG_BRCMFMAC=y`), vops are directly linked. The mock needs `fwvid` set before `brcmf_attach()` calls `brcmf_fwvid_attach()`.

## Probe Path Findings (Phase 11)
### Real SDIO Probe (for comparison)
```
sdio_register_driver(&brcmf_sdmmc_driver)  ← brcmf_sdio_register()
  → MMC subsystem discovers SDIO func → brcmf_ops_sdio_probe()
    → brcmf_sdio_probe(sdiodev)
      → brcmf_sdio_probe_attach() [talks to real chip registers]
      → brcmf_sdio_prepare_fw_request()
      → brcmf_fw_get_firmwares(callback=brcmf_sdio_firmware_callback)
        → [async: fw loaded from /lib/firmware/brcm/]
        → brcmf_sdio_firmware_callback()
          → download fw to chip → enable F2 → register interrupts
          → bus_if->ops = &brcmf_sdio_bus_ops
          → brcmf_alloc(dev, settings)
          → brcmf_attach(dev)
            → brcmf_fwvid_attach() → brcmf_proto_attach(BCDC) → brcmf_fweh_attach()
            → brcmf_bus_started()
              → brcmf_add_if(0, "wlan%d") ← wlan0 created
              → brcmf_bus_change_state(BRCMF_BUS_UP)
              → brcmf_c_preinit_dcmds() ← ioctl/iovar queries to firmware
              → brcmf_net_attach() ← netdev registered
```
### Mock SDIO Probe (designed)
```
platform_driver_register(&brcmf_sim_sdio_driver)  ← brcmf_sim_sdio_register()
  → platform_device_register(&brcmf_sim_sdio_pdev)
    → brcmf_sim_sdio_probe(pdev)
      → alloc sim_dev + brcmf_bus + brcmf_mp_device
      → bus_if->ops = &brcmf_sim_sdio_bus_ops
      → bus_if->proto_type = BRCMF_PROTO_BCDC
      → bus_if->fwvid = BRCMF_FWVENDOR_WCC
      → bus_if->chip = 0x4339; bus_if->chiprev = 2
      → bus_if->maxctl = 8192
      → dev_set_drvdata(&pdev->dev, bus_if)
      → [NO firmware loading]
      → brcmf_alloc(&pdev->dev, settings)
      → brcmf_attach(&pdev->dev)
        → brcmf_fwvid_attach() [binds WCC vops]
        → brcmf_proto_attach() [selects BCDC]
        → brcmf_fweh_attach()
        → brcmf_bus_started()
          → brcmf_add_if(0, "wlan%d") ← wlan0 created
          → brcmf_c_preinit_dcmds()
            → All ioctls go through sim_sdio_bus_txctl()/rxctl()
            → sim_fw handles each command and returns appropriate data
          → brcmf_net_attach()
```

### What brcmf_c_preinit_dcmds() Needs From Fake Firmware
| Order | Call | What sim_fw must return |
|-------|------|------------------------|
| 1 | `brcmf_fil_iovar_data_get("cur_etheraddr")` | 6-byte MAC address |
| 2 | `brcmf_fil_cmd_data_get(BRCMF_C_GET_REVINFO)` | `struct brcmf_rev_info_le` with valid vendorid, deviceid, chipnum, chiprev, etc. |
| 3 | `brcmf_c_process_clm_blob()` → `brcmf_bus_get_blob(BRCMF_BLOB_CLM)` | No blob (return -ENOENT or NULL; CLM processing tolerates absence) |
| 4 | `brcmf_fil_iovar_data_get("ver")` | Firmware version string (e.g., "wl0: Oct 2024 version 7.45.229\n") |
| 5 | `brcmf_fil_iovar_data_get("clmver")` | CLM version string |
| 6 | `brcmf_fil_iovar_int_set("mpc", 1)` | Success (0) |
| 7 | `brcmf_fil_iovar_data_get("event_msgs")` | Event mask buffer (all zeros or preconfigured) |
| 8 | `brcmf_fil_iovar_data_set("event_msgs")` | Success (0) |
| 9 | `brcmf_fil_cmd_int_set(BRCMF_C_SET_SCAN_CHANNEL_TIME)` | Success (0) |
| 10 | `brcmf_fil_cmd_int_set(BRCMF_C_SET_SCAN_UNASSOC_TIME)` | Success (0) |
| 11+ | `brcmf_feat_attach()` → various `brcmf_fil_iovar_int_get()` | Feature-specific responses or -ENOTSUP |

## Implementation Plan Summary
1. Add **Kconfig and Makefile entries** for `CONFIG_BRCMFMAC_HWSIM_SDIO` and `CONFIG_BRCMFMAC_HWSIM_PCIE`.
2. Modify **bus.h** and **core.c** to register/deregister the mock bus backends.
3. Build a **transport-neutral fake-firmware engine** (`sim_fw.c`) with deterministic command handling and event generation.
4. Implement an **SDIO+BCDC mock bus** (`sim_sdio.c`) that uses `platform_device`, implements `brcmf_bus_ops`, routes control through `sim_fw`, and triggers wlan0 creation.
5. Add **debug hooks** to inject expected and unexpected firmware statuses, timeouts, malformed lengths, duplicate IDs, and spurious event codes.
6. Extend to **PCIe+MSGBUF mock bus** (`sim_pcie.c`) after the fake-firmware semantics are stable, reusing the same command/event engine but adding ring simulation.
7. **Validate** by building with each config, verifying wlan0 creation, and testing with wpa_supplicant/hostapd attachment.

## Issues Encountered
| Issue | Resolution |
|-------|------------|
| No prior planning files existed | Created fresh project-root planning files |
| None yet for the new research task | Pending deeper code study |

## Resources
- Skill root: `.github/skills/planning-with-files/`
- Skill template: `.github/skills/planning-with-files/templates/task_plan.md`
- Skill template: `.github/skills/planning-with-files/templates/findings.md`
- Skill template: `.github/skills/planning-with-files/templates/progress.md`
- Session catchup script: `.github/skills/planning-with-files/scripts/session-catchup.py`
- `hostap/tests/hwsim/README`
- `hostap/tests/hwsim/hwsim.py`
- `hostap/tests/hwsim/run-all.sh`
- `hostap/tests/hwsim/start.sh`
- `hostap/tests/hwsim/run-tests.py`
- `hostap/tests/hwsim/hostapd.py`
- `hostap/tests/hwsim/hwsim_utils.py`
- `hostap/tests/hwsim/wlantest.py`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/bus.h`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/proto.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/fwil.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/fweh.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/bcdc.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/msgbuf.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/msgbuf.h`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/sdio.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/usb.c`
- `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/`

## Visual/Browser Findings
## Firmware Loading Path Research (v5.1 update)

### SDIO Firmware Loading 完整流程
1. `brcmf_sdio_probe()` → `brcmf_sdio_prepare_fw_request()` 建立 firmware 請求
   - 使用 `brcmf_sdio_fwnames[]` mapping table 查找 chip ID → firmware 名稱
   - 名稱格式: `brcmfmac<chip>-sdio.{bin,txt,clm_blob}`
   - CLM blob 標記為 `BRCMF_FW_REQF_OPTIONAL`
2. `brcmf_fw_get_firmwares()` → `request_firmware_nowait()` 從 `/lib/firmware/brcm/` 載入
3. `brcmf_sdio_firmware_callback()`:
   - 取 firmware binary + NVRAM + CLM blob
   - `brcmf_sdio_download_firmware()` 寫入晶片 (SDIO register writes)
   - 存 CLM: `sdiod->clm_fw = clm_blob`
   - 設定硬體 (clock, interrupt mask, F2 watermark)
   - `brcmf_alloc()` + `brcmf_attach()`
4. 後續 `brcmf_c_preinit_dcmds()` 處理 blobs:
   - `brcmf_c_process_clm_blob()` → `get_blob(BRCMF_BLOB_CLM)` → 下載到 firmware
   - `brcmf_c_process_txcap_blob()` → `get_blob(BRCMF_BLOB_TXCAP)` → SDIO 返回 `-ENOENT`
   - `brcmf_c_process_cal_blob()` → 使用 `settings->cal_blob` (通常 NULL)

### Blob Graceful Degradation
- CLM: 回傳 error 時 → `"no clm_blob available"` → `return 0` (繼續，channel 受限)
- TxCap: 回傳 error → `"no txcap_blob available"` → `return 0`
- Cal: `settings->cal_blob == NULL` → 直接 `return 0`
- **結論: 所有 blob 缺失都不會 fatal error，driver 正常啟動**

### HWSIM Firmware 策略
- Milestone 1: 完全繞過 `brcmf_fw_get_firmwares()`
- `fw_download(ctx, NULL, 0, NULL, 0)` 通知 hwsim module → 設定 "fw booted" 狀態
- `get_blob()` 返回 `-ENOENT` → kernel graceful skip
- 不需要任何 firmware/NVRAM/CLM 檔案
- hwsim module 內嵌回應 `"ver"` 和 `"clmver"` iovar

---

## Design Review: Plan v5.2 — Architecture & Integration (2026-04-15)

### Methodology
Performed a code-level design review of plan v5.2 by reading every referenced kernel source file in `linux-6.12.81/drivers/net/wireless/broadcom/brcm80211/brcmfmac/`. Each finding is verified against specific source lines.

---

### 🔴 DR-01: Wrong TX completion function name
**Category:** Interface
**Evidence:** bus.h:78 comment says "brcmf_txcomplete()" but no such function exists. The real function is `brcmf_proto_bcdc_txcomplete(dev, skb, success)` at bcdc.c:364, declared in bcdc.h:12. SDIO calls it at sdio.c:2324.
**Issue:** The plan states "shim MUST call `brcmf_txcomplete()` after txdata". This function does not exist. Using this name will cause a compile error.
**Fix:** Replace all references to `brcmf_txcomplete()` with `brcmf_proto_bcdc_txcomplete(struct device *dev, struct sk_buff *txp, bool success)`. The shim's txdata op must call this after forwarding the skb to hwsim (or on failure). Include `bcdc.h` in the shim.

---

### 🔴 DR-02: Mandatory bus_ops not fully enumerated — `stop` will crash
**Category:** Integration
**Evidence:** bus.h:199-201:
```c
static inline void brcmf_bus_stop(struct brcmf_bus *bus)
{
    bus->ops->stop(bus->dev);
}
```
No NULL check. Called unconditionally from `brcmf_detach()` at core.c:1448.
**Issue:** The plan does not explicitly list which `brcmf_bus_ops` members the sim must implement. `stop` is mandatory (no NULL guard) but is not mentioned in the plan. If omitted, module unload or any error teardown will dereference NULL and panic.
**Fix:** Add an explicit bus_ops table to the plan:
- **Mandatory** (no NULL check, crash if missing): `stop`, `txdata`, `txctl`, `rxctl`, `get_blob`
- **Optional** (NULL-checked): `preinit`, `gettxq`, `wowl_config`, `get_ramsize`, `get_memdump`, `debugfs_create`, `reset`
- **Semi-optional**: `remove` (falls back to `device_release_driver(bus->dev)` if NULL, per bus.h:281)

---

### 🔴 DR-03: maxctl=8192 is wrong — BCDC unconditionally overwrites it
**Category:** Interface
**Evidence:** bcdc.c:474-475 in `brcmf_proto_bcdc_attach()`:
```c
drvr->bus_if->maxctl = BRCMF_DCMD_MAXLEN +
        sizeof(struct brcmf_proto_bcdc_dcmd) + ROUND_UP_MARGIN;
```
= 8192 (core.h:32) + 16 (struct size) + 2048 (bcdc.c:90) = **10256 bytes**.
This runs inside `brcmf_attach()` → `brcmf_proto_attach()` → `brcmf_proto_bcdc_attach()`, which executes AFTER the shim sets maxctl=8192 and BEFORE `brcmf_bus_started()` calls `preinit`.
**Issue:** The plan states "maxctl: 8192 (same as real SDIO)" but this value is immediately overwritten to 10256 by BCDC and never used. If any shim code (e.g., rxctl buffer allocation) relies on the 8192 value, it will be undersized. The real SDIO preinit (sdio.c:3537) reads the BCDC-set value (10256), not the bus-set value.
**Fix:** Remove "maxctl: 8192" from the plan. Document that maxctl is set by `brcmf_proto_bcdc_attach()` to 10256 and the shim should NOT pre-set or rely on it. The shim's rxctl buffer must accommodate up to `bus_if->maxctl` bytes, read AFTER `brcmf_attach()` returns (or simply use the response length from hwsim).

---

### 🟡 DR-04: Kconfig must `select BRCMFMAC_PROTO_BCDC`
**Category:** Integration
**Evidence:** Kconfig:17-21 — `BRCMFMAC_SDIO` selects `BRCMFMAC_PROTO_BCDC`. Makefile:29-31 — `brcmfmac-$(CONFIG_BRCMFMAC_PROTO_BCDC) += bcdc.o fwsignal.o`.
**Issue:** If `BRCMFMAC_HWSIM_SDIO` is mutually exclusive with `BRCMFMAC_SDIO` (per plan), and only `BRCMFMAC_SDIO` selects `BRCMFMAC_PROTO_BCDC`, then when HWSIM_SDIO is enabled and SDIO is disabled, `bcdc.c` and `fwsignal.c` will NOT be compiled. The sim's BCDC protocol path will be entirely missing. `brcmf_proto_attach()` will fail with "Unsupported proto type" (proto.c:39).
**Fix:** The `BRCMFMAC_HWSIM_SDIO` Kconfig entry MUST include `select BRCMFMAC_PROTO_BCDC`. Also consider `select FW_LOADER` if any firmware infrastructure is still referenced (though it may not be needed since fw_download is bypassed).

---

### 🟡 DR-05: `dev_set_drvdata(dev, bus_if)` not in probe flow
**Category:** Integration
**Evidence:** core.c:1318 in `brcmf_alloc()`: `drvr->bus_if = dev_get_drvdata(dev);`. Every brcmfmac core function retrieves `bus_if` from drvdata: `brcmf_attach()` (core.c:1327), `brcmf_rx_frame()` (core.c:501), `brcmf_detach()` (core.c:1426), etc. SDIO sets it at bcmsdh.c:1083: `dev_set_drvdata(&func->dev, bus_if);`.
**Issue:** The plan's probe flow does not mention `dev_set_drvdata(&pdev->dev, bus_if)`. Without this, `brcmf_alloc()` will get `bus_if = NULL`, and all subsequent dereferences will crash.
**Fix:** Add explicit `dev_set_drvdata(&pdev->dev, bus_if)` step to the probe flow, BEFORE calling `brcmf_alloc()`. This is a critical invariant of the brcmfmac core API.

---

### 🟡 DR-06: Teardown missing `brcmf_detach()` + `brcmf_free()` calls
**Category:** Integration
**Evidence:** SDIO cleanup at sdio.c:4568-4569: `brcmf_detach(bus->sdiodev->dev); brcmf_free(bus->sdiodev->dev);`. PCIe at pcie.c:2546-2547: same pattern. `brcmf_detach()` handles interface removal, bus_stop, fweh/proto/fwvid detach. `brcmf_free()` frees wiphy and cfg80211_ops.
**Issue:** The plan's teardown protocol (§Teardown Protocol) lists: (1) shutting_down=true, (2) ops->detach(), (3) flush workqueues, (4) symbol_put(). But it omits the essential `brcmf_detach(dev)` and `brcmf_free(dev)` calls that every real bus driver makes. Without these, net interfaces are leaked, cfg80211 is leaked, and wiphy is leaked.
**Fix:** The sim shim's remove/cleanup function must call:
1. `brcmf_detach(dev)` — tears down driver core (calls bus_stop, removes interfaces)
2. `brcmf_free(dev)` — frees wiphy
3. Then hwsim cleanup: `ops->detach()`, flush workqueues, `symbol_put()`
4. Then free `bus_if` and `sim_dev` structs

---

### 🟡 DR-07: Event frame format requirements critically under-specified
**Category:** Interface
**Evidence:** fweh.h:361-394 `brcmf_fweh_process_skb()` validates:
1. `skb->protocol == cpu_to_be16(ETH_P_LINK_CTL)` (0x886c) — set by `eth_type_trans()` from ETH header
2. `skb_mac_header(skb)` → must point to `struct brcmf_event` (ETH + brcm_ethhdr + event_msg_be)
3. `brcm_ethhdr.oui == BRCM_OUI` ("\x00\x10\x18")
4. `brcm_ethhdr.usr_subtype == cpu_to_be16(BCMILCP_BCM_SUBTYPE_EVENT)` (= 1)
5. `event_msg_be` fields in big-endian (`event_type`, `status`, `flags`, etc.)

Additionally, since events arrive through `brcmf_rx_frame()` which calls `brcmf_rx_hdrpull()` → `brcmf_proto_bcdc_hdrpull()` (bcdc.c:281), the skb MUST be prepended with a 4-byte BCDC header where `(flags >> 4) & 0xf == BCDC_PROTO_VER` (= 2, per bcdc.c:48). Without this, BCDC hdrpull returns `-EBADE` and the event is silently dropped.
**Issue:** The plan mentions events at a high level ("workqueue → BRCMF_E_ESCAN_RESULT") but doesn't specify the exact wire format. The hwsim module must construct frames as: `BCDC_header(4B)` + `ethhdr(14B, h_proto=0x886c)` + `brcm_ethhdr(10B, OUI + subtype)` + `brcmf_event_msg_be(~58B)` + `optional_data`. This is a ~86-byte minimum frame with very specific byte-level layout. Missing any field silently drops the event with no error.
**Fix:** Add a `sim_event.h` helper or `brcmf_hwsim_build_event()` function specification to the plan that constructs properly formatted event skbs. Document the exact byte layout in sim_bus_if.h or a companion document.

---

### 🟡 DR-08: IPC response buffer lifecycle needs clarification
**Category:** Interface
**Evidence:** BCDC flow: `brcmf_proto_bcdc_msg()` → `bus->txctl(dev, &bcdc->msg, len)` (sends command), then `brcmf_proto_bcdc_cmplt()` → `bus->rxctl(dev, &bcdc->msg, len)` (reads response into SAME buffer). SDIO's rxctl (sdio.c:3262) copies from `bus->rxctl` buffer into the caller's `msg` buffer.
**Issue:** The plan's IPC has `ops->tx_ctl(ctx, msg, len)` to send and `ops->rx_ctl(ctx, msg, len)` to receive, with `cb->rx_ctl_ready(ctx)` as a signal. But the plan doesn't specify:
- Where does hwsim store the response between generating it and the shim calling `ops->rx_ctl`?
- What is the maximum response size? (BCDC allocates BRCMF_DCMD_MAXLEN = 8192 bytes)
- Is `ops->rx_ctl` a synchronous copy or an async operation?
- Can hwsim's response be overwritten if a new command arrives before rx_ctl is called? (No — proto_block mutex serializes, but this should be documented)
**Fix:** Document in sim_bus_if.h that: (a) hwsim internally buffers the response after processing tx_ctl, (b) rx_ctl is a synchronous copy from hwsim's internal buffer into the provided msg buffer, (c) the buffer must be at least BRCMF_DCMD_MAXLEN bytes, (d) the proto_block mutex ensures only one command/response pair is in flight at a time.

---

### 🟡 DR-09: `fwvid_attach` can block/fail without pre-loaded WCC module
**Category:** Integration
**Evidence:** fwvid.c:60-84 in `brcmf_fwvid_request_module()`: unlocks mutex, calls `request_module("brcmfmac-wcc")`, then `wait_for_completion_interruptible()`. If `request_module` fails (e.g., module not on disk), the probe fails.
**Issue:** The plan correctly specifies "insmod brcmfmac-wcc.ko" before brcmfmac, but doesn't document this as a hard dependency. If the load order is wrong, `brcmf_fwvid_attach()` will attempt auto-loading. If auto-loading fails (common in test environments), the probe fails with no clear error message.
**Fix:** Document brcmfmac-wcc.ko as a **mandatory prerequisite** in the module load sequence. Consider adding a check in the sim shim probe that verifies WCC is loaded before proceeding, with a clear error message.

---

### 🟢 DR-10: chip=0xFFFF produces confusing "BCM65535/0" in logs
**Category:** Kernel Practice
**Evidence:** chip.c:495-501:
```c
fmt = ((id > 0xa000) || (id < 0x4000)) ? "BCM%d/%u" : "BCM%x/%u";
snprintf(buf, len, fmt, id, rev);
```
With id=0xFFFF (> 0xa000), format is `"BCM%d/%u"` → "BCM65535/0".
Also, feature.c:298-328 has chip-specific logic:
```c
if (drvr->bus_if->chip != BRCM_CC_43430_CHIP_ID && ...)
    brcmf_feat_iovar_data_set(ifp, BRCMF_FEAT_GSCAN, ...);
```
and feature.c:321-328, 356-366 have chip-specific switch statements. 0xFFFF avoids all known chip IDs, so falls through to default (no quirks). This is correct behavior.
**Issue:** "BCM65535/0" in kernel logs is confusing. Not a functional issue but unhelpful for debugging.
**Fix:** Consider using a chip ID like 0xBEEF or defining `BRCMF_CHIP_SIM = 0xFFFF` with a comment. Or add a special case in the sim's preinit to override `ri->chipname` to "HWSIM/0".

---

### 🟢 DR-11: `brcmf_release_module_param` must be called on sim cleanup
**Category:** Integration
**Evidence:** common.c:570-573: `brcmf_release_module_param()` does `kfree(module_param)`. SDIO calls this in its cleanup. The settings struct allocated by `brcmf_get_module_param()` must be freed.
**Issue:** The plan's teardown doesn't mention freeing the `settings` struct returned by `brcmf_get_module_param()`.
**Fix:** Sim shim cleanup must call `brcmf_release_module_param(settings)` after `brcmf_free()`.

---

### 🟢 DR-12: `brcmf_c_preinit_dcmds` `ver` iovar needs specific format
**Category:** Interface
**Evidence:** common.c:364-384:
```c
err = brcmf_fil_iovar_data_get(ifp, "ver", buf, sizeof(buf));
...
ptr = (char *)buf;
strsep(&ptr, "\n");       // split at newline
brcmf_info("Firmware: %s %s\n", ri->chipname, buf);
ptr = strrchr(buf, ' ');   // find LAST space
if (!ptr) { ... goto done; }  // FATAL if no space found
strscpy(ifp->drvr->fwver, ptr + 1, sizeof(ifp->drvr->fwver));
```
**Issue:** The `ver` response must contain at least one space character, otherwise `strrchr(buf, ' ')` returns NULL and probe terminates with error (goto done). The plan mentions `ver` as probe-FATAL but doesn't specify the required format.
**Fix:** Document that `ver` must return a string like `"wl0: <date> version X.Y.Z\nother_info"` — at minimum it needs a space before the version number. Suggest: `"brcmfmac_hwsim 1.0.0\n"`.

---

### 🟢 DR-13: Platform device + driver registration order
**Category:** Kernel Practice
**Evidence:** The plan uses `platform_device_register("brcmfmac-sim-sdio")` inside `brcmf_sim_sdio_register()`. The standard kernel pattern is to register the platform_driver first, then the platform_device. If the device is registered before the driver, the probe won't fire until the driver is registered. Since both happen in `brcmf_sim_sdio_register()`, the order within this function matters.
**Issue:** Minor — the plan doesn't specify the internal order.
**Fix:** Register the `platform_driver` first, then `platform_device_register()`. The device registration triggers `platform_driver.probe` synchronously if the driver is already registered. Alternatively, use `platform_device_register_simple()` for cleaner code.

---

### 🟢 DR-14: Missing `brcmf_bus_add_txhdrlen` consideration
**Category:** Interface
**Evidence:** core.c:1380-1388 and bus.h:316. SDIO calls `brcmf_bus_add_txhdrlen(dev, SDPCM_HDRLEN)` to inform the core how much extra headroom is needed for bus headers. BCDC also adds headroom in `brcmf_proto_bcdc_attach()` (bcdc.c:473).
**Issue:** The sim shim doesn't need bus-level header space (no SDPCM framing), but if it doesn't call `brcmf_bus_add_txhdrlen()`, the `drvr->hdrlen` may be too small for BCDC's needs. Actually, BCDC's own attach adds its header length (bcdc.c:473: `drvr->hdrlen += BCDC_HEADER_LEN + BRCMF_PROT_FW_SIGNAL_MAX_TXBYTES`), so the BCDC header space is already accounted for. The sim shim needs zero additional bus headroom.
**Fix:** No action needed — `drvr->hdrlen` is initialized to 0 in `brcmf_attach()` (core.c:1340), and BCDC adds its own header space. The sim doesn't need to call `brcmf_bus_add_txhdrlen()`. Document this as intentional (no bus framing overhead for sim).

---

## Summary

### Overall Architectural Assessment: **SOUND with fixable gaps**
The two-component architecture (bus shim + hwsim module) is fundamentally correct and well-designed. The IPC via `symbol_get()`/`symbol_put()` is appropriate for module-to-module communication. The Kconfig mutual exclusion, platform_device self-probe, BCDC reuse, and WCC vendor binding are all architecturally sound choices. The identified issues are implementation-level gaps, not fundamental design flaws.

### Top 3 Risks
1. **Silent event drops** (DR-07): The event frame format is complex (BCDC + ETH + BRCM + event_msg_be, all with specific byte values). Any byte-level mistake silently drops the event with no error log. This will be the hardest part to debug.
2. **Missing Kconfig `select`** (DR-04): Without `select BRCMFMAC_PROTO_BCDC`, the build will succeed but `brcmf_proto_attach()` will fail at runtime with a confusing "Unsupported proto type" error. Easy to miss during initial development.
3. **Teardown crash** (DR-02 + DR-06): Missing `stop` bus_op causes NULL deref panic on any teardown path. Missing `brcmf_detach()`/`brcmf_free()` leaks cfg80211/wiphy resources.

### Probe Sequence Success Confidence: **78%**
The probe flow is mostly correct but the three CRITICAL items (DR-01 wrong function name, DR-02 missing stop op, DR-03 misleading maxctl) and two HIGH items (DR-04 missing Kconfig select, DR-05 missing dev_set_drvdata) would each prevent successful compilation or probe. Once these five items are fixed, the probe sequence should work. The remaining HIGH/MEDIUM items affect correctness of operation (events, teardown) but not initial probe success.

---

*Update this file after every 2 view/browser/search operations*
*This prevents visual information from being lost*

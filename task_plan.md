# Task Plan: brcmfmac In-Tree HWSIM Mock Bus (v5.3 — Design Review 整合完成)

## Goal
Design and implement a two-component simulation stack for `brcmfmac` in Linux 6.12.81:
1. **Inside brcmfmac** — compile-time bus replacement (`CONFIG_BRCMFMAC_HWSIM_SDIO` / `CONFIG_BRCMFMAC_HWSIM_PCIE`) that substitutes the real SDIO/PCIe bus implementation with a mock bus shim.
2. **Separate kernel module** (`brcmfmac_hwsim.ko`) — the "mocked bus" that acts as the virtual firmware and hardware bus, receiving packets from brcmfmac, parsing CDC/MSGBUF headers, handling ioctl commands, converting TX→RX for data packets, simulating firmware download acceptance, generating interrupts, and producing firmware events.

The end result: brcmfmac probes a simulated phy device, creates `wlan0`, and communicates through the mock bus module as if talking to real Broadcom firmware.

## Current Phase
**Milestone 1 — ✅ COMPLETE (2026-04-21)**: wlan0 probes via sim_sdio, wpa_supplicant can attach/detach, dmesg 100% clean (0 WARNING / 0 error).
**Milestone 2 — 🚧 IN PROGRESS**: wlan1 AP + hostapd + wlan0 STA associate + data-plane loopback.

## Milestone 2 Scope (User approved 2026-04-21 — "option 3")
Goal: Bring up hostapd on a second virtual interface `wlan1` (AP mode) on the same phy, have `wpa_supplicant` on `wlan0` scan, find, authenticate, associate, and exchange data frames with the AP — all over the sim_sdio mock bus. No real RF / no real firmware.

### M2 Phase Map
| Phase | Deliverable | Key Work |
|-------|------------|----------|
| M2-A  | wlan1 AP iface appears after `iw dev wlan0 interface add wlan1 type __ap` | `interface_create` iovar v1 GET handler that echoes struct back with synthesized MAC; emit `WLC_E_IF` ADD event → driver calls `brcmf_add_if` → register_netdev wlan1 |
| M2-B  | `hostapd` binary loads, reaches `WLAN_STATUS_SUCCESS` on wlan1 | AP iovars: `bsscfg:ssid`, `bsscfg:ap`, `bss`, `bcn_prb`, `wsec`, `wpa_auth`, `mpc`, dcmds `WLC_SET_SSID`, `WLC_SET_BEACON_INTERVAL`, `WLC_DOWN/UP`, `WLC_SET_AP`, `WLC_SET_CHANNEL` + event `WLC_E_LINK` on AP |
| M2-C  | `wpa_supplicant` on wlan0 sees the AP SSID in scan results | escan path: `escan`/`escan_params` iovar sink → sim fabricates BSS announcement matching wlan1's config, sends `WLC_E_ESCAN_RESULT` events with synthesized beacon IE buffer |
| M2-D  | wlan0 associates with wlan1 AP (4-way handshake works) | JOIN/AUTH/ASSOC dcmds + events `WLC_E_AUTH`, `WLC_E_ASSOC`, `WLC_E_LINK`; inter-bsscfg EAPOL data forwarding inside sim_sdio |
| M2-E  | `ping` from wlan0 to wlan1 side reaches userspace | Data-plane loopback in sim_sdio: wlan0 TX SKB → route by dst MAC → wlan1 RX via `brcmf_rx_frame`; both sides see each other's traffic |

### M2 Exit Criteria
- `hostapd -i wlan1 -dd` stays running with `AP-ENABLED`
- `wpa_cli -i wlan0 scan_results` lists the SSID
- `wpa_cli -i wlan0 status` shows `wpa_state=COMPLETED`
- `ping -I wlan0 <wlan1-side-addr>` succeeds
- dmesg during the whole flow: 0 WARNING / 0 stack trace (errors only for designed-refusal cases)
- handover protocol: every phase transition writes a checkpoint block to progress.md and updates this plan

### M2 Handover Protocol (enforced)
1. **At every phase boundary** (A→B, B→C, …) append to progress.md:
   - Current goal, current phase, files modified, commands run, validation done, next exact action, blockers.
2. **After every 2 search/view/edit operations** persist key findings into findings.md.
3. **Every commit** updates the overlay repo (`/Users/carterchan/working/coding/hwsim/repo/`) and pushes to GitHub.
4. **3-strike rule**: any single iovar/dcmd/event that fails 3 times in a row → stop, escalate via `ask_user`, do not retry the same code path.
5. **Stop gate**: do not close out a session while planning files are stale relative to code. Final action per session = write handover block.

## Reviews Completed
- ✅ CEO Review: 3 blockers resolved
- ✅ Engineering Review: 1 BLOCKER + 5 HIGH + 4 MEDIUM integrated
- ✅ Firmware Handling: bypass strategy designed
- ✅ QA Review: 7 BLOCKERS + 14 HIGH + 17 MEDIUM integrated
- ✅ **Design Review: 2 BLOCKERS + 9 HIGH + 5 MEDIUM integrated (v5.3)**

### Key Design Review Fixes (v5.3)
- **D1**: `brcmf_txcomplete()` → `brcmf_proto_bcdc_txcomplete()` 全文修正
- **D2**: BCDC error codes 必須用 BCME_* (-23) 不是 Linux errno (-95)
- **D3-D5,D11**: Probe-FATAL 分類修正（join_pref, REVINFO, clmver, country 非 fatal）
- **D6,D8**: Shim probe 補齊 `brcmf_get_module_param()` 和 `dev_set_drvdata()`
- **D9**: chip ID 統一 `0xFFFF`（修正 Probe Flow 不一致）
- **D10**: rxctl return semantics = byte count（正數），timeout = -ETIMEDOUT

## Resolved Decisions (CEO Review)
| Decision | Resolution |
|----------|-----------|
| **Git 策略** | 本地 dev branch 追蹤，不 push 到 GitHub。linux-6.12.81 和 hostap 各自建 local branch |
| **IPC 機制** | Exported Symbols + `symbol_get()`/`symbol_put()`（Option 1） |
| **編譯/測試環境** | Intel NUC 直接編譯（x86_64 native），user 負責 NUC 設定 |
| **Phase 16 (PCIe)** | 獨立 milestone，不在本期 scope。SDIO+BCDC 完成後再議 |
| **Kconfig 互斥** | `CONFIG_BRCMFMAC_HWSIM_SDIO` 與 `CONFIG_BRCMFMAC_SDIO` 互斥 (XOR)，PCIE 同理 |
| **VID/PID** | 使用 Broadcom VID + fake PID 做 mock device probe |
| **fwvid** | 重用 WCC vendor ops |
| **未知 ioctl 策略** | GET → BCDC status=-23 (BCME_UNSUPPORTED) + ERROR flag，SET → status=0 (success) [D2] |
| **Thread Safety** | `sim_bus_if.h` 明定：completion for ctrl, symbol_get 引用計數替代 RCU [E8] |
| **每 Phase 驗收條件** | 見各 Phase 詳述 |
| **get_blob** | bus shim 必須實作，返回 `-ENOENT` [E1] |
| **TX completion** | shim 的 txdata 完成後必須呼叫 `brcmf_proto_bcdc_txcomplete()` [E6, D1] |
| **bus_type** | 新增 `BRCMF_BUSTYPE_SIM` enum 值 [E3] |
| **WCC module** | NUC 驗證需先 `insmod brcmfmac-wcc.ko` [E4] |

## Engineering Review Findings (Opus 4.6 Review)

### 🔴 BLOCKER
| ID | Issue | Fix |
|----|-------|-----|
| **E1** | `brcmf_bus_get_blob()` 無 NULL guard（bus.h:258 直接呼叫 `ops->get_blob()`）→ shim 未實作 = NULL deref OOPS | bus shim 必須實作 `get_blob` op，返回 `-ENOENT` |

### 🟡 HIGH
| ID | Issue | Fix |
|----|-------|-----|
| **E2** | Kconfig 需加 `depends on BRCMFMAC = m` — built-in brcmfmac 無法與 loadable hwsim.ko 配合 | Phase 12: 加入 `depends on BRCMFMAC = m` 條件 |
| **E3** | `brcmf_get_module_param()` 需要 `enum brcmf_bus_type` 值，目前只有 SDIO/USB/PCIE | Phase 12: 在 `brcmfmac.h` 新增 `BRCMF_BUSTYPE_SIM` |
| **E4** | WCC vendor module (`brcmfmac-wcc.ko`) 必須在 NUC 上安裝，否則 `brcmf_fwvid_attach()` 失敗 | NUC 驗證步驟加入 `insmod brcmfmac-wcc.ko` |
| **E5** | ioctl handler table 遺漏 `"cap"`、`"join_pref"`、`"txbf"` — 缺 `"cap"` 會導致 cfg80211 無功能註冊 | Phase 14: 補充完整 ioctl table |
| **E6** | TX 完成路徑：shim **必須** 呼叫 `brcmf_proto_bcdc_txcomplete()` 消耗 TX skb，否則記憶體洩漏 + TX queue 停滯 | Phase 15: sim_sdio txdata 完成後呼叫 `brcmf_proto_bcdc_txcomplete()` [D1] |

### 🟢 MEDIUM
| ID | Issue | Fix |
|----|-------|-----|
| **E7** | hwsim 模組用 `EXPORT_SYMBOL_GPL`（不帶 namespace）避免 `symbol_get()` 的 `MODULE_IMPORT_NS` 問題 | Phase 14: 使用 `EXPORT_SYMBOL_GPL()` |
| **E8** | Locking 過度設計：RCU 與 `symbol_get()` 的模組引用計數重複；改用 `symbol_get` + `completion` 即可 | Phase 13: 簡化 locking model |
| **E9** | Module unload race：teardown 時 in-flight hwsim→shim callback = use-after-free | Phase 15: unload 時先 flush workqueue 再 `symbol_put()` |
| **E10** | hwsim/ Makefile 需正確 Kconfig gating | Phase 12: 按照 wcc/cyw/bca 模式 |

## QA Review Findings (Opus 4.6 QA Review — v5.2)

### 🔴 BLOCKER
| ID | Issue | Fix |
|----|-------|-----|
| **B1** | IOCTL handler table不完整 — `event_msgs`(GET+SET probe-fatal), `brcmf_feat_attach()` 14 iovars 缺失 | Phase 14: 完整追蹤 `brcmf_c_preinit_dcmds()` + `brcmf_feat_attach()` + `brcmf_setup_wiphybands()` 產出 FATAL/OPTIONAL 分類 ioctl 表（v5.3: join_pref/REVINFO/clmver/country 修正為 non-fatal [D3-D5,D11]） |
| **B2** | 無 Scan event generation 規格 — `escan` iovar 未列入，mock 未產生 `BRCMF_E_ESCAN_RESULT` (partial+complete) → `wpa_cli scan` hang 10s | Phase 14: 新增 scan event generator，模擬至少一個 BSS result (`brcmf_escan_result_le`) |
| **B3** | 無 Connect/Association event 規格 — `join` bsscfg iovar、security ioctls、`BRCMF_E_LINK` event 未定義 → STA 無法連線 | Phase 14: 新增 connect event flow，定義 join→timer→BRCMF_E_LINK(flags=1) 序列 |
| **B4** | 無 AP mode event 規格 — hostapd 需 ~11 ioctls (SET_AP/SET_INFRA/UP/DOWN/SET_SSID 等) + `BRCMF_E_SET_SSID` event | Phase 14: AP mode ioctls 加入 known handler table |
| **B5** | Module unload race — `rmmod brcmfmac_hwsim` while brcmfmac active；`symbol_get()` 只鎖 hwsim 引用；hwsim→shim callback 可能 use-after-free | Phase 15: 定義完整 teardown protocol（shutting_down flag → detach → flush → symbol_put）；測試 rmmod hwsim 時 busy |
| **B6** | `bus_priv` union 未修改 — `struct brcmf_bus` 的 `bus_priv` union 缺 `sim` 成員 → shim 無法透過 `bus->bus_priv.sim` 存取私有資料 | Phase 12: bus.h 新增 `struct brcmf_sim_dev *sim` 至 bus_priv union |
| **B7** | `"cap"` iovar 回應內容未指定 — 空字串 = wiphy 無 HT/VHT/MFP → cfg80211 功能缺失 | Phase 14: 定義 cap 回應 `"sta wl 11n 11ac 11d 11h mfp"` |

### 🟡 HIGH
| ID | Issue | Fix |
|----|-------|-----|
| **H1** | 驗收條件非 binary pass/fail — "dmesg 無 WARN" 不夠精確 | 所有 Phase: 定義 PASS/FAIL exact condition |
| **H2** | 無 module load ordering 負面測試 — 只測 happy path | Phase 18: 新增 brcmfmac without hwsim、without WCC、wrong order 測試 |
| **H3** | 無 KASAN/kmemleak/lockdep 測試 | Phase 18: NUC 以 `CONFIG_KASAN=y`/`KMEMLEAK=y`/`LOCKDEP=y` 編譯測試 |
| **H4** | Stress test 只有 10 次 load/unload — 不足以暴露 race/leak | Phase 18: 3 層壓測（10/100/1000 次） |
| **H5** | TX skb lifecycle 未完整指定 — 何時 `brcmf_proto_bcdc_txcomplete()`、誰 own skb、error path [D1] | Phase 13: `sim_bus_if.h` 加 kernel-doc 文檔 skb ownership 協議 |
| **H6** | BCDC reqid matching 未測試 — wrong reqid response 的行為 | Phase 17: fault inject 加 reqid mangle 測試 |
| **H7** | 無 concurrent ioctl + data traffic 測試 | Phase 18: 並行 ping + wpa_cli 測試 |
| **H8** | `event_msgs` iovar 語意關鍵 — GET 回傳 `BRCMF_E_LAST/8` bytes, SET 需存儲 mask | Phase 14: 明確定義 event_msgs buffer size (27 bytes for WCC) |
| **H9** | `BRCMF_BUSTYPE_SIM` 下游影響 — grep 所有 `switch(bus_type)` 確認 default case | Phase 12: 全面搜索 bus_type switch 驗證安全性 |
| **H10** | `platform_device` lifecycle 不完整 — name、cleanup、failure path | Phase 15: 文檔化完整 platform_device 生命週期 |
| **H11** | `fw_download(NULL)` 語意未定義 — 返回值、同步/非同步、冪等性 | Phase 13: sim_bus_if.h 加 kernel-doc: 同步, return 0=success, 冪等 |
| **H12** | WCC binding flow 未驗證 — `request_module("brcmfmac-wcc")` + completion | Phase 18: 測試 WCC pre-loaded 和 WCC not loaded 兩種路徑 |
| **H13** | `get_blob()` 精確語意 — 返回 `-ENOENT` + 不修改 fw pointer | Phase 15: 明確實作 + 測試 dmesg "no clm_blob" 訊息 |
| **H14** | WCC event code range — mock events 必須 < 213 | Phase 14: 文檔化 event code 限制；Phase 17: 加 out-of-range fault inject |

### 🟢 MEDIUM (摘要)
| ID | Issue | Fix |
|----|-------|-----|
| **M1** | `maxctl` 未指定 | 設定 `maxctl = 8192` (同真實 SDIO) |
| **M2** | wiphy band/channel 測試缺失 | chanspec/bw_cap iovar 需回傳模擬 channel data |
| **M3** | bus shim 無 debugfs | 實作 debugfs_create 暴露 bus state/ioctl count |
| **M4** | Data loopback test 不足 | 明確定義 100 ICMP + 計數驗證 |
| **M5** | 無 suspend/resume 測試 | 低優先，wowl_config stub NULL |
| **M6** | 無 gettxq/flow control 測試 | 低優先，NULL fallback 驗證 |
| **M7** | test script 缺 WCC module load | 修正 script 順序 |
| **M8** | `brcmf_get_module_param()` SIM case | 追蹤驗證 default case 安全 |
| **M9** | Fault injection debugfs 格式未定義 | 定義 file names/format |
| **M10** | 無 dmesg positive indicator 驗證 | 加正面 dmesg grep checks |
| **M11** | Multi-interface (wlan1) 建立機制未定義 | BRCMF_E_IF event 或 iw add_iface |
| **M12** | callback registration thread safety | 強制 ordering: register CB → fw_download |
| **M13** | hwsim_ops 無 error code documentation | sim_bus_if.h 加 kernel-doc |
| **M14** | rmmod while wpa_supplicant running | 新增 lifecycle test |
| **M15** | chip/chiprev 影響 feature detection | 考慮用 0xFFFF 避免 chip-specific paths |
| **M16** | wiphy name 不可預期 | 用 `ls /sys/class/ieee80211/` 測試 |
| **M17** | 無 Known Limitations 文檔 | 新增 Known Limitations 段落 |

## Design Review Findings (Opus 4.6 Deep-Dive — v5.3)

### 🔴 BLOCKER
| ID | Issue | Evidence | Fix |
|----|-------|---------|-----|
| **D1** | TX completion function 錯誤 — plan 全文用 `brcmf_txcomplete()` 但實際為 `brcmf_proto_bcdc_txcomplete(dev, skb, success)` | sdio.c:2324, usb.c:495, bcdc.h:12 | 全部修正為 `brcmf_proto_bcdc_txcomplete()` |
| **D2** | BCDC error codes 必須用 Broadcom fw error codes (BCME_*), NOT Linux errno — plan 說 "GET → `-ENOTSUP`" 但 feature detection 檢查 `-BRCMF_FW_UNSUPPORTED`(-23)，`-ENOTSUP`(-95) 會導致**所有 feature 被錯誤啟用** | feature.c:194 checks `err != -23`, fwil.c:118-124 fw error path, bcdc.c:209-210 ERROR flag + status field | hwsim BCDC response 未知 iovar: flags 設 BCDC_DCMD_ERROR(0x01), status = -23。明確區分「transport error」(txctl/rxctl return int) vs「firmware error」(BCDC status field) |

### 🟡 HIGH
| ID | Issue | Evidence | Fix |
|----|-------|---------|-----|
| **D3** | `join_pref` SET **非** probe-fatal — plan B1 標為 fatal 但實際為 void function | common.c:413 `brcmf_c_set_joinpref_default(ifp)` = void, 內部 error 只 log 不 propagate | 從 Probe-FATAL 移至 Non-fatal；hwsim SET 仍接受(return 0) |
| **D4** | `BRCMF_C_GET_REVINFO` **非** probe-fatal — error 只設 "UNKNOWN" chipname | common.c:310-313 error → `strscpy("UNKNOWN")`, no `goto done`, err 被 line 343 覆蓋 | 從 Probe-FATAL 移至 Important-but-non-fatal；hwsim 仍回傳有效 revinfo |
| **D5** | `clmver` GET **非** probe-fatal | common.c:389-391 `if (err)` → `brcmf_dbg(TRACE, ...)` 繼續，no goto done | 從 Probe-FATAL 移至 Non-fatal；hwsim 仍回傳模擬字串 |
| **D6** | Shim probe 缺少 `brcmf_get_module_param()` 呼叫 | sdio.c:4001, `brcmf_alloc(dev, settings)` 需要 settings 指標 | Phase 15 probe 步驟加入 `settings = brcmf_get_module_param(&pdev->dev, BRCMF_BUSTYPE_SIM, 0xFFFF, 0)` |
| **D7** | `cap` 非 probe-fatal 但**功能關鍵** — error 只 log 不 propagate 但無 cap = wiphy 無 HT/VHT/MFP 功能 | feature.c:234-237 error → return (not fatal), 但 brcmf_fwcap_map 所有 feature 都不會設定 | 保持在 important 分類，必須回傳有效字串，但不算 fatal |
| **D8** | Phase 15 probe flow 缺 `dev_set_drvdata()` — Probe Flow 段(line 977)有寫但 Phase 15 詳述缺 | core.c:1318 `dev_get_drvdata(dev)` in `brcmf_alloc()` | Phase 15 步驟 3 明確加入 `dev_set_drvdata(&pdev->dev, bus_if)` BEFORE `brcmf_alloc()` |
| **D9** | chip ID 不一致 — Probe Flow 用 `0x4339` 但 M15 決議用 `0xFFFF` | feature.c:298-301 and 356-366 有 chip-specific code paths | 統一為 `chip=0xFFFF, chiprev=0`；修正 Probe Flow 段 |
| **D10** | `rxctl` 必須回傳**位元組數**（正數），不是 0 或 error code | sdio.c:3284-3289 returns `rxlen`; bcdc.c:201 用 ret 當 byte count | Phase 15 rxctl 回傳實際 response length，timeout 回傳 -ETIMEDOUT |
| **D11** | `country` 不在 `preinit_dcmds` probe 路徑 — plan 標為 probe-fatal 但 grep 無此呼叫 | common.c:266-458 無 "country" 呼叫，country 在 cfg80211.c 運行時呼叫 | 從 Probe-FATAL 移至 Runtime category |

### 🟢 MEDIUM
| ID | Issue | Evidence | Fix |
|----|-------|---------|-----|
| **D12** | Feature detection iovars 用 `fwil_fwerr=true` 路徑，回傳值是 raw fw error code 不是 Linux errno | feature.c:191-202, fwil.c:123-124 | hwsim 文檔明確：feature detection 時 BCDC status=-23, not -ENOTSUP |
| **D13** | `txbf` SET 未列入 ioctl table（非 fatal，已被 `(void)` 顯式忽略） | common.c:455 | 加入 non-fatal SET category（default SET→0 已覆蓋，但文檔化） |
| **D14** | `brcmf_release_module_param(settings)` 需在 teardown 時呼叫避免 leak | common.c:570 `kfree(module_param)` | Phase 15 teardown 加入 |
| **D15** | NUC 手動驗證步驟缺 `insmod brcmfmac-wcc.ko` | Build Verification section line 828 直接跳到 hwsim | 修正驗證步驟順序 |
| **D16** | `symbol_get()` 需配合正確 typedef — `symbol_get(sym)` 返回 `typeof(sym) *` | kernel module.h macro | sim_bus_if.h 加入 extern function prototypes |

### BCDC Error Code 完整說明 (D2 展開)
```
Broadcom Firmware Error Codes (BCDC dcmd status field, le32):
  BCME_OK           =  0   (success)
  BCME_ERROR        = -1   (generic error)
  BCME_BADARG       = -2
  BCME_NOTUP        = -4
  BCME_NOTAP        = -6
  BCME_UNSUPPORTED  = -23  ← feature detection 用這個判斷是否支援
  BCME_NOTFOUND     = -30

Driver 轉換路徑:
  1. hwsim 設定 BCDC response: flags |= BCDC_DCMD_ERROR (0x01), status = -23
  2. BCDC layer: sees ERROR flag → *fwerr = le32_to_cpu(status) = -23
  3. fwil layer: fwerr < 0 → err = -EBADE (normally) 
                fwil_fwerr=true → return fwerr (-23) directly
  4. feature.c: err == -23 == -BRCMF_FW_UNSUPPORTED → feature NOT enabled ✓
  
  ⚠ 如果 hwsim 回傳 status=-95 (-ENOTSUP):
     fwil_fwerr=true → return -95
     feature.c: -95 != -23 → feature ENABLED ← 嚴重錯誤！所有 feature 被啟用！
```

### 正確的 Probe-Fatal ioctl 分類 (D3-D5, D7, D11 修正)

#### ✅ Probe-FATAL (error 會導致 probe 失敗):
| ioctl/iovar | Direction | Evidence |
|-------------|-----------|---------|
| `cur_etheraddr` | GET | common.c:287-289 → `goto done` |
| `ver` | GET | common.c:366-369 → `goto done` |
| `mpc` | SET | common.c:407-411 → `goto done` |
| `event_msgs` | GET | common.c:416-421 → `goto done` |
| `event_msgs` | SET | common.c:429-434 → `goto done` |
| `BRCMF_C_SET_SCAN_CHANNEL_TIME` | SET | common.c:437-442 → `goto done` |
| `BRCMF_C_SET_SCAN_UNASSOC_TIME` | SET | common.c:446-451 → `goto done` |

#### 🟡 Important but Non-fatal (error 降級處理但不中斷 probe):
| ioctl/iovar | Direction | Degradation |
|-------------|-----------|-------------|
| `BRCMF_C_GET_REVINFO` | GET | chipname = "UNKNOWN" |
| `cap` | GET | 所有 feature flags = 0，wiphy 功能缺失 |
| `clmver` | GET | 只有 TRACE log，無影響 |
| `join_pref` | SET | 只 log error，不影響 probe |
| `txbf` | SET | 顯式 `(void)` 忽略 |

#### ⚪ Runtime (不在 probe path):
| ioctl/iovar | Direction | When called |
|-------------|-----------|------------|
| `country` | SET/GET | cfg80211 setup / regulatory |
| `escan` | SET | wpa_cli scan |
| `join` bsscfg | SET | wpa_cli connect |

- hwsim orchestration study: complete
- brcmfmac bus/proto/control/event study: complete
- Initial architecture design: complete
- Build system (Kconfig/Makefile) study: complete
- Probe path (SDIO + PCIe) study: complete
- Firmware loading path study: complete
- fwvid vendor binding study: complete
- BCDC dcmd wire format study: complete
- SDIO DPC/interrupt mechanism study: complete
- PCIe mailbox/threaded IRQ study: complete

## Two-Component Architecture

### Component A: Bus Shim (inside brcmfmac)
Lives in brcmfmac's source tree, gated by Kconfig. Replaces `sdio.c`+`bcmsdh.c` or `pcie.c`.
- Implements `brcmf_bus_ops` (txdata, txctl, rxctl, stop, preinit, etc.)
- Connects to the mocked bus module via `symbol_get()` / `symbol_put()` 取得 hwsim 模組匯出的 ops struct
- Does NOT interpret packet content — just passes raw frames to/from the mocked bus module
- Handles interrupt callbacks from the mocked bus module (e.g. "data available", "event pending")
- Creates a `platform_device` for self-probe (no real SDIO/MMC or PCI dependency)
- Manages the brcmf_bus lifecycle (state machine, chip/chiprev, proto_type, fwvid)

### Component B: Mocked Bus Module (`brcmfmac_hwsim.ko`)
Separate loadable kernel module that acts as the virtual firmware + virtual hardware bus.
Responsibilities:
1. **Firmware download simulation** — accepts firmware image content from the bus shim, returns appropriate "download complete" status
2. **IOCTL/iovar handling** — receives BCDC dcmd or MSGBUF ioctl request, parses command ID and payload, produces response with correct status/data and matching request ID
3. **Data packet TX→RX conversion** — receives TX packets (with CDC/MSGBUF header), strips TX header, re-wraps as RX packet (with RX CDC/MSGBUF header), sends back to driver. Example: SA=wlan0 DA=wlan1 → mocked bus converts TX CDC→RX CDC, driver receives on RX path
4. **Firmware event generation** — produces `BRCMF_E_*` event packets (CDC-framed or MSGBUF WL_EVENT completion) and delivers to driver via interrupt + RX path
5. **Interrupt simulation** — triggers interrupt callbacks to the bus shim to signal "control response ready", "data frame available", or "event pending"
6. **Fault injection** — debugfs/configfs interface for injecting error codes, delays, malformed responses, spurious events, firmware crash simulation

### Packet Flow: IOCTL (SDIO+BCDC)
```
wpa_supplicant → nl80211 → brcmf_cfg80211 → brcmf_fil_iovar_data_get("cur_etheraddr")
  → brcmf_proto_bcdc_msg(): builds bcdc_dcmd{cmd, len, flags(reqid), status=0} + payload
    → brcmf_bus_txctl(msg, len)
      → sim_sdio_bus_txctl(): forwards raw msg to mocked bus module
        → brcmfmac_hwsim receives: parse bcdc_dcmd header → extract cmd/reqid/ifidx
          → cmd handler: "cur_etheraddr" → copy configured MAC into response buffer
          → builds response: bcdc_dcmd{cmd, len, flags(same reqid), status=0} + MAC data
          → triggers interrupt to sim_sdio → "control response available"
      → sim_sdio receives interrupt → stores response in rxctl buffer → wakes ctrl_wait
    → brcmf_bus_rxctl() → sim_sdio_bus_rxctl(): returns stored response
  → brcmf_proto_bcdc_cmplt(): verifies reqid match
  → brcmf_proto_bcdc_query_dcmd(): copies response data to caller buffer
→ MAC address returned to cfg80211
```

### Packet Flow: Data TX→RX (SDIO+BCDC)
```
wlan0 sends frame (DA=wlan1, SA=wlan0)
  → brcmf_netdev_start_xmit() → brcmf_proto_bcdc_txhdrpush() adds BCDC data header
    → brcmf_bus_txdata(skb)
      → sim_sdio_bus_txdata(): forwards skb to mocked bus module
        → brcmfmac_hwsim receives: strip SDPCM + BCDC TX header
          → inspect DA → route to appropriate interface/loopback
          → wrap as RX frame: add BCDC RX header (flags2=target_ifidx, data_offset)
          → trigger interrupt to sim_sdio → "frame available"
      → sim_sdio: interrupt handler wakes DPC / calls brcmf_rx_frame()
    → brcmf_proto_bcdc_hdrpull(): strips BCDC header, extracts ifidx
  → dispatched to wlan1's netdev → delivered to kernel networking stack
```

### Packet Flow: Firmware Event
```
brcmfmac_hwsim wants to emit BRCMF_E_LINK (e.g. after simulated association)
  → builds brcmf_event struct with event_type=BRCMF_E_LINK, status=0, reason=0
  → wraps in BCDC data header (flags2=ifidx, ethertype=BRCM_ETHERTYPE)
  → triggers interrupt to sim_sdio → "frame available"
  → sim_sdio: calls brcmf_rx_frame() with the event skb
  → brcmf_proto_bcdc_hdrpull() → sees BRCM_ETHERTYPE → brcmf_rx_event()
  → brcmf_fweh_process_event() → schedules event work → cfg80211 callback
```

### Firmware / NVRAM / CLM Blob 處理策略

#### 真實 SDIO 的 firmware 載入流程 (參考)
```
brcmf_sdio_probe()
  → brcmf_sdio_prepare_fw_request()
    → brcmf_fw_alloc_request(chip_id, chiprev, brcmf_sdio_fwnames[])
      → 依 chip ID 查找 firmware mapping table 建立檔名:
         brcmfmac<chip>-sdio.bin      (firmware binary)
         brcmfmac<chip>-sdio.txt      (NVRAM key=value)
         brcmfmac<chip>-sdio.clm_blob (CLM data, OPTIONAL)
  → brcmf_fw_get_firmwares(dev, fwreq, callback)
    → request_firmware_nowait() — 從 /lib/firmware/brcm/ 非同步載入
      → brcmf_sdio_firmware_callback()
        → 取得 firmware binary, nvram data, clm_blob
        → brcmf_sdio_download_firmware(bus, code, nvram, nvram_len)
          → 透過 SDIO register writes 將 firmware 寫入晶片
        → sdiod->clm_fw = clm_blob  (存起來，後面用)
        → 設定硬體（clock, interrupt, watermark）
        → bus_if->ops = &brcmf_sdio_bus_ops
        → brcmf_alloc() + brcmf_attach()
          → brcmf_bus_started() → brcmf_c_preinit_dcmds()
            → brcmf_c_process_clm_blob()
              → brcmf_bus_get_blob(bus, &fw, BRCMF_BLOB_CLM)
                → get_blob() 返回之前存的 clm_fw → 下載到 firmware
            → brcmf_c_process_txcap_blob()
              → get_blob(BRCMF_BLOB_TXCAP) → SDIO 返回 -ENOENT (不支援)
            → brcmf_c_process_cal_blob()
              → 使用 settings->cal_blob (platform data) → 通常為 NULL → skip
```

#### 問題：Mock device (fake PID) 沒有對應的 firmware 檔案
- 我們用 Broadcom VID + fake PID (e.g. chip_id=0xFFFF, chiprev=0)
- 真實的 firmware loading 會嘗試載入 `brcmfmac65535-sim.bin`
- 這些檔案不存在於 `/lib/firmware/brcm/`
- 如果走標準 `brcmf_fw_get_firmwares()` 路徑，`request_firmware()` 會失敗

#### 設計方案（兩層）

**Layer 1 — Bus Shim Probe (sim_sdio.c): 完全繞過 firmware loading**
```
brcmf_sim_sdio_probe()
  → [不呼叫 brcmf_fw_get_firmwares()]
  → 呼叫 hwsim_ops->fw_download(ctx, NULL, 0) 模擬 firmware ready
    → hwsim module 內部設定 "firmware booted" 狀態
  → 直接設定 bus_if: ops, chip, chiprev, proto_type(BCDC), bus_type(SIM)
  → brcmf_alloc() + brcmf_attach()
  → [driver 進入正常運作狀態]
```
**理由:**
- firmware download 到 SDIO 硬體的操作（register writes, DMA, clock control）
  是深度硬體相關的，在 simulation 環境中沒有意義
- request_firmware() 需要實體 firmware 檔案，對 mock device 沒必要
- 繞過 firmware loading 是最簡潔、最不容易出錯的方法

**Layer 2 — `get_blob()` 在 `brcmf_c_preinit_dcmds()` 期間的處理:**
```
brcmf_c_preinit_dcmds() 呼叫順序:
  1. brcmf_c_process_clm_blob()
     → brcmf_bus_get_blob(BRCMF_BLOB_CLM)
     → shim 的 get_blob() 返回 -ENOENT
     → 核心印出: "no clm_blob available, device may have limited channels"
     → 回傳 0 (不是 error) → 繼續執行 ✓

  2. brcmf_c_process_txcap_blob()
     → brcmf_bus_get_blob(BRCMF_BLOB_TXCAP)
     → get_blob() 返回 -ENOENT
     → "no txcap_blob available" → return 0 → 繼續 ✓

  3. brcmf_c_process_cal_blob()
     → 檢查 settings->cal_blob (platform data)
     → 我們的 settings 沒有 cal_blob → 直接 return 0 ✓
```
**結論：所有 blob 處理都 gracefully 退化，driver 正常啟動（只是 channel 受限）。**

#### 不需要 firmware 檔案的原因
| 項目 | 真實 SDIO | HWSIM Mock |
|------|----------|-----------|
| firmware binary (.bin) | 下載到晶片 RAM | 不需要 — hwsim 模組本身就是 "firmware" |
| NVRAM (.txt) | 寫入晶片 NVRAM | 不需要 — hwsim 直接用內嵌預設值回應 ioctl |
| CLM blob (.clm_blob) | 透過 ioctl 下載 | 不需要 — get_blob 返回 -ENOENT，graceful skip |
| TxCap blob | 透過 ioctl 下載 | 不需要 — get_blob 返回 -ENOENT，graceful skip |
| Calibration blob | platform data | 不需要 — settings->cal_blob = NULL |

#### 模擬 firmware 狀態 (hwsim module 內部)
hwsim module 在收到 `fw_download()` 呼叫後:
- 設定內部狀態 `fw_state = HWSIM_FW_BOOTED`
- 從此開始回應 ioctl/iovar（在 `fw_download` 前的 ioctl 可選擇返回 error）
- 對 `"ver"` iovar 回應模擬版本字串: `"wl0: Oct 01 2024 brcmfmac-hwsim-1.0"`
- 對 `"clmver"` 回應: `"API: 0.0 Data: hwsim.0"`

#### 未來擴展：可選的 request_firmware 路徑
如果未來需要測試 firmware loading path 本身，可加入:
1. 新增 firmware mapping entry: `BRCMF_FW_DEF(HWSIM, "brcmfmac-hwsim")`
2. 建立最小 dummy firmware 檔案放在 `/lib/firmware/brcm/`:
   - `brcmfmac-hwsim.bin` — 幾 bytes magic header
   - `brcmfmac-hwsim.txt` — 最小 NVRAM (`boardtype=0xffff\0boardrev=0xff\0`)
3. shim probe 時走標準 `brcmf_fw_get_firmwares()` 路徑
4. firmware_callback 將 firmware 傳給 hwsim module (驗證 magic → accept)
**→ 這是 Phase 16+ 的事，不在本期 Milestone 1 scope 內**

## Implementation Phases

### Phase 11: Amended Architecture Design v2 (current)
- [x] User clarified: mocked bus is a SEPARATE kernel module, not just code inside brcmfmac
- [x] Redesigned architecture: bus shim (inside brcmfmac) + mocked bus module (separate .ko)
- [x] Documented packet flows for ioctl, data TX→RX, firmware download, and events
- [x] Identified two design options (A: simple shim + smart module, B: high-fidelity transport)
- **Status:** complete

### Phase 11.5: Git Repository & Build Environment Setup
- [ ] `cd linux-6.12.81 && git init && git checkout -b hwsim-dev` 建立 local dev branch
- [ ] `git add -A && git commit -m "baseline: Linux 6.12.81 original source"` 基準 commit
- [ ] `cd hostap && git checkout -b hwsim-dev` 建立 local dev branch（hostap 已是 git repo）
- [ ] 不推送到 GitHub，僅本地追蹤
- **驗收條件:** `git log --oneline` 在兩個 repo 都顯示 baseline commit
- **Status:** pending

### Phase 12: Kconfig & Build System
- [ ] Add `CONFIG_BRCMFMAC_HWSIM_SDIO` to brcmfmac's Kconfig (selects `BRCMFMAC_PROTO_BCDC`)
  - **互斥約束:** `depends on !BRCMFMAC_SDIO`（與真實 SDIO 不可並存）
  - **模組約束:** `depends on BRCMFMAC = m`（brcmfmac 必須是 module，不能 built-in）[E2]
- [ ] Add `CONFIG_BRCMFMAC_HWSIM_PCIE` to brcmfmac's Kconfig (selects `BRCMFMAC_PROTO_MSGBUF`)
  - **互斥約束:** `depends on !BRCMFMAC_PCIE`（與真實 PCIe 不可並存）
  - **模組約束:** `depends on BRCMFMAC = m` [E2]
- [ ] Add `BRCMF_BUSTYPE_SIM` to `enum brcmf_bus_type` in `brcmfmac.h` [E3]
  - grep 所有 `switch(bus_type)` 或 `BRCMF_BUSTYPE_` 確認 default case 安全 [H9]
- [ ] Add `struct brcmf_sim_dev *sim;` to `bus_priv` union in `bus.h` [B6]
- [ ] Add bus shim `.o` files to brcmfmac's Makefile
- [ ] Add `brcmf_sim_sdio_register/exit` to `bus.h`（Phase 16 前不加 pcie）
- [ ] Hook into `core.c:brcmf_core_init()`
- [ ] Create Kconfig + Makefile for `brcmfmac_hwsim.ko` module (under `hwsim/`)
  - 按照 `wcc/`、`cyw/`、`bca/` 的 Makefile 模式 [E10]
- [ ] **Compile check:** `make M=.../brcmfmac modules` 必須通過（stub functions）
- **驗收條件:**
  - `grep BRCMFMAC_HWSIM .config` 顯示正確配置
  - `CONFIG_BRCMFMAC_HWSIM_SDIO=y` 時 `CONFIG_BRCMFMAC_SDIO` 自動 unset
  - `make M=...` 編譯通過（含 stub .o）
- **Commits:**
  - `brcmfmac: add CONFIG_BRCMFMAC_HWSIM_SDIO Kconfig option (XOR with real SDIO)`
  - `brcmfmac: add BRCMF_BUSTYPE_SIM and hwsim bus shim build rules`
  - `brcmfmac: add sim bus registration stubs in bus.h and core.c`
  - `brcmfmac/hwsim: add Kconfig and Makefile for brcmfmac_hwsim module`
- **Status:** pending

### Phase 13: Internal API between Bus Shim and Mocked Bus Module
- [ ] Design `sim_bus_if.h`：定義 shim ↔ hwsim 模組介面
- [ ] **IPC 機制:** hwsim.ko 匯出 `struct brcmf_hwsim_ops` + 註冊/取消函式
  - `brcmf_hwsim_get_ops()` — 由 bus shim 透過 `symbol_get()` 取得
  - `brcmf_hwsim_put_ops()` — 釋放引用（`symbol_put()`）
- [ ] **Ops 定義:**
  ```c
  struct brcmf_hwsim_ops {
      int (*tx_ctl)(void *ctx, u8 *msg, uint len);   /* BCDC dcmd */
      int (*rx_ctl)(void *ctx, u8 *msg, uint len);   /* response */
      int (*tx_data)(void *ctx, struct sk_buff *skb); /* data frame */
      int (*fw_download)(void *ctx, const u8 *fw, size_t fw_len,
                         const void *nvram, size_t nvram_len); /* FW + NVRAM (可為 NULL) */
      void (*detach)(void *ctx);
  };
  ```
  > **Note:** `fw_download()` 的 `fw`/`nvram` 參數在 Milestone 1 中為 NULL/0
  > (shim 繞過 request_firmware，直接呼叫 fw_download(ctx, NULL, 0, NULL, 0))
  > hwsim module 收到 NULL fw → 設定內部 `fw_state = HWSIM_FW_BOOTED`
  > 未來若走標準 firmware loading 路徑，可傳入實際 firmware data
- [ ] **Interrupt callback (shim 提供給 hwsim):**
  ```c
  struct brcmf_hwsim_cb {
      void (*rx_ctl_ready)(void *ctx);     /* control response available */
      void (*rx_data)(void *ctx, struct sk_buff *skb); /* data/event frame */
  };
  ```
- [ ] **Locking model (簡化版 [E8]):**
  - ctrl path: `struct completion` for txctl→rxctl synchronization
  - rxctl buffer: no extra spinlock needed (completion ensures ordering)
  - hwsim ops pointer: `symbol_get()` 的模組引用計數已足夠，不需 RCU
  - data path: skb ownership transfer, no extra lock needed
  - unload: flush workqueue → `symbol_put()` [E9]
- [ ] **skb ownership 協議 [H5, D1]:**
  ```
  txdata(skb): caller 轉移 ownership → shim → hwsim
  完成後（成功或失敗）: shim 呼叫 brcmf_proto_bcdc_txcomplete(dev, skb, success)
  hwsim 返回 error: shim 仍需呼叫 brcmf_proto_bcdc_txcomplete(dev, skb, false)
  ```
- [ ] **fw_download() 語意 [H11]:**
  - 同步呼叫，return 0 = success, negative errno = failure
  - 冪等（多次呼叫等同一次）
  - fw/nvram 參數可為 NULL（Milestone 1 直接 NULL）
- [ ] **Callback registration ordering [M12]:**
  - shim 先 register callbacks → 然後 fw_download()
  - hwsim 在 fw_download() 返回前不得呼叫 callbacks
- [ ] **hwsim_ops return code documentation [M13]:**
  - 所有函式加 kernel-doc 註釋說明 return value
- [ ] **Compile check:** bus shim stub + hwsim stub 均可編譯
- **驗收條件:**
  - `sim_bus_if.h` 可被 bus shim 和 hwsim 模組同時 `#include` 無錯誤
  - 兩個模組均編譯通過
- **Commits:**
  - `brcmfmac: add sim_bus_if.h shim-to-hwsim interface definition`
- **Status:** pending

### Phase 14: Mocked Bus Module — Core Engine (`brcmfmac_hwsim.ko`)
- [ ] Module init/exit, device registration
- [ ] BCDC dcmd parser: extract cmd, reqid, ifidx, set/get flag, payload
- [ ] IOCTL/iovar command handler table [B1 完整追蹤]:
  - **Probe-FATAL (必須正確處理，否則 probe 失敗) [D3-D5 修正]:**
    - `cur_etheraddr` (GET) — 回傳模擬 MAC
    - `ver` (GET) — 回傳 `"wl0: Oct 01 2024 brcmfmac-hwsim-1.0"`
    - `mpc` (SET) — 接受 (power management control)
    - `event_msgs` (GET) — 回傳 `BRCMF_E_LAST/8` bytes zero buffer (27 bytes) [H8]
    - `event_msgs` (SET) — 存儲 event mask，用於過濾事件
    - `SCAN_CHANNEL_TIME` / `SCAN_UNASSOC_TIME` (SET) — 接受
  - **Important but Non-fatal (error 降級但不中斷 probe) [D3-D5,D7 新分類]:**
    - `BRCMF_C_GET_REVINFO` (GET) — 回傳模擬 chip revision info（error → "UNKNOWN"）
    - `cap` (GET) — 回傳 `"sta wl 11n 11ac 11d 11h mfp"` [B7]（error → 無 feature）
    - `clmver` (GET) — 回傳 `"API: 0.0 Data: hwsim.0"`（error → 只 TRACE log）
    - `join_pref` (SET) — 接受 & 存儲（error → 只 log）
    - `txbf` (SET) — 接受（error → 顯式忽略）[D13]
  - **Feature detection (brcmf_feat_attach — 非 fatal 但影響功能) [D2,D12 BCME codes]:**
    - `pfn` — BCDC status=-23 (BCME_UNSUPPORTED, disable PNO)
    - `wowl` / `wowl_cap` — BCDC status=-23
    - `rsdb_mode` — BCDC status=-23
    - `tdls_enable` — BCDC status=-23
    - `mfp` — 回傳 1 (enable MFP)
    - `sup_wpa` — BCDC status=-23
    - `wlc_ver` — 回傳 version struct
    - `scan_ver` — 回傳預設 scan version
  - **Scan 相關 [B2]:**
    - `escan` (SET) — 接受 scan request → 觸發 scan event generator
    - `chanspec` / `bw_cap` (GET) — 回傳模擬 channel data (至少 ch1/2.4GHz) [M2]
    - `rxchain` (GET) — 回傳 1 (single chain)
    - `nmode` / `vhtmode` (GET) — 回傳 1 / 1
  - **Connect/Association 相關 [B3]:**
    - `join` bsscfg (SET) — 觸發 connect event flow
    - `BRCMF_C_SET_SSID` (SET) — fallback connect
    - `wpa_auth` / `auth` / `wsec` / `wpaie` bsscfg (SET) — 接受 & 存儲
  - **AP mode 相關 [B4]:**
    - `BRCMF_C_SET_AP` / `BRCMF_C_SET_INFRA` / `BRCMF_C_UP` / `BRCMF_C_DOWN` (SET) — 接受
    - `SET_SSID` in AP mode → 觸發 `BRCMF_E_SET_SSID` event
    - `chanspec` (SET) — 設定 AP channel
    - `SET_BCNPRD` / `SET_DTIMPRD` / `closednet` (SET) — 接受
  - **Runtime (cfg80211 setup, 不在 probe path) [D11]:**
    - `country` (SET/GET) — 接受 & 回傳預設 country code
  - **Default fallback [D2]:** 未知 GET → BCDC response: flags|=ERROR, status=-23 (BCME_UNSUPPORTED); 未知 SET → status=0 (success)
- [ ] **Scan event generator [B2]:**
  - escan iovar → 排程 workqueue → 產生 `BRCMF_E_ESCAN_RESULT` (status=PARTIAL) × N BSS
  - 最後產生 `BRCMF_E_ESCAN_RESULT` (status=SUCCESS) 結束 scan
  - 每個 partial result 包含有效 `brcmf_escan_result_le` + `brcmf_bss_info_le`
  - 預設: 1 個模擬 BSS (SSID="HWSIM-AP", ch=1, signal=-50dBm)
  - 所有 events 必須在 10 秒內完成（scan timeout）
- [ ] **Connect event generator [B3]:**
  - join iovar → delay (模擬 auth/assoc) → `BRCMF_E_LINK` (flags bit 0 = 1, status=0, addr=BSSID)
  - 或 `BRCMF_E_SET_SSID` (status=SUCCESS) 備用
- [ ] **AP mode event generator [B4]:**
  - SET_SSID in AP mode → `BRCMF_E_SET_SSID` event
  - 可選: `BRCMF_E_IF` event 建立 wlan1 [M11]
- [ ] Data packet TX→RX converter (strip TX header, add RX header, route by ifidx/DA)
- [ ] Firmware event generator (BRCMF_E_IF, BRCMF_E_LINK, BRCMF_E_ASSOC, etc.)
  - **Event codes 必須 < 213 (WCC range)** [H14]
  - 使用存儲的 event_msgs mask 過濾事件
- [ ] Firmware download acceptance handler
  - `fw_download(ctx, NULL, 0, NULL, 0)` → 設定 `fw_state = HWSIM_FW_BOOTED`
- [ ] `EXPORT_SYMBOL_GPL()` 匯出（不帶 namespace，避免 `symbol_get` 問題）[E7]: `brcmf_hwsim_get_ops`, `brcmf_hwsim_put_ops`
- [ ] Per-device state tracking (MAC, interfaces, association state, AP mode state, event mask)
- [ ] Device probe: 使用 Broadcom VID + fake PID
  - chip ID 考慮用 `0xFFFF` 避免觸發 chip-specific code paths [M15]
- [ ] `maxctl = 8192` (同真實 SDIO) [M1]
- [ ] **Compile check:** `make M=.../brcmfmac/hwsim modules` 通過，`brcmfmac_hwsim.ko` 產生
- **驗收條件:**
  - `brcmfmac_hwsim.ko` 成功產生
  - `nm brcmfmac_hwsim.ko | grep 'T brcmf_hwsim'` 顯示匯出符號
  - `modinfo brcmfmac_hwsim.ko` 顯示正確模組資訊
- **Commits:**
  - `brcmfmac/hwsim: add module skeleton with init/exit and device registration`
  - `brcmfmac/hwsim: add BCDC dcmd parser and response builder`
  - `brcmfmac/hwsim: add ioctl/iovar command handler table with default fallback`
  - `brcmfmac/hwsim: add data packet TX-to-RX converter`
  - `brcmfmac/hwsim: add firmware event generator`
  - `brcmfmac/hwsim: add firmware download acceptance handler`
- **Status:** pending

### Phase 15: SDIO+BCDC Bus Shim (`sim_sdio.c`)
- [ ] **`platform_device` lifecycle [H10]:**
  - name: `"brcmfmac-sim-sdio"` (唯一)
  - `brcmf_sim_sdio_register()`: `platform_driver_register()` + `platform_device_register()`
  - `brcmf_sim_sdio_exit()`: `platform_device_unregister()` + `platform_driver_unregister()`
  - Error: `platform_device_register()` 失敗 → unregister driver → return error
- [ ] Implement `brcmf_bus_ops`: all ops forward to mocked bus module
  - **必須實作 `get_blob`** 返回 `-ENOENT`，不修改 fw pointer [E1 BLOCKER, H13]
- [ ] `txctl`: forward raw BCDC dcmd buffer to mocked bus, wake ctrl_wait on response
- [ ] `rxctl`: return stored response from mocked bus, **回傳值為 byte count（正數）或 -ETIMEDOUT** [D10]
- [ ] `txdata`: forward data skb to mocked bus, **完成後呼叫 `brcmf_proto_bcdc_txcomplete()`** [E6, D1, H5]
  - hwsim 返回 error → shim 仍呼叫 `brcmf_proto_bcdc_txcomplete(dev, skb, false)`
- [ ] `preinit`/`stop`/`remove`: lifecycle management
  - **完整 teardown protocol [B5, E9, D14]:**
    1. 設定 `shutting_down = true`
    2. 呼叫 `hwsim_ops->detach()` 通知 hwsim 停止 callbacks
    3. flush 所有 workqueues
    4. `brcmf_release_module_param(settings)` [D14] 釋放 settings 記憶體
    5. `symbol_put()` 釋放 hwsim 模組引用
    - 測試：`rmmod brcmfmac_hwsim` while brcmfmac loaded → "module in use"
- [ ] `debugfs_create`: 暴露 bus state、ioctl count、tx/rx 計數 [M3]
- [ ] Interrupt handler: receive notifications from mocked bus, call `brcmf_rx_frame()` or wake waitqueues
- [ ] Probe flow [D6, D8, D9 修正]:
  1. `symbol_get("brcmf_hwsim_get_ops")` → 取得 hwsim ops (失敗 → 乾淨退出) [D16: 需 extern prototype]
  2. **register callbacks with hwsim** BEFORE fw_download [M12]
  3. alloc `bus_if` (kzalloc) + setup: chip=0xFFFF [D9], chiprev=0, proto_type=BCDC, bus_type=SIM, fwvid=WCC, maxctl=8192 [M1]
  4. **`dev_set_drvdata(&pdev->dev, bus_if)`** [D8] — MUST be before brcmf_alloc()
  5. `hwsim_ops->fw_download(ctx, NULL, 0, NULL, 0)` → 模擬 firmware ready
  6. bus_if->ops = &sim_sdio_bus_ops
  7. **`settings = brcmf_get_module_param(&pdev->dev, BRCMF_BUSTYPE_SIM, 0xFFFF, 0)`** [D6]
  8. `brcmf_alloc(&pdev->dev, settings)` + `brcmf_attach(&pdev->dev)` → wlan0 created
  9. **完全繞過 `brcmf_fw_get_firmwares()`** — 不需要 firmware/NVRAM/CLM 檔案
  10. `get_blob()` 對所有 blob type 返回 `-ENOENT` → kernel graceful skip
- [ ] **Failure mode:** hwsim 未載入時 `symbol_get()` 返回 NULL → probe 不建立 device（乾淨退出，非 OOPS）
- [ ] **Compile check:** `make M=.../brcmfmac modules` 通過，`brcmfmac.ko` 包含 sim_sdio
- **驗收條件 [H1]:**
  - **PASS:** `nm brcmfmac.ko | grep sim_sdio` 顯示 shim 符號
  - **PASS:** `insmod brcmfmac-wcc.ko && insmod brcmfmac_hwsim.ko && insmod brcmfmac.ko && ip link show wlan0` 成功 [E4]
  - **PASS:** `dmesg | grep "no clm_blob available"` 存在 [H13, M10]
  - **PASS:** `dmesg | grep "brcmfmac-hwsim"` 存在
  - **FAIL:** `dmesg | grep -E 'brcmfmac.*(error|BUG|WARN|OOPS)'` 有任何匹配
- **Commits:**
  - `brcmfmac: add SDIO+BCDC bus shim with platform_device probe`
  - `brcmfmac: implement sim_sdio txctl/rxctl/txdata forwarding to hwsim module`
  - `brcmfmac: add sim_sdio interrupt handling and lifecycle management`
- **Status:** pending

### Phase 16: PCIe+MSGBUF Bus Shim (`sim_pcie.c`) — **獨立 Milestone，不在本期 Scope**
- 需在 SDIO+BCDC 完全驗證後才啟動
- PCIe+MSGBUF 需模擬 shared memory rings, DMA buffer, doorbell writes, 6 種 ring type
- 預估複雜度為 SDIO 的 3-5 倍
- 另行規劃
- **Status:** deferred

## Known Limitations (Milestone 1) [M17]
以下功能在 Milestone 1 中 **不支援**：
1. **真實掃描結果** — mock 只產生 1 個模擬 BSS (HWSIM-AP)，無法回傳真實 AP
2. **真實 association** — connect event 為模擬，無真正 802.11 auth/assoc 交換
3. **真實資料傳輸** — 資料在 hwsim 內 loopback，不連接外部網路
4. **WPA/WPA2 金鑰交換** — 無 4-way handshake 事件（wpa_supplicant 可連線但 key 不真實）
5. **Roaming** — 無 BSS transition events
6. **Power management / WoWL** — wowl_config 為 NULL stub
7. **PCIe+MSGBUF** — Phase 16 deferred
8. **Multi-channel operation** — mock 只有 1 個 channel
9. **DFS / radar detection** — 不模擬 regulatory events
10. **Firmware coredump** — memdump ops 為 stub

### Phase 17: Fault Injection & Control Plane
- [ ] **debugfs interface [M9]:**
  ```
  /sys/kernel/debug/brcmfmac_hwsim/
  ├── fault_ioctl_cmd    # echo "262 -5" → next ioctl cmd 262 returns -EIO
  ├── fault_event        # echo "1 0 1" → emit BRCMF_E_LINK(status=0, flags=1)
  ├── fault_delay_ms     # echo "500" → add 500ms to all responses
  ├── fault_reqid_mangle # echo "1" → corrupt next reqid in response [H6]
  ├── fault_event_oob    # echo "250" → emit out-of-range event code [H14]
  └── stats              # cat → ioctl_count, tx_count, rx_count, event_count
  ```
- [ ] Faults 為 one-shot（觸發後自動清除），除了 delay 和 stats
- [ ] Per-ioctl error injection: by cmd number (not name)
- [ ] Event injection: emit specific BRCMF_E_* events on demand
- [ ] Delay injection: configurable response latency (ms)
- [ ] Malformed response injection (wrong reqid [H6], truncated data, bad status)
- [ ] Firmware crash simulation
- **驗收條件 [H1]:**
  - **PASS:** `echo "262 -5" > .../fault_ioctl_cmd` → 下一次 ioctl 262 返回 -EIO
  - **PASS:** `cat .../stats` 顯示正確 ioctl/tx/rx/event 計數
  - **PASS:** out-of-range event code (≥213) 不造成 crash
  - **FAIL:** 任何 fault injection 導致 kernel OOPS
- **Commits:**
  - `brcmfmac/hwsim: add debugfs fault injection control plane`
- **Status:** pending

### Phase 18: Testing & Validation
- [ ] Build brcmfmac with `CONFIG_BRCMFMAC_HWSIM_SDIO=y`, load `brcmfmac_hwsim.ko`
- [ ] **Module load script (`test_hwsim.sh`):**
  ```bash
  insmod cfg80211.ko       # if not built-in
  insmod brcmfmac-wcc.ko   # [M7] WCC 必須先載入
  insmod brcmfmac_hwsim.ko
  insmod brcmfmac.ko
  ```
- [ ] **基本驗證 [H1]:**
  - **PASS:** `ip link show wlan0` 存在
  - **PASS:** `ls /sys/class/ieee80211/` 有 phy entry [M16]
  - **PASS:** `iw phy | grep -c 'MHz'` ≥ 1 (至少有 channel) [M2]
  - **PASS:** `dmesg | grep "no clm_blob available"` [M10]
  - **PASS:** `dmesg | grep "brcmfmac-hwsim"` [M10]
  - **FAIL:** `dmesg | grep -E 'brcmfmac.*(error|BUG|WARN|OOPS)'`
- [ ] **wpa_supplicant 測試:**
  - **PASS:** `wpa_cli -i wlan0 status` 回傳 `wpa_state=DISCONNECTED` within 5s
  - **PASS:** `wpa_cli -i wlan0 scan && wpa_cli -i wlan0 scan_results` 回傳至少 1 BSS [B2]
- [ ] **hostapd 測試:**
  - **PASS:** `hostapd -i wlan0 conf/minimal_ap.conf -B` 啟動成功
  - **PASS:** AP mode ioctls 不產生 OOPS [B4]
- [ ] **Data loopback 測試 [M4]:**
  - 100 ICMP packets wlan0→wlan1, verify 100 received
  - `ip -s link show wlan0` counters 驗證
- [ ] **Negative testing — module load ordering [H2]:**
  ```bash
  # Case 1: brcmfmac without hwsim — EXPECT: no wlan0, no OOPS
  insmod brcmfmac.ko && ! ip link show wlan0 && rmmod brcmfmac

  # Case 2: brcmfmac without WCC — EXPECT: clean -EIO [H12]
  insmod brcmfmac_hwsim.ko && insmod brcmfmac.ko && dmesg | grep -q "error" && rmmod brcmfmac && rmmod brcmfmac_hwsim

  # Case 3: rmmod hwsim while brcmfmac loaded — EXPECT: "module in use" [B5]
  insmod brcmfmac-wcc.ko && insmod brcmfmac_hwsim.ko && insmod brcmfmac.ko
  rmmod brcmfmac_hwsim  # should FAIL
  ```
- [ ] **Concurrent traffic test [H7]:**
  - Continuous `ping -f -c 1000 ...` + `wpa_cli` ioctls 同時執行
  - Scan while data is flowing
- [ ] **Stress testing [H4]:**
  - Tier 1 (Quick smoke): 10 cycles, 1s sleep
  - Tier 2 (Standard): 100 cycles, no sleep
  - Tier 3 (Race): 1000 cycles, random 0-100ms sleep, concurrent `ip link` queries
- [ ] **Lifecycle stress [M14]:**
  - start wpa_supplicant → initiate scan → `rmmod brcmfmac` during scan → verify no OOPS
- [ ] **KASAN/kmemleak/lockdep testing [H3]:**
  - NUC 以 `CONFIG_KASAN=y`/`CONFIG_DEBUG_KMEMLEAK=y`/`CONFIG_LOCKDEP=y` 重新編譯 kernel
  - 執行完整測試 suite under each config
  - `kmemleak scan` + `kmemleak clear` 加入 test script
- [ ] **Fault injection scenarios:**
  - wrong reqid response [H6] → verify driver retry then -EINVAL
  - out-of-range event code [H14] → verify no crash
  - ioctl error during probe → verify clean failure
- [ ] Build with `CONFIG_BRCMFMAC_HWSIM_PCIE=y` → **deferred (Milestone 2)**
- [ ] 建立自動化測試腳本 `test_hwsim.sh`
- **Commits:**
  - `test: add automated hwsim module load/unload and wlan0 verification script`
  - `test: add wpa_supplicant, hostapd, stress and negative test cases`
- **Status:** pending

## Agent Dispatch Strategy

### 設計原則
- Kernel 程式碼高度關聯，過度平行化會導致不一致
- 基礎建設（Kconfig/IPC）必須先完成才能分發
- 新檔案（hwsim/ 模組內）可平行撰寫
- 修改既有檔案（bus.h, core.c, sim_sdio.c）需要序列化處理

### Wave 1：基礎建設（我自己做，序列）
| 步驟 | Phase | 工作內容 | 理由 |
|------|-------|---------|------|
| 1 | 11.5 | `git init` + dev branch | 簡單 bash，無需 agent |
| 2 | 12 | Kconfig + Makefile + bus.h + core.c | 觸及多個既有檔案，需一致性 |
| 3 | 13 | `sim_bus_if.h` IPC 介面設計 | 後續所有工作的根基 |

### Wave 2：核心模組（平行 agents，Phase 13 完成後啟動）
| Agent | 工作內容 | 產出檔案 | 依賴 |
|-------|---------|---------|------|
| **Agent A** (general-purpose) | hwsim 模組骨架 + BCDC parser + FW download handler | `hwsim_core.c/h`, `hwsim_bcdc.c/h`, `hwsim_fwdl.c/h` | Phase 13 完成 |
| **Agent B** (general-purpose) | ioctl/iovar handler table + default fallback | `hwsim_fw.c/h` | Phase 13 完成 |
| **Agent C** (general-purpose) | Data TX→RX converter + Event generator | `hwsim_data.c/h`, `hwsim_event.c/h` | Phase 13 完成 |

> 三個 agent 各自撰寫獨立 .c/.h 檔案，不觸及相同檔案 → 無衝突。
> 我在 Wave 2 完成後整合、review、逐一 commit。

### Wave 3：Bus Shim 整合（我自己做，序列）
| 步驟 | Phase | 工作內容 | 理由 |
|------|-------|---------|------|
| 7 | 15 | `sim_sdio.c` — probe, forwarding, interrupt, lifecycle | 需與 Wave 2 產物整合，觸及既有 bus.h/core.c |

> 必須在 Wave 2 所有 agent 完成後才開始，確保 hwsim 模組 API 確定。

### Wave 4：收尾（可平行 agents）
| Agent | Phase | 工作內容 |
|-------|-------|---------|
| **Agent D** (general-purpose) | 17 | `hwsim_inject.c` debugfs 故障注入 |
| **Agent E** (general-purpose) | 18 | `test_hwsim.sh` 自動化測試腳本 |

> Phase 17 和 18 獨立於彼此，可平行。

### 流程圖
```
Wave 1 (序列，我做)
  Phase 11.5 → Phase 12 → Phase 13
                                |
Wave 2 (平行 agents)            v
  Agent A (core+bcdc+fwdl) ─────┐
  Agent B (fw/ioctl)        ────┤── 我整合 + review + commit
  Agent C (data+event)      ────┘
                                |
Wave 3 (序列，我做)              v
  Phase 15 (sim_sdio.c)
                                |
Wave 4 (平行 agents)            v
  Agent D (fault inject) ──────┐
  Agent E (test script)  ──────┘── 我 review + commit
```

### 品質關卡
- **Wave 2 → Wave 3 之間:**
  1. 我 review 所有 agent 產出，確保 API 一致、coding style 統一、kernel coding conventions 遵守
  2. **啟動 code-review agent** 做獨立 code review（檢查 bugs、security、logic errors）
  3. 修正 review 發現的問題後才進入 Wave 3
- **Wave 3 完成後:**
  1. 整合編譯驗證（在 NUC 上 `make M=`）
  2. **啟動 code-review agent** review sim_sdio.c 與 hwsim 模組的整合
- **Wave 4 完成後:** 完整 runtime 驗證

### Review Agent 部署計畫
| 時間點 | Review Agent | Review 範圍 | 重點 |
|--------|-------------|------------|------|
| Wave 1 完成後 | **Plan Review** (general-purpose) | Phase 12-13 產出 vs. task_plan.md | Kconfig 互斥邏輯、`sim_bus_if.h` 介面完整性、與原始設計一致性 |
| Wave 2 完成後 | **Code Review** (code-review) | hwsim/ 所有 .c/.h | Kernel coding style、NULL deref、locking correctness、BCDC format 正確性 |
| Wave 3 完成後 | **Code Review** (code-review) | sim_sdio.c + 整體整合 | 跨模組 API 呼叫正確性、race condition、error path、module unload safety |
| Wave 4 完成後 | **Code Review** (code-review) | hwsim_inject.c + test_hwsim.sh | debugfs 安全性、測試覆蓋完整性 |
| 全部完成後 | **Final Review** (general-purpose) | 全部修改 vs. task_plan.md | 架構一致性 audit、遺漏功能、open issues |

### Context Window 隔離策略
每個 agent 在**獨立的 context window** 中執行，互不干擾：
- ✅ Coding Agent A/B/C 各自獨立 context → 不會互相污染
- ✅ Code Review Agent 獨立 context → 拿到乾淨的 diff 來 review
- ✅ 我（主 orchestrator）保持自己的 context，只做整合和 commit
- 啟動 agent 時提供完整的必要 context（sim_bus_if.h 內容、architecture design、coding conventions）

### LLM Model 分配
| Agent 角色 | Agent Type | Model | 理由 |
|-----------|-----------|-------|------|
| **我（主 orchestrator）** | — | Claude Opus 4.6 | 架構決策、整合、品質把關需要最強推理 |
| **Coding Agent A/B/C** (Wave 2) | general-purpose | Claude Sonnet 4.6 | Kernel C coding 需要高品質但 Sonnet 已足夠，比 Opus 快 2-3x |
| **Coding Agent D/E** (Wave 4) | general-purpose | Claude Sonnet 4.6 | 同上 |
| **Code Review Agent** | code-review | **Claude Opus 4.6** (override) | Kernel 程式碼 review 需要最深度分析：race condition、locking bug、use-after-free 等 |
| **Plan Review Agent** | general-purpose | Claude Sonnet 4.6 | 設計一致性檢查，Sonnet 已足夠 |
| **Final Review Agent** | general-purpose | **Claude Opus 4.6** | 全面架構 audit 需要最強推理 |

> **為什麼不全用 Opus?**
> - Opus 推理品質最高但速度最慢。Coding task 用 Sonnet 可在品質與速度間取得平衡。
> - Review task 用 Opus 是因為找 bug 比寫程式更需要深度推理。
> - 如果預算不是問題，可以全部改用 Opus。

## Git Repository & Commit Strategy

### Repository 結構
- **本地 dev branch 追蹤**（不推送 GitHub）
- `linux-6.12.81/` — `git init` + `hwsim-dev` branch，baseline commit 為原始 tarball
- `hostap/` — 已有 git repo，checkout `hwsim-dev` branch
- 所有實作修改 commit 到各自的 dev branch

### 初始化步驟
```bash
# linux-6.12.81: 建立 git repo + dev branch
cd /Users/carterchan/working/coding/hwsim/linux-6.12.81
git init
git add -A
git commit -m "baseline: Linux 6.12.81 original source"
git checkout -b hwsim-dev

# hostap: 建立 dev branch (已是 git repo)
cd /Users/carterchan/working/coding/hwsim/hostap
git checkout -b hwsim-dev
```

### Commit 規範
- 每個邏輯修改獨立一個 commit（不做 squash）
- Commit message 格式：`<scope>: <description>`
  - scope 範例：`brcmfmac`, `brcmfmac/hwsim`, `build`, `test`, `docs`
- 所有 commit 附加 `Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>`
- 本地 branch 追蹤，不推送 GitHub

### 預計 Commit 清單（約 17 個 commits — 不含 Phase 16 PCIe）
| Phase | Commit | 描述 |
|-------|--------|------|
| 11.5 | `baseline: Linux 6.12.81 original source` | 初始化 linux repo |
| 12 | `brcmfmac: add CONFIG_BRCMFMAC_HWSIM_SDIO Kconfig option (XOR with real SDIO)` | Kconfig 互斥配置 |
| 12 | `brcmfmac: add hwsim bus shim build rules to Makefile` | Makefile 修改 |
| 12 | `brcmfmac: add sim bus registration stubs in bus.h and core.c` | bus.h + core.c 修改 |
| 12 | `brcmfmac/hwsim: add Kconfig and Makefile for brcmfmac_hwsim module` | hwsim/ 建置系統 |
| 13 | `brcmfmac: add sim_bus_if.h shim-to-hwsim interface definition` | IPC 介面 + locking model |
| 14 | `brcmfmac/hwsim: add module skeleton with init/exit and device registration` | 模組骨架 |
| 14 | `brcmfmac/hwsim: add BCDC dcmd parser and response builder` | BCDC 解析 |
| 14 | `brcmfmac/hwsim: add ioctl/iovar command handler table with default fallback` | ioctl 處理 + fallback |
| 14 | `brcmfmac/hwsim: add data packet TX-to-RX converter` | 資料轉換 |
| 14 | `brcmfmac/hwsim: add firmware event generator` | 事件產生器 |
| 14 | `brcmfmac/hwsim: add firmware download acceptance handler` | 韌體下載 |
| 15 | `brcmfmac: add SDIO+BCDC bus shim with platform_device probe` | SDIO shim probe |
| 15 | `brcmfmac: implement sim_sdio txctl/rxctl/txdata forwarding` | SDIO 資料轉發 |
| 15 | `brcmfmac: add sim_sdio interrupt handling and lifecycle` | SDIO 中斷/生命週期 |
| 17 | `brcmfmac/hwsim: add debugfs fault injection control plane` | 故障注入 |
| 18 | `test: add automated hwsim verification script` | 測試腳本 |

## Build & Verification Plan

### Build Environment
- **編譯環境:** Intel NUC，x86_64 native 編譯（user 負責 NUC 設定）
- **目標架構:** x86_64 (amd64)
- **驗證環境:** 同一台 NUC（編譯 + 執行同機器，避免 vermagic 不匹配）
- **本機:** Mac Studio M3 Ultra (arm64)，僅用於程式碼撰寫，不做編譯

### 編譯驗證步驟 (每個 Phase 完成後，在 NUC 上)

#### Phase 12 完成後：Kconfig/Build 驗證
```bash
# 在 NUC 上
cd /path/to/linux-6.12.81
make defconfig
# 啟用 hwsim SDIO
scripts/config --module CONFIG_BRCMFMAC
scripts/config --enable CONFIG_BRCMFMAC_HWSIM_SDIO
make olddefconfig
# 驗證配置正確
grep BRCMFMAC .config
# 應顯示: CONFIG_BRCMFMAC=m, CONFIG_BRCMFMAC_HWSIM_SDIO=y, CONFIG_BRCMFMAC_PROTO_BCDC=y
# 驗證互斥: CONFIG_BRCMFMAC_SDIO 不存在或未設定
grep BRCMFMAC_SDIO .config  # 不應出現 =y
# 編譯僅 brcmfmac 模組 (快速驗證語法/連結)
make M=drivers/net/wireless/broadcom/brcm80211/brcmfmac modules
```

#### Phase 13 完成後：介面編譯驗證
```bash
# sim_bus_if.h 可被 bus shim 和 hwsim 模組同時引用
make M=drivers/net/wireless/broadcom/brcm80211/brcmfmac modules
make M=drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim modules
# 兩者都應無錯誤
```

#### Phase 14 完成後：Mocked Bus 模組編譯驗證
```bash
# 編譯 hwsim 模組
make M=drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim modules
# 驗證 brcmfmac_hwsim.ko 產生
ls -la drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim/brcmfmac_hwsim.ko
# 檢查 exported symbols
nm drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim/brcmfmac_hwsim.ko | grep -i 'T brcmf_hwsim'
```

#### Phase 15 完成後：SDIO Bus Shim + 模組整合編譯驗證
```bash
# 完整編譯 brcmfmac + hwsim
make M=drivers/net/wireless/broadcom/brcm80211 modules
# 驗證兩個 .ko 檔都存在
ls -la drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko
ls -la drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim/brcmfmac_hwsim.ko
# 檢查 brcmfmac.ko 包含 sim_sdio 符號
nm drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko | grep sim_sdio
# modinfo 檢查
modinfo drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko
modinfo drivers/net/wireless/broadcom/brcm80211/brcmfmac/hwsim/brcmfmac_hwsim.ko
```

### 運行時驗證步驟 (在同一台 NUC 上)

#### 基本功能驗證
```bash
# 1. 載入依賴模組
sudo modprobe cfg80211
sudo modprobe mac80211  # 如果需要

# 2. 載入 WCC vendor module [D15]
sudo insmod /tmp/brcmfmac-wcc.ko

# 3. 載入 mocked bus 模組（必須先於 brcmfmac）
sudo insmod /tmp/brcmfmac_hwsim.ko
dmesg | tail -20  # 確認無錯誤

# 4. 載入 brcmfmac（會觸發 bus shim probe）
sudo insmod /tmp/brcmfmac.ko
dmesg | tail -30  # 確認 probe 成功

# 5. 驗證 wlan0 已建立
ip link show wlan0
# 期望: wlan0 存在且 state DOWN

# 6. 驗證 MAC 位址
ip link show wlan0 | grep ether
# 期望: 顯示 sim_fw 設定的 MAC 位址

# 7. 驗證 phy 裝置
iw phy
# 期望: 顯示模擬的 phy 裝置資訊
```

#### wpa_supplicant 驗證
```bash
# 8. 啟動 wpa_supplicant 控制介面
sudo wpa_supplicant -i wlan0 -D nl80211 -c /etc/wpa_supplicant.conf -B
# 期望: 成功啟動，無 "driver not supported" 錯誤

# 9. 透過 wpa_cli 確認連線
sudo wpa_cli -i wlan0 status
# 期望: 顯示 driver=nl80211, wpa_state=DISCONNECTED

# 10. 測試 scan 指令
sudo wpa_cli -i wlan0 scan
# 期望: 成功送出（即使 scan 結果為空）
```

#### hostapd 驗證
```bash
# 11. 建立最小 hostapd 配置
cat > /tmp/hostapd.conf << 'EOF'
interface=wlan0
driver=nl80211
ssid=hwsim_test
hw_mode=g
channel=1
EOF

# 12. 啟動 hostapd
sudo hostapd /tmp/hostapd.conf -B
# 期望: 成功啟動 AP 模式（或至少不因 bus 層錯誤而崩潰）
```

#### 模組卸載驗證
```bash
# 13. 卸載順序（反向）
sudo rmmod brcmfmac
sudo rmmod brcmfmac_hwsim
dmesg | tail -20  # 確認 clean removal，無 BUG/WARN/OOPS
```

#### 壓力測試
```bash
# 14. 反覆載入/卸載
for i in $(seq 1 10); do
  sudo insmod /tmp/brcmfmac_hwsim.ko
  sudo insmod /tmp/brcmfmac.ko
  sleep 1
  ip link show wlan0 > /dev/null 2>&1 || echo "FAIL: wlan0 not found at iteration $i"
  sudo rmmod brcmfmac
  sudo rmmod brcmfmac_hwsim
done
echo "Load/unload stress test complete"
```

### 自動化驗證腳本
Phase 18 會建立一個 `test_hwsim.sh` 腳本，自動執行上述所有驗證步驟，並輸出 PASS/FAIL 報告。腳本將：
1. 檢查編譯產物存在
2. 檢查模組 modinfo/nm 符號
3. 在遠端機器上執行載入/wlan0/wpa_supplicant/hostapd/卸載測試
4. 收集 dmesg 日誌
5. 輸出結構化測試報告

## Key Architecture Decisions
| Decision | Rationale |
|----------|-----------|
| Two-component design: bus shim + separate mocked bus module | User requirement: the "mocked bus" is a separate entity that receives/processes packets |
| IPC: `symbol_get()` / `symbol_put()` | 最簡單的 kernel module 間通訊方式，hwsim 未載入時乾淨失敗 |
| Kconfig 互斥: HWSIM_SDIO XOR real SDIO | 避免同時編譯兩個 SDIO 實作造成衝突 |
| Broadcom VID + fake PID | 維持 vendor 相容性，fake PID 識別模擬裝置 |
| Default ioctl fallback | GET→BCDC status=-23 (BCME_UNSUPPORTED), SET→status=0，避免遺漏 ioctl 導致 probe 失敗 [D2] |
| Thread safety: completion + spinlock + RCU | ctrl path 用 completion，rxctl 用 spinlock，ops 指標用 RCU |
| Bus shim does NOT parse packet content | Separation of concerns: shim 轉發，hwsim 解析 |
| `platform_device` for bus shim probe | No real SDIO/MMC or PCI subsystem dependency needed |
| SDIO+BCDC first, PCIe+MSGBUF deferred | SDIO 複雜度低，PCIe 為獨立 milestone |
| `fwvid = BRCMF_FWVENDOR_WCC` | 最簡單 vendor ops，最常見 |
| Skip `brcmf_fw_get_firmwares()` in mock path | No firmware files needed; directly probe |
| Local git branches, no GitHub push | 避免 1.6GB kernel source push 問題 |
| Intel NUC native compile | 避免 arm64→x86_64 交叉編譯問題和 vermagic 不匹配 |
| Workqueue-based interrupt simulation | No real IRQ needed; deterministic, debuggable |

## File Map
```
drivers/net/wireless/broadcom/brcm80211/brcmfmac/
├── sim_sdio.c            # SDIO+BCDC bus shim (inside brcmfmac, gated by Kconfig)
├── sim_sdio.h
├── sim_pcie.c            # PCIe+MSGBUF bus shim (inside brcmfmac, gated by Kconfig)
├── sim_pcie.h
├── sim_bus_if.h          # Internal API: shim ↔ mocked bus module interface
├── Kconfig               # (modified) +CONFIG_BRCMFMAC_HWSIM_SDIO/PCIE
├── Makefile              # (modified) +sim_sdio.o, +sim_pcie.o
├── bus.h                 # (modified) +sim member, +register/exit declarations
├── core.c                # (modified) +sim register/exit calls
├── hwsim/                # Mocked bus module directory
│   ├── Kconfig
│   ├── Makefile
│   ├── hwsim_core.c      # Module init, device management, registration
│   ├── hwsim_core.h
│   ├── hwsim_bcdc.c      # BCDC dcmd parser + response builder
│   ├── hwsim_bcdc.h
│   ├── hwsim_msgbuf.c    # MSGBUF message parser + completion builder
│   ├── hwsim_msgbuf.h
│   ├── hwsim_fw.c        # Fake firmware: ioctl handler table, state machine
│   ├── hwsim_fw.h
│   ├── hwsim_data.c      # Data packet TX→RX converter
│   ├── hwsim_data.h
│   ├── hwsim_event.c     # Firmware event generator
│   ├── hwsim_event.h
│   ├── hwsim_fwdl.c      # Firmware download acceptance handler
│   ├── hwsim_fwdl.h
│   ├── hwsim_inject.c    # Fault injection via debugfs
│   └── hwsim_inject.h
```

## Probe Flow (SDIO Mock — Option A)
```
1. insmod brcmfmac_hwsim.ko
   → hwsim_core_init(): register as hwsim bus provider (export symbols)

2. brcmfmac module loads (CONFIG_BRCMFMAC_HWSIM_SDIO=y)
   → brcmfmac_module_init()
     → brcmf_core_init()
       → brcmf_sim_sdio_register()          [gated by CONFIG_BRCMFMAC_HWSIM_SDIO]
         → platform_device_register("brcmfmac-sim-sdio")
         → brcmf_sim_sdio_probe()           [platform driver probe callback]
           → look up mocked bus module (via exported symbol or registered callback)
           → alloc brcmf_bus + sim_dev
           → bus_if->ops = &brcmf_sim_sdio_bus_ops
           → bus_if->proto_type = BRCMF_PROTO_BCDC
           → bus_if->fwvid = BRCMF_FWVENDOR_WCC
           → bus_if->chip = 0xFFFF, bus_if->chiprev = 0  # [D9]
           → dev_set_drvdata(&pdev->dev, bus_if)          # [D8]
           → settings = brcmf_get_module_param(...)        # [D6]
           → brcmf_alloc(dev, settings)
           → brcmf_attach(dev)
             → brcmf_fwvid_attach()         [binds WCC vops]
             → brcmf_proto_attach()         [selects BCDC]
             → brcmf_fweh_attach()          [event handler]
             → brcmf_bus_started()
               → brcmf_add_if(0, "wlan%d")  ← wlan0 created here
               → brcmf_bus_change_state(BRCMF_BUS_UP)
               → brcmf_bus_preinit()
               → brcmf_c_preinit_dcmds()    [all ioctls go through mocked bus module]
               → brcmf_feat_attach()        [feature ioctls go through mocked bus]
               → brcmf_net_attach()         [registers netdev → wlan0 visible]
```

## IOCTL Command Handling via Mocked Bus Module
```
brcmf_c_preinit_dcmds(ifp) calls:
  1. brcmf_fil_iovar_data_get("cur_etheraddr")  → mocked bus returns configured MAC
  2. brcmf_fil_cmd_data_get(BRCMF_C_GET_REVINFO)→ mocked bus returns simulated revinfo
  3. brcmf_c_process_clm_blob()                  → mocked bus returns success (no real CLM)
  4. brcmf_fil_iovar_data_get("ver")             → mocked bus returns version string
  5. brcmf_fil_iovar_data_get("clmver")          → mocked bus returns CLM version string
  6. brcmf_fil_iovar_int_set("mpc", 1)           → mocked bus returns success
  7. brcmf_fil_iovar_data_get("event_msgs")      → mocked bus returns event mask
  8. brcmf_fil_iovar_data_set("event_msgs")      → mocked bus accepts event mask
  9. brcmf_fil_cmd_int_set(SCAN_CHANNEL_TIME)    → mocked bus returns success
  ...
  All go through:
    bus_ops->txctl() → sim_sdio forwards raw BCDC dcmd to hwsim module
    hwsim module: parse dcmd → look up handler → produce response → trigger interrupt
    bus_ops->rxctl() → sim_sdio returns buffered response
```

## Errors Encountered
| Error | Attempt | Resolution |
|-------|---------|------------|
| None so far | 1 | N/A |

## Notes
- Re-read this plan before any major implementation decision or tool batch.
- The mocked bus module is the "firmware brain" — it must understand BCDC dcmd format, CDC data headers, and all the ioctl/iovar semantics that `brcmf_c_preinit_dcmds()` and `brcmf_feat_attach()` require.
- For data routing: initially support loopback (same device) and multi-interface routing (wlan0→wlan1 on same device). Later: cross-device routing for multi-radio testing.
- The bus shim + mocked bus module interface should be clean enough that the same mocked bus module works with both SDIO and PCIe shims — the module handles protocol framing differences internally.
- Firmware download in Option A: the bus shim skips firmware loading entirely. For future Option B, keep the firmware loading path but make the mocked bus module accept any content.
- The mocked bus module must be loaded BEFORE brcmfmac for the bus shim to find it during probe. Module dependency should enforce this.

## Phase M2-B — hostapd open-system AP on wlan1 (planned)

### Status: planned (design locked)

### Implementation checklist (atomic items)
- [ ] B0: verify bsscfg: prefix is parsed by hwsim_handle_set_var/get_var; if not, add prefix decoder that strips prefix + extracts LE32 bsscfgidx, passes the rest to existing dispatcher with bsscfgidx context.
- [ ] B1: add `struct hwsim_bss bss[2]` to hwsim_dev; init in init_dev.
- [ ] B2: WLC dcmd handlers: C_GET_REGULATORY, C_SET_BCNPRD, C_SET_DTIMPRD, C_DOWN, C_SET_INFRA, C_SET_AP, C_UP, C_SET_SSID.
- [ ] B3: iovar SET handlers (global): mpc, apsta.
- [ ] B4: iovar SET handlers (per-iface ifidx=1): arp_ol, arpoe, ndoe.
- [ ] B5: bsscfg SET handlers (bsscfgidx=1): auth, wsec, wpa_auth, closednet, vndr_ie.
- [ ] B6: chanspec iovar SET handler (decode bw/band/chan; for M2-B store raw value, no validation).
- [ ] B7: build clean, deploy to VM with stale-module-check.
- [ ] B8: write /tmp/hostapd-wlan1.conf (interface=wlan1, driver=nl80211, ssid="brcmsim", channel=6, hw_mode=g, no encryption).
- [ ] B9: run `hostapd -B /tmp/hostapd-wlan1.conf`, verify rc=0 + `hostapd_cli status` shows `state=ENABLED`.
- [ ] B10: dmesg audit (0 error/warn besides clm_blob).
- [ ] B11: 3x stop/start hostapd cycle stable.
- [ ] B12: commit + sync overlay/patches + push.

### Exit criteria
- `hostapd_cli -i wlan1 status` reports `state=ENABLED`
- `iw dev wlan1 info` shows type AP, ssid, channel
- 3 cycles stable

### Open questions / risks
- Does our dispatcher already strip "bsscfg:" prefix? (B0 must answer first.)
- C_SET_SSID payload is `struct brcmf_join_params` — first 4 bytes are SSID len LE32, then 32-byte SSID, then a chunk of zero `params_le` (~64 bytes). We just need to record ssid_len + ssid; rest can be ignored.
- vndr_ie may be sent BEFORE start_ap finishes (during change_beacon callbacks) — return ACK regardless.

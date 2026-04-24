#!/usr/bin/env bash
# brcmfmac/hwsim 標準 regression 測試腳本
#
# 用途：在 VM 內執行，重新載入兩個 module，建立 wlan0 + wlan1 (AP)，
#       啟動 hostapd + wpa_supplicant，做掃描，並檢查 dmesg 是否完全乾淨。
#
# 通過條件 (exit code 0):
#   1. wlan0 + wlan1 介面存在
#   2. hostapd 進入 AP-ENABLED
#   3. wpa_cli scan_results 看到 hostapd 設定的 SSID
#   4. dmesg 沒有 WARN/BUG/oops/stack/panic
#
# 注意：modprobe 順序很重要 — brcmfmac_hwsim 必須在 brcmfmac 之前載入
#       (此需求已透過 MODULE_SOFTDEP 在 common.c 自動處理；本腳本仍顯式
#       照順序載以利舊版 kernel 部署測試)。
set -u
SSID="${SSID:-brcmsim}"
CHAN="${CHAN:-1}"
PASS=0
FAIL=0

log()  { printf '\033[1;36m[REGR]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  PASS\033[0m  %s\n' "$*"; PASS=$((PASS+1)); }
fail() { printf '\033[1;31m  FAIL\033[0m  %s\n' "$*"; FAIL=$((FAIL+1)); }

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    log "需要 root 權限，使用 sudo 重跑"
    exec sudo -E bash "$0" "$@"
  fi
}

cleanup() {
  log "Cleanup"
  for pid in $(pidof hostapd 2>/dev/null) $(pidof wpa_supplicant 2>/dev/null); do
    [ -n "$pid" ] && /bin/kill -9 "$pid" 2>/dev/null || true
  done
  ip link set wlan1 down 2>/dev/null
  ip link set wlan0 down 2>/dev/null
  sleep 1
  # Unload in dependency order: vendor module → brcmfmac → hwsim
  rmmod brcmfmac_wcc    2>/dev/null
  rmmod brcmfmac        2>/dev/null
  rmmod brcmfmac_hwsim  2>/dev/null
  if lsmod | grep -q '^brcmfmac '; then
    log "WARN: brcmfmac still loaded after rmmod; existing wlan ifaces:"
    iw dev 2>/dev/null | grep Interface
  fi
}

require_root "$@"
trap cleanup EXIT

log "Phase 0: reset"
cleanup
dmesg -C
sleep 1

log "Phase 1: load modules (hwsim first)"
modprobe brcmfmac_hwsim || { fail "modprobe brcmfmac_hwsim"; exit 1; }
sleep 1
modprobe brcmfmac       || { fail "modprobe brcmfmac";       exit 1; }
sleep 2
ip link show wlan0 >/dev/null 2>&1 && ok "wlan0 created" || { fail "wlan0 missing"; exit 1; }

log "Phase 2: M2-A — create wlan1 AP iface"
ip link set wlan0 down 2>/dev/null
sleep 1
iw dev wlan0 interface add wlan1 type __ap || { fail "iw add wlan1"; exit 1; }
ip link set wlan0 up
ip link set wlan1 up
sleep 1
ip link show wlan1 >/dev/null 2>&1 && ok "wlan1 created" || { fail "wlan1 missing"; exit 1; }

log "Phase 3: M2-B — start hostapd on wlan1"
cat > /tmp/regr_hostapd.conf <<HC
interface=wlan1
driver=nl80211
ctrl_interface=/var/run/hostapd
ssid=$SSID
hw_mode=g
channel=$CHAN
HC
hostapd -B -P /tmp/regr_hostapd.pid /tmp/regr_hostapd.conf >/tmp/regr_hostapd.log 2>&1
for i in 1 2 3 4 5 6 7 8 9 10; do
  hostapd_cli -i wlan1 status 2>/dev/null | grep -q "state=ENABLED" && break
  sleep 1
done
if hostapd_cli -i wlan1 status 2>/dev/null | grep -q "state=ENABLED"; then
  ok "hostapd AP-ENABLED"
else
  fail "hostapd not ENABLED"; tail -20 /tmp/regr_hostapd.log
fi

log "Phase 4: M2-C — wpa_supplicant scan from wlan0"
cat > /tmp/regr_wpa.conf <<WC
ctrl_interface=/var/run/wpa_supplicant
WC
wpa_supplicant -B -i wlan0 -c /tmp/regr_wpa.conf >/tmp/regr_wpa.log 2>&1
sleep 2
wpa_cli -i wlan0 scan >/dev/null
sleep 4
SCAN_OUT=$(wpa_cli -i wlan0 scan_results 2>/dev/null)
echo "$SCAN_OUT"
if echo "$SCAN_OUT" | grep -qw "$SSID"; then
  ok "scan_results contains $SSID"
else
  fail "scan_results missing $SSID"
fi

log "Phase 6: M2-D — STA association to brcmsim AP"
# Add brcmsim network and trigger reconnect
NETID=$(wpa_cli -i wlan0 add_network 2>/dev/null | tail -1)
wpa_cli -i wlan0 set_network "$NETID" ssid "\"$SSID\"" >/dev/null
wpa_cli -i wlan0 set_network "$NETID" key_mgmt NONE  >/dev/null
wpa_cli -i wlan0 set_network "$NETID" scan_ssid 1    >/dev/null
wpa_cli -i wlan0 enable_network "$NETID" >/dev/null
wpa_cli -i wlan0 reconnect >/dev/null
ASSOC_OK=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  ST=$(wpa_cli -i wlan0 status 2>/dev/null | awk -F= '/^wpa_state/{print $2}')
  if [ "$ST" = "COMPLETED" ]; then ASSOC_OK=1; break; fi
  sleep 1
done
if [ "$ASSOC_OK" = "1" ]; then
  ok "wpa_state=COMPLETED at t=${i}s"
else
  fail "wpa_state never reached COMPLETED (last=$ST)"
fi
STA_LIST=$(hostapd_cli -i wlan1 all_sta 2>/dev/null)
if echo "$STA_LIST" | grep -qE '^[0-9a-f:]{17}$'; then
  ok "AP sees STA: $(echo "$STA_LIST" | head -1)"
else
  fail "AP all_sta empty"
fi

# M2-D disconnect path: wpa_cli disconnect → BRCMF_C_DISASSOC →
# expect STA wpa_state to leave COMPLETED and AP all_sta to empty.
wpa_cli -i wlan0 disconnect >/dev/null
DISC_OK=0
for i in 1 2 3 4 5 6 7 8; do
  ST=$(wpa_cli -i wlan0 status 2>/dev/null | awk -F= '/^wpa_state/{print $2}')
  if [ "$ST" != "COMPLETED" ] && [ -n "$ST" ]; then DISC_OK=1; break; fi
  sleep 1
done
if [ "$DISC_OK" = "1" ]; then
  ok "STA disconnect → wpa_state=$ST at t=${i}s"
else
  fail "STA still COMPLETED after disconnect"
fi
STA_LIST=""
for i in 1 2 3 4 5; do
  STA_LIST=$(hostapd_cli -i wlan1 all_sta 2>/dev/null)
  [ -z "$(echo "$STA_LIST" | grep -E '^[0-9a-f:]{17}$')" ] && break
  sleep 1
done
if [ -z "$(echo "$STA_LIST" | grep -E '^[0-9a-f:]{17}$')" ]; then
  ok "AP all_sta empty after disconnect"
else
  fail "AP still has STA after disconnect: $STA_LIST"
fi

log "Phase 7: dmesg cleanliness"
BAD=$(dmesg | grep -iE 'WARN|BUG:|oops|null deref|unable to handle|panic|RIP:|Call Trace' || true)
if [ -z "$BAD" ]; then
  ok "dmesg clean"
else
  fail "dmesg contains warnings/oops:"
  echo "$BAD"
fi

echo
log "Summary: PASS=$PASS  FAIL=$FAIL"
exit "$FAIL"

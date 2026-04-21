// SPDX-License-Identifier: ISC
/*
 * brcmfmac_hwsim - Simulated bus module for brcmfmac testing.
 *
 * This module acts as virtual firmware + virtual hardware bus for the
 * brcmfmac driver. It handles BCDC ioctls/iovars, generates firmware
 * events, converts TX data to RX loopback, and simulates firmware
 * download — all without real hardware.
 *
 * Copyright (c) 2024 brcmfmac hwsim contributors
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include <brcmu_wifi.h>
#include "core.h"
#include "fwil.h"
#include "fwil_types.h"
#include "fweh.h"
#include "sim_bus_if.h"

/* BCDC protocol definitions (mirrored from bcdc.c) */
struct hwsim_bcdc_dcmd {
	__le32 cmd;
	__le32 len;
	__le32 flags;
	__le32 status;
};

#define HWSIM_BCDC_DCMD_ERROR		0x01
#define HWSIM_BCDC_DCMD_SET		0x02
#define HWSIM_BCDC_DCMD_IF_MASK	0xF000
#define HWSIM_BCDC_DCMD_IF_SHIFT	12
#define HWSIM_BCDC_DCMD_ID_MASK	0xFFFF0000
#define HWSIM_BCDC_DCMD_ID_SHIFT	16

/* BCDC data header */
#define HWSIM_BCDC_HEADER_LEN		4
#define HWSIM_BCDC_PROTO_VER		2
#define HWSIM_BCDC_FLAG_VER_SHIFT	4
#define HWSIM_BCDC_FLAG2_IF_MASK	0x0f

/* BCME firmware error codes */
#define BCME_OK			0
#define BCME_UNSUPPORTED	(-23)

/* Event message version */
#define BRCMF_EVENT_MSG_VERSION		2

/* Simulated device constants */
#define HWSIM_MAC_ADDR		"\x02\x00\x00\x48\x53\x00"
#define HWSIM_AP_MAC		"\x02\x00\x00\x48\x53\x10"
#define HWSIM_AP_SSID		"HWSIM-AP"
#define HWSIM_AP_CHANNEL	1
#define HWSIM_AP_SIGNAL		(-50)
#define HWSIM_VERSION_STR	"wl0: Oct 01 2024 brcmfmac-hwsim-1.0"
#define HWSIM_CLMVER_STR	"API: 0.0 Data: hwsim.0"
#define HWSIM_CAP_STR		"sta wl 11n 11ac 11d 11h mfp"
#define HWSIM_EVENT_MSGS_LEN	27
#define HWSIM_MAXCTL		8192

/* BRCM event ethernet type and OUI */
#define ETH_P_BRCM		0x886c

enum hwsim_fw_state {
	HWSIM_FW_OFF = 0,
	HWSIM_FW_BOOTED,
};

enum hwsim_mode {
	HWSIM_MODE_STA = 0,
	HWSIM_MODE_AP,
};

/**
 * struct hwsim_dev - per-device state for the virtual firmware
 * @fw_state: firmware boot state
 * @mode: current operating mode (STA/AP)
 * @mac_addr: simulated MAC address
 * @event_mask: stored event_msgs mask from driver
 * @country: stored country code
 * @associated: whether STA is associated
 * @ap_started: whether AP mode is started
 * @ap_ssid: AP mode SSID
 * @ap_ssid_len: AP mode SSID length
 * @cb: callbacks to bus shim (rx_ctl_ready, rx_data)
 * @cb_ctx: opaque context for callbacks
 * @cb_registered: whether callbacks have been registered
 * @resp_buf: buffer for BCDC control responses
 * @resp_len: length of current response in resp_buf
 * @resp_ready: response is ready for retrieval
 * @lock: protects state modifications
 * @scan_work: delayed work for scan event generation
 * @connect_work: delayed work for connect event generation
 * @wq: private workqueue for async work
 */
struct hwsim_dev {
	enum hwsim_fw_state fw_state;
	enum hwsim_mode mode;
	u8 mac_addr[ETH_ALEN];
	u8 event_mask[HWSIM_EVENT_MSGS_LEN];
	u8 country[4]; /* "XX\0\0" */
	bool associated;
	bool ap_started;
	u8 ap_ssid[IEEE80211_MAX_SSID_LEN];
	u8 ap_ssid_len;
	u16 scan_sync_id;

	const struct brcmf_hwsim_cb *cb;
	void *cb_ctx;
	bool cb_registered;
	bool detached;

	u8 resp_buf[HWSIM_MAXCTL];
	int resp_len;
	bool resp_ready;

	struct mutex lock;
	struct delayed_work scan_work;
	struct delayed_work connect_work;
	struct workqueue_struct *wq;

	/* Fault injection state (Phase 17) */
	struct {
		u32 force_ioctl_error;   /* non-zero → all ioctls return this BCME code */
		u32 force_txdata_error;  /* non-zero → tx_data returns this Linux errno (positive) */
		u32 drop_events;         /* bitmask: 1=drop scan, 2=drop connect, 4=drop AP */
		u32 ctl_delay_ms;        /* extra delay before ctrl response (0=none) */
		u32 txdata_drop_pct;     /* percentage of TX data to drop (0-100) */
		u32 tx_count;            /* cumulative TX data count */
		u32 rx_count;            /* cumulative RX data count */
		u32 ctl_count;           /* cumulative control transactions */
		u32 event_count;         /* cumulative events generated */
	} fi;

	struct dentry *debugfs_dir;
};

/* Global device instance (Milestone 1: single device) */
static struct hwsim_dev *hwsim_device;
static DEFINE_MUTEX(hwsim_global_lock);

/* ======================================================================
 * BCDC IOVAR name extraction
 *
 * For GET_VAR/SET_VAR commands, the iovar name is the first
 * NUL-terminated string in the payload buffer.
 * ====================================================================== */

static const char *hwsim_extract_iovar(const u8 *payload, uint len)
{
	uint i;

	if (!payload || len == 0)
		return NULL;

	for (i = 0; i < len && i < 64; i++) {
		if (payload[i] == '\0')
			return (const char *)payload;
	}
	return NULL;
}

/* ======================================================================
 * BCDC response builder
 * ====================================================================== */

static void hwsim_build_response(struct hwsim_dev *dev,
				 const struct hwsim_bcdc_dcmd *req,
				 const void *data, uint data_len,
				 s32 status, bool error)
{
	struct hwsim_bcdc_dcmd *resp;
	u32 flags;

	resp = (struct hwsim_bcdc_dcmd *)dev->resp_buf;
	resp->cmd = req->cmd;
	resp->len = cpu_to_le32(data_len);

	flags = le32_to_cpu(req->flags);
	/* Keep the request ID and interface index, clear set/error bits */
	flags &= (HWSIM_BCDC_DCMD_ID_MASK | HWSIM_BCDC_DCMD_IF_MASK);
	if (error)
		flags |= HWSIM_BCDC_DCMD_ERROR;
	resp->flags = cpu_to_le32(flags);
	resp->status = cpu_to_le32(status);

	if (data && data_len > 0) {
		memcpy(dev->resp_buf + sizeof(*resp), data, data_len);
	}

	dev->resp_len = sizeof(*resp) + data_len;
	dev->resp_ready = true;
}

static void hwsim_build_ok(struct hwsim_dev *dev,
			   const struct hwsim_bcdc_dcmd *req,
			   const void *data, uint data_len)
{
	hwsim_build_response(dev, req, data, data_len, BCME_OK, false);
}

static void hwsim_build_error(struct hwsim_dev *dev,
			      const struct hwsim_bcdc_dcmd *req,
			      s32 bcme_status)
{
	hwsim_build_response(dev, req, NULL, 0, bcme_status, true);
}

/* ======================================================================
 * Firmware event generation helpers
 * ====================================================================== */

static struct sk_buff *hwsim_alloc_event_skb(struct hwsim_dev *dev,
					     u32 event_type, u32 status,
					     u32 reason, u16 flags,
					     const u8 *addr, u8 ifidx,
					     const void *data, u32 datalen)
{
	struct sk_buff *skb;
	struct brcmf_event *event;
	u8 *bcdc_hdr;
	int total_len;

	/* BCDC data header + event struct + extra data */
	total_len = HWSIM_BCDC_HEADER_LEN + sizeof(*event) + datalen;
	skb = alloc_skb(total_len, GFP_KERNEL);
	if (!skb)
		return NULL;

	/* BCDC data header */
	bcdc_hdr = skb_put(skb, HWSIM_BCDC_HEADER_LEN);
	bcdc_hdr[0] = (HWSIM_BCDC_PROTO_VER << HWSIM_BCDC_FLAG_VER_SHIFT);
	bcdc_hdr[1] = 0; /* priority */
	bcdc_hdr[2] = ifidx & HWSIM_BCDC_FLAG2_IF_MASK;
	bcdc_hdr[3] = 0; /* data_offset: 0 extra words */

	/* Event structure — set skb_mac_header so fweh.c can find it */
	skb_set_mac_header(skb, skb->len);
	event = (struct brcmf_event *)skb_put(skb, sizeof(*event));

	/* Ethernet header */
	eth_broadcast_addr(event->eth.h_dest);
	memcpy(event->eth.h_source, dev->mac_addr, ETH_ALEN);
	event->eth.h_proto = htons(ETH_P_BRCM);

	/* Broadcom event header */
	event->hdr.subtype = cpu_to_be16(BCMILCP_SUBTYPE_VENDOR_LONG);
	event->hdr.length = cpu_to_be16(sizeof(event->msg) + datalen);
	event->hdr.version = 2;
	memcpy(event->hdr.oui, BRCM_OUI, 3);
	event->hdr.usr_subtype = cpu_to_be16(BCMILCP_BCM_SUBTYPE_EVENT);

	/* Event message */
	event->msg.version = cpu_to_be16(BRCMF_EVENT_MSG_VERSION);
	event->msg.flags = cpu_to_be16(flags);
	event->msg.event_type = cpu_to_be32(event_type);
	event->msg.status = cpu_to_be32(status);
	event->msg.reason = cpu_to_be32(reason);
	event->msg.auth_type = 0;
	event->msg.datalen = cpu_to_be32(datalen);
	if (addr)
		memcpy(event->msg.addr, addr, ETH_ALEN);
	else
		eth_zero_addr(event->msg.addr);
	memset(event->msg.ifname, 0, IFNAMSIZ);
	snprintf(event->msg.ifname, IFNAMSIZ, "wlan%d", ifidx);
	event->msg.ifidx = ifidx;
	event->msg.bsscfgidx = ifidx;

	/* Extra event data */
	if (data && datalen > 0)
		skb_put_data(skb, data, datalen);

	return skb;
}

static void hwsim_send_event(struct hwsim_dev *dev, struct sk_buff *skb)
{
	if (!dev->cb_registered || dev->detached || !dev->cb || !dev->cb->rx_data)
		goto free;

	dev->cb->rx_data(dev->cb_ctx, skb);
	return;
free:
	kfree_skb(skb);
}

/* ======================================================================
 * Scan event generator (workqueue)
 * ====================================================================== */

static void hwsim_scan_work_fn(struct work_struct *work)
{
	struct hwsim_dev *dev = container_of(work, struct hwsim_dev,
					     scan_work.work);
	struct sk_buff *skb;
	struct brcmf_escan_result_le *escan;
	struct brcmf_bss_info_le *bi;
	u8 ie_data[] = { /* SSID IE */
		0x00, 0x08,
		'H', 'W', 'S', 'I', 'M', '-', 'A', 'P',
		/* Supported rates IE */
		0x01, 0x04,
		0x82, 0x84, 0x8b, 0x96,
	};
	int bss_len, escan_len;

	mutex_lock(&dev->lock);
	if (dev->detached) {
		mutex_unlock(&dev->lock);
		return;
	}

	/* Fault injection: drop scan events (bit 0) */
	if (dev->fi.drop_events & 0x01) {
		mutex_unlock(&dev->lock);
		return;
	}

	dev->fi.event_count++;

	/* Partial result with 1 BSS */
	bss_len = offsetof(struct brcmf_bss_info_le, basic_mcs) +
		  BRCMF_MCSSET_LEN + sizeof(ie_data);
	escan_len = sizeof(*escan) - sizeof(struct brcmf_bss_info_le) + bss_len;

	skb = hwsim_alloc_event_skb(dev, BRCMF_E_ESCAN_RESULT,
				    BRCMF_E_STATUS_PARTIAL, 0, 0,
				    NULL, 0, NULL, escan_len);
	if (!skb)
		goto done;

	/* Fill escan result in the event data portion */
	escan = (struct brcmf_escan_result_le *)
		(skb->data + skb->len - escan_len);
	escan->buflen = cpu_to_le32(escan_len);
	escan->version = cpu_to_le32(109);
	escan->sync_id = cpu_to_le16(dev->scan_sync_id);
	escan->bss_count = cpu_to_le16(1);

	bi = &escan->bss_info_le;
	memset(bi, 0, bss_len);
	bi->version = cpu_to_le32(109);
	bi->length = cpu_to_le32(bss_len);
	memcpy(bi->BSSID, HWSIM_AP_MAC, ETH_ALEN);
	bi->beacon_period = cpu_to_le16(100);
	bi->capability = cpu_to_le16(0x0431); /* ESS | Privacy | Short Preamble | Short Slot */
	bi->SSID_len = strlen(HWSIM_AP_SSID);
	memcpy(bi->SSID, HWSIM_AP_SSID, bi->SSID_len);
	bi->chanspec = cpu_to_le16(HWSIM_AP_CHANNEL);
	bi->RSSI = cpu_to_le16(HWSIM_AP_SIGNAL);
	bi->phy_noise = -95;
	bi->n_cap = 1;
	bi->ctl_ch = HWSIM_AP_CHANNEL;
	bi->ie_offset = cpu_to_le16(offsetof(struct brcmf_bss_info_le,
					     basic_mcs) + BRCMF_MCSSET_LEN);
	bi->ie_length = cpu_to_le32(sizeof(ie_data));

	/* Copy IEs after the fixed BSS fields */
	memcpy((u8 *)bi + le16_to_cpu(bi->ie_offset), ie_data,
	       sizeof(ie_data));

	mutex_unlock(&dev->lock);
	hwsim_send_event(dev, skb);

	/* Schedule scan complete after a short delay */
	msleep(50);

	mutex_lock(&dev->lock);
	if (dev->detached) {
		mutex_unlock(&dev->lock);
		return;
	}

	/* Send scan complete (SUCCESS status, no extra data) */
	skb = hwsim_alloc_event_skb(dev, BRCMF_E_ESCAN_RESULT,
				    BRCMF_E_STATUS_SUCCESS, 0, 0,
				    NULL, 0, NULL, 0);
	mutex_unlock(&dev->lock);

	if (skb)
		hwsim_send_event(dev, skb);
	return;

done:
	mutex_unlock(&dev->lock);
}

/* ======================================================================
 * Connect event generator (workqueue)
 * ====================================================================== */

static void hwsim_connect_work_fn(struct work_struct *work)
{
	struct hwsim_dev *dev = container_of(work, struct hwsim_dev,
					     connect_work.work);
	struct sk_buff *skb;

	mutex_lock(&dev->lock);
	if (dev->detached) {
		mutex_unlock(&dev->lock);
		return;
	}

	/* Fault injection: drop connect events (bit 1) */
	if (dev->fi.drop_events & 0x02) {
		mutex_unlock(&dev->lock);
		return;
	}

	dev->fi.event_count++;
	dev->associated = true;

	/* Send BRCMF_E_LINK with LINK flag set */
	skb = hwsim_alloc_event_skb(dev, BRCMF_E_LINK,
				    BRCMF_E_STATUS_SUCCESS, 0,
				    BRCMF_EVENT_MSG_LINK,
				    (const u8 *)HWSIM_AP_MAC, 0,
				    NULL, 0);
	mutex_unlock(&dev->lock);

	if (skb)
		hwsim_send_event(dev, skb);
}

/* ======================================================================
 * IOCTL/IOVAR command handlers
 * ====================================================================== */

static int hwsim_handle_get_var(struct hwsim_dev *dev,
				const struct hwsim_bcdc_dcmd *req,
				const u8 *payload, uint payload_len)
{
	const char *iovar = hwsim_extract_iovar(payload, payload_len);
	size_t name_len;

	if (!iovar)
		goto unsupported;

	name_len = strlen(iovar) + 1;

	/* --- Probe-FATAL iovars --- */

	if (strcmp(iovar, "cur_etheraddr") == 0) {
		hwsim_build_ok(dev, req, dev->mac_addr, ETH_ALEN);
		return 0;
	}

	if (strcmp(iovar, "ver") == 0) {
		hwsim_build_ok(dev, req, HWSIM_VERSION_STR,
			       strlen(HWSIM_VERSION_STR) + 1);
		return 0;
	}

	if (strcmp(iovar, "event_msgs") == 0) {
		hwsim_build_ok(dev, req, dev->event_mask,
			       HWSIM_EVENT_MSGS_LEN);
		return 0;
	}

	/* --- Important but non-fatal iovars --- */

	if (strcmp(iovar, "cap") == 0) {
		hwsim_build_ok(dev, req, HWSIM_CAP_STR,
			       strlen(HWSIM_CAP_STR) + 1);
		return 0;
	}

	if (strcmp(iovar, "clmver") == 0) {
		hwsim_build_ok(dev, req, HWSIM_CLMVER_STR,
			       strlen(HWSIM_CLMVER_STR) + 1);
		return 0;
	}

	/* --- Feature detection iovars (BCME_UNSUPPORTED) --- */

	if (strcmp(iovar, "mfp") == 0) {
		__le32 val = cpu_to_le32(1);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	if (strcmp(iovar, "pfn") == 0 ||
	    strcmp(iovar, "wowl") == 0 ||
	    strcmp(iovar, "wowl_cap") == 0 ||
	    strcmp(iovar, "rsdb_mode") == 0 ||
	    strcmp(iovar, "tdls_enable") == 0 ||
	    strcmp(iovar, "sup_wpa") == 0) {
		hwsim_build_error(dev, req, BCME_UNSUPPORTED);
		return 0;
	}

	if (strcmp(iovar, "wlc_ver") == 0) {
		struct brcmf_wlc_version_le ver = {};

		ver.version = cpu_to_le16(1);
		ver.length = cpu_to_le16(sizeof(ver));
		ver.epi_ver_major = cpu_to_le16(1);
		ver.wlc_ver_major = cpu_to_le16(1);
		hwsim_build_ok(dev, req, &ver, sizeof(ver));
		return 0;
	}

	if (strcmp(iovar, "scan_ver") == 0) {
		/* Return a minimal version struct */
		__le32 val = cpu_to_le32(1);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	/* --- Scan related --- */

	if (strcmp(iovar, "chanspec") == 0) {
		__le32 val = cpu_to_le32(HWSIM_AP_CHANNEL);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	if (strcmp(iovar, "per_chan_info") == 0) {
		/* No radar, not passive-only: return 0 flags */
		__le32 val = cpu_to_le32(0);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	if (strcmp(iovar, "chanspecs") == 0) {
		/* D11AC encoding: ch_num | BW_20 (0x1000) | BND_2G (0x0000) */
		struct {
			__le32 count;
			__le32 element[3];
		} list = {
			.count = cpu_to_le32(3),
			.element = {
				cpu_to_le32(0x1001), /* ch 1,  20 MHz, 2.4G */
				cpu_to_le32(0x1006), /* ch 6,  20 MHz, 2.4G */
				cpu_to_le32(0x100B), /* ch 11, 20 MHz, 2.4G */
			},
		};
		hwsim_build_ok(dev, req, &list, sizeof(list));
		return 0;
	}

	if (strcmp(iovar, "bw_cap") == 0) {
		/* Report bw_cap query as unsupported so cfg80211 disables
		 * 40MHz HT cap and skips the bw40 chanspecs path, avoiding
		 * WARN_ON in brcmf_enable_bw40_2g().
		 */
		hwsim_build_error(dev, req, BCME_UNSUPPORTED);
		return 0;
	}

	if (strcmp(iovar, "rxchain") == 0) {
		__le32 val = cpu_to_le32(1);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	if (strcmp(iovar, "qtxpower") == 0) {
		/* Report 20 dBm (qdbm = dBm * 4) */
		__le32 val = cpu_to_le32(80);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	if (strcmp(iovar, "nmode") == 0 || strcmp(iovar, "vhtmode") == 0) {
		__le32 val = cpu_to_le32(1);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	/* --- Runtime --- */

	if (strcmp(iovar, "country") == 0) {
		hwsim_build_ok(dev, req, dev->country, sizeof(dev->country));
		return 0;
	}

	/* --- Association state --- */

	if (strcmp(iovar, "assoc") == 0) {
		__le32 val = cpu_to_le32(dev->associated ? 1 : 0);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

unsupported:
	/* Default: unknown GET → BCME_UNSUPPORTED */
	hwsim_build_error(dev, req, BCME_UNSUPPORTED);
	return 0;
}

static int hwsim_handle_set_var(struct hwsim_dev *dev,
				const struct hwsim_bcdc_dcmd *req,
				const u8 *payload, uint payload_len)
{
	const char *iovar = hwsim_extract_iovar(payload, payload_len);
	size_t name_len;
	const u8 *val_ptr;
	uint val_len;

	if (!iovar)
		goto accept;

	name_len = strlen(iovar) + 1;
	val_ptr = payload + name_len;
	val_len = (payload_len > name_len) ? payload_len - name_len : 0;

	/* --- Probe-FATAL SET iovars --- */

	if (strcmp(iovar, "mpc") == 0 ||
	    strcmp(iovar, "SCAN_CHANNEL_TIME") == 0 ||
	    strcmp(iovar, "SCAN_UNASSOC_TIME") == 0) {
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;
	}

	if (strcmp(iovar, "event_msgs") == 0) {
		if (val_len >= HWSIM_EVENT_MSGS_LEN)
			memcpy(dev->event_mask, val_ptr,
			       HWSIM_EVENT_MSGS_LEN);
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;
	}

	/* --- Important but non-fatal --- */

	if (strcmp(iovar, "join_pref") == 0 ||
	    strcmp(iovar, "txbf") == 0) {
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;
	}

	/* Reject these so brcmf_enable_bw40_2g() bails early and
	 * cfg80211 masks off HT 40MHz, avoiding a harmless WARN.
	 */
	if (strcmp(iovar, "mimo_bw_cap") == 0 ||
	    strcmp(iovar, "bw_cap") == 0) {
		hwsim_build_error(dev, req, BCME_UNSUPPORTED);
		return 0;
	}

	/* --- Runtime --- */

	if (strcmp(iovar, "country") == 0) {
		if (val_len >= 2) {
			memcpy(dev->country, val_ptr,
			       min_t(uint, val_len, sizeof(dev->country)));
		}
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;
	}

	/* --- Scan --- */

	if (strcmp(iovar, "escan") == 0) {
		if (val_len >= sizeof(struct brcmf_escan_params_le)) {
			const struct brcmf_escan_params_le *ep;
			ep = (const struct brcmf_escan_params_le *)val_ptr;
			dev->scan_sync_id = le16_to_cpu(ep->sync_id);
		}
		hwsim_build_ok(dev, req, NULL, 0);
		/* Schedule scan results */
		queue_delayed_work(dev->wq, &dev->scan_work,
				   msecs_to_jiffies(100));
		return 0;
	}

	/* --- Connect/Association --- */

	if (strcmp(iovar, "join") == 0) {
		hwsim_build_ok(dev, req, NULL, 0);
		queue_delayed_work(dev->wq, &dev->connect_work,
				   msecs_to_jiffies(200));
		return 0;
	}

	if (strcmp(iovar, "wpa_auth") == 0 ||
	    strcmp(iovar, "auth") == 0 ||
	    strcmp(iovar, "wsec") == 0 ||
	    strcmp(iovar, "wpaie") == 0) {
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;
	}

	/* --- AP mode --- */

	if (strcmp(iovar, "chanspec") == 0 ||
	    strcmp(iovar, "closednet") == 0) {
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;
	}

accept:
	/* Default: unknown SET → success */
	hwsim_build_ok(dev, req, NULL, 0);
	return 0;
}

static int hwsim_handle_cmd(struct hwsim_dev *dev,
			    const struct hwsim_bcdc_dcmd *req,
			    const u8 *payload, uint payload_len,
			    bool is_set)
{
	u32 cmd = le32_to_cpu(req->cmd);

	switch (cmd) {
	case BRCMF_C_GET_VAR:
		return hwsim_handle_get_var(dev, req, payload, payload_len);

	case BRCMF_C_SET_VAR:
		return hwsim_handle_set_var(dev, req, payload, payload_len);

	case BRCMF_C_GET_VERSION: {
		/* Returns the D11 IO type; cfg80211_attach() requires it. */
		__le32 val = cpu_to_le32(2); /* BRCMU_D11AC_IOTYPE */
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_GET_REVINFO: {
		struct brcmf_rev_info_le ri = {};

		ri.vendorid = cpu_to_le32(0x14e4); /* Broadcom */
		ri.deviceid = cpu_to_le32(0xFFFF);
		ri.chipnum = cpu_to_le32(0xFFFF);
		ri.chiprev = cpu_to_le32(0);
		ri.corerev = cpu_to_le32(1);
		ri.bus = cpu_to_le32(1); /* SDIO */
		hwsim_build_ok(dev, req, &ri, sizeof(ri));
		return 0;
	}

	case BRCMF_C_UP:
	case BRCMF_C_DOWN:
	case BRCMF_C_SET_PROMISC:
	case BRCMF_C_SET_INFRA:
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;

	case BRCMF_C_GET_INFRA: {
		__le32 val = cpu_to_le32(1);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_SET_AP:
		dev->mode = HWSIM_MODE_AP;
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;

	case BRCMF_C_GET_AP: {
		__le32 val = cpu_to_le32(dev->mode == HWSIM_MODE_AP ? 1 : 0);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_SET_SSID:
		if (dev->mode == HWSIM_MODE_AP && payload_len >= 4) {
			/* Store AP SSID and signal AP started */
			u32 ssid_len = le32_to_cpu(*(__le32 *)payload);

			if (ssid_len <= IEEE80211_MAX_SSID_LEN &&
			    payload_len >= 4 + ssid_len) {
				dev->ap_ssid_len = ssid_len;
				memcpy(dev->ap_ssid, payload + 4, ssid_len);
				dev->ap_started = true;
			}
			hwsim_build_ok(dev, req, NULL, 0);
			/* Generate SET_SSID event for AP */
			{
				struct sk_buff *skb;

				skb = hwsim_alloc_event_skb(dev,
					BRCMF_E_SET_SSID,
					BRCMF_E_STATUS_SUCCESS, 0, 0,
					dev->mac_addr, 0, NULL, 0);
				if (skb)
					hwsim_send_event(dev, skb);
			}
		} else {
			hwsim_build_ok(dev, req, NULL, 0);
			/* STA connect via SET_SSID fallback */
			queue_delayed_work(dev->wq, &dev->connect_work,
					   msecs_to_jiffies(200));
		}
		return 0;

	case BRCMF_C_GET_SSID: {
		struct brcmf_ssid_le ssid = {};

		ssid.SSID_len = cpu_to_le32(dev->ap_ssid_len);
		memcpy(ssid.SSID, dev->ap_ssid, dev->ap_ssid_len);
		hwsim_build_ok(dev, req, &ssid, sizeof(ssid));
		return 0;
	}

	case BRCMF_C_SET_BCNPRD:
	case BRCMF_C_SET_DTIMPRD:
	case BRCMF_C_SET_CHANNEL:
	case BRCMF_C_SET_KEY:
	case BRCMF_C_SET_PASSIVE_SCAN:
	case BRCMF_C_SET_SCAN_PASSIVE_TIME:
	case BRCMF_C_SET_SCAN_CHANNEL_TIME:
	case BRCMF_C_SET_SCAN_UNASSOC_TIME:
	case BRCMF_C_SET_ROAM_TRIGGER:
	case BRCMF_C_SET_ROAM_DELTA:
	case BRCMF_C_SET_PM:
	case BRCMF_C_SET_COUNTRY:
	case BRCMF_C_SET_REGULATORY:
	case BRCMF_C_SET_SRL:
	case BRCMF_C_SET_LRL:
	case BRCMF_C_SET_FAKEFRAG:
	case BRCMF_C_SET_KEY_PRIMARY:
	case BRCMF_C_SET_SCB_AUTHORIZE:
	case BRCMF_C_SET_SCB_DEAUTHORIZE:
	case BRCMF_C_SET_ASSOC_PREFER:
	case BRCMF_C_SCB_DEAUTHENTICATE_FOR_REASON:
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;

	case BRCMF_C_GET_CHANNEL: {
		__le32 val = cpu_to_le32(HWSIM_AP_CHANNEL);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_GET_PM: {
		__le32 val = cpu_to_le32(0);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_GET_RSSI: {
		__le32 val = cpu_to_le32(HWSIM_AP_SIGNAL);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_GET_RATE: {
		__le32 val = cpu_to_le32(54); /* 54 Mbps */
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_GET_BANDLIST: {
		/* Return 2.4 GHz band only */
		__le32 list[3] = {
			cpu_to_le32(1), /* count */
			cpu_to_le32(2), /* WLC_BAND_2G */
			cpu_to_le32(0),
		};
		hwsim_build_ok(dev, req, list, sizeof(list));
		return 0;
	}

	case BRCMF_C_GET_PHYLIST: {
		char phylist[] = "g\0";
		hwsim_build_ok(dev, req, phylist, sizeof(phylist));
		return 0;
	}

	case BRCMF_C_GET_PHYTYPE: {
		__le32 val = cpu_to_le32(2); /* PHY_TYPE_G */
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	case BRCMF_C_GET_BSSID:
		if (dev->associated)
			hwsim_build_ok(dev, req, HWSIM_AP_MAC, ETH_ALEN);
		else
			hwsim_build_error(dev, req, BCME_UNSUPPORTED);
		return 0;

	case BRCMF_C_GET_VALID_CHANNELS: {
		/* Return channel 1 only */
		__le32 chans[2] = {
			cpu_to_le32(1), /* count */
			cpu_to_le32(1), /* channel 1 */
		};
		hwsim_build_ok(dev, req, chans, sizeof(chans));
		return 0;
	}

	case BRCMF_C_DISASSOC:
		dev->associated = false;
		hwsim_build_ok(dev, req, NULL, 0);
		return 0;

	case BRCMF_C_GET_WSEC:
	case BRCMF_C_GET_AUTH: {
		__le32 val = cpu_to_le32(0);
		hwsim_build_ok(dev, req, &val, sizeof(val));
		return 0;
	}

	default:
		if (is_set)
			hwsim_build_ok(dev, req, NULL, 0);
		else
			hwsim_build_error(dev, req, BCME_UNSUPPORTED);
		return 0;
	}
}

/* ======================================================================
 * brcmf_hwsim_ops callbacks (called by bus shim)
 * ====================================================================== */

static int hwsim_tx_ctl(void *ctx, u8 *msg, uint len)
{
	struct hwsim_dev *dev = ctx;
	struct hwsim_bcdc_dcmd *dcmd;
	u8 *payload;
	uint payload_len;
	u32 flags;
	bool is_set;

	if (!dev || dev->fw_state != HWSIM_FW_BOOTED)
		return -ENODEV;

	if (len < sizeof(*dcmd))
		return -EINVAL;

	/* Fault injection: extra control path delay */
	if (dev->fi.ctl_delay_ms)
		msleep(dev->fi.ctl_delay_ms);

	dev->fi.ctl_count++;

	dcmd = (struct hwsim_bcdc_dcmd *)msg;
	payload = msg + sizeof(*dcmd);
	payload_len = len - sizeof(*dcmd);
	flags = le32_to_cpu(dcmd->flags);
	is_set = !!(flags & HWSIM_BCDC_DCMD_SET);

	mutex_lock(&dev->lock);
	dev->resp_ready = false;

	/* Fault injection: force all ioctls to return an error code */
	if (dev->fi.force_ioctl_error) {
		hwsim_build_response(dev, dcmd, NULL, 0,
				     dev->fi.force_ioctl_error, true);
		mutex_unlock(&dev->lock);
		goto signal;
	}

	hwsim_handle_cmd(dev, dcmd, payload, payload_len, is_set);

	mutex_unlock(&dev->lock);

signal:
	/* Signal response ready to bus shim */
	if (dev->cb_registered && dev->cb && dev->cb->rx_ctl_ready && !dev->detached)
		dev->cb->rx_ctl_ready(dev->cb_ctx);

	return 0;
}

static int hwsim_rx_ctl(void *ctx, u8 *msg, uint len)
{
	struct hwsim_dev *dev = ctx;
	int copy_len;

	if (!dev || dev->fw_state != HWSIM_FW_BOOTED)
		return -ENODEV;

	mutex_lock(&dev->lock);
	if (!dev->resp_ready) {
		mutex_unlock(&dev->lock);
		return -ETIMEDOUT;
	}

	copy_len = min_t(int, dev->resp_len, (int)len);
	memcpy(msg, dev->resp_buf, copy_len);
	dev->resp_ready = false;
	mutex_unlock(&dev->lock);

	/* Must return positive byte count on success (not 0) [D10] */
	return copy_len;
}

static int hwsim_tx_data(void *ctx, struct sk_buff *skb)
{
	struct hwsim_dev *dev = ctx;
	struct sk_buff *rx_skb;
	u8 *bcdc_hdr;

	if (!dev || dev->fw_state != HWSIM_FW_BOOTED)
		return -ENODEV;

	dev->fi.tx_count++;

	/* Fault injection: force TX error */
	if (dev->fi.force_txdata_error)
		return -(int)dev->fi.force_txdata_error;

	/* Fault injection: probabilistic TX drop */
	if (dev->fi.txdata_drop_pct &&
	    (get_random_u32() % 100) < dev->fi.txdata_drop_pct)
		return 0; /* silently drop */

	/*
	 * Loopback: clone the skb, rewrite the BCDC header for RX,
	 * and deliver back via rx_data callback.
	 * The original skb is NOT freed — the bus shim retains ownership
	 * and will pass it to brcmf_proto_bcdc_txcomplete().
	 */
	rx_skb = skb_copy(skb, GFP_KERNEL);
	if (!rx_skb)
		return -ENOMEM;

	/* Rewrite BCDC data header for RX */
	if (rx_skb->len >= HWSIM_BCDC_HEADER_LEN) {
		bcdc_hdr = rx_skb->data;
		/* Set protocol version for RX */
		bcdc_hdr[0] = (HWSIM_BCDC_PROTO_VER <<
			       HWSIM_BCDC_FLAG_VER_SHIFT);
		/* ifidx stays the same for loopback */
	}

	dev->fi.rx_count++;
	hwsim_send_event(dev, rx_skb);
	return 0;
}

static int hwsim_fw_download(void *ctx, const u8 *fw, size_t fw_len,
			     const void *nvram, size_t nvram_len)
{
	struct hwsim_dev *dev = ctx;

	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->lock);
	dev->fw_state = HWSIM_FW_BOOTED;
	mutex_unlock(&dev->lock);

	pr_info("brcmfmac_hwsim: firmware download complete (simulated)\n");
	return 0;
}

static void hwsim_detach(void *ctx)
{
	struct hwsim_dev *dev = ctx;

	if (!dev)
		return;

	mutex_lock(&dev->lock);
	dev->detached = true;
	mutex_unlock(&dev->lock);

	cancel_delayed_work_sync(&dev->scan_work);
	cancel_delayed_work_sync(&dev->connect_work);

	pr_info("brcmfmac_hwsim: detached\n");
}

static const struct brcmf_hwsim_ops hwsim_ops = {
	.tx_ctl = hwsim_tx_ctl,
	.rx_ctl = hwsim_rx_ctl,
	.tx_data = hwsim_tx_data,
	.fw_download = hwsim_fw_download,
	.detach = hwsim_detach,
};

/* ======================================================================
 * Registration function (exported, obtained via symbol_get)
 * ====================================================================== */

int brcmf_hwsim_get_ops(const struct brcmf_hwsim_cb *cb, void *cb_ctx,
			 const struct brcmf_hwsim_ops **ops_out,
			 void **ctx_out)
{
	struct hwsim_dev *dev;

	mutex_lock(&hwsim_global_lock);
	dev = hwsim_device;

	if (!dev) {
		mutex_unlock(&hwsim_global_lock);
		return -ENODEV;
	}

	mutex_lock(&dev->lock);
	if (dev->cb_registered) {
		mutex_unlock(&dev->lock);
		mutex_unlock(&hwsim_global_lock);
		return -EBUSY;
	}

	dev->cb = cb;
	dev->cb_ctx = cb_ctx;
	dev->cb_registered = true;
	dev->detached = false;

	*ops_out = &hwsim_ops;
	*ctx_out = dev;

	mutex_unlock(&dev->lock);
	mutex_unlock(&hwsim_global_lock);

	pr_info("brcmfmac_hwsim: ops registered for bus shim\n");
	return 0;
}
EXPORT_SYMBOL_GPL(brcmf_hwsim_get_ops);

/* ======================================================================
 * Module init/exit
 * ====================================================================== */

/* ======================================================================
 * Debugfs fault injection interface (Phase 17)
 *
 * Directory: /sys/kernel/debug/brcmfmac_hwsim/
 *
 * Writable files (fault injection):
 *   force_ioctl_error  - Set to BCME_* code to make all ioctls fail
 *   force_txdata_error - Set to Linux errno (positive) to make TX fail
 *   drop_events        - Bitmask: 1=scan, 2=connect, 4=AP events
 *   ctl_delay_ms       - Extra delay (ms) before control responses
 *   txdata_drop_pct    - Percentage of TX data to drop (0-100)
 *
 * Read-only files (counters):
 *   tx_count           - Cumulative TX data frames
 *   rx_count           - Cumulative RX data frames
 *   ctl_count          - Cumulative control transactions
 *   event_count        - Cumulative firmware events generated
 *   state              - Virtual firmware state summary
 * ====================================================================== */

static struct dentry *hwsim_debugfs_root;

static int hwsim_state_show(struct seq_file *s, void *unused)
{
	struct hwsim_dev *dev = s->private;

	mutex_lock(&dev->lock);
	seq_printf(s, "fw_state:   %s\n",
		   dev->fw_state == HWSIM_FW_BOOTED ? "BOOTED" : "OFF");
	seq_printf(s, "mode:       %s\n",
		   dev->mode == HWSIM_MODE_AP ? "AP" : "STA");
	seq_printf(s, "mac:        %pM\n", dev->mac_addr);
	seq_printf(s, "country:    %.2s\n", dev->country);
	seq_printf(s, "associated: %s\n", dev->associated ? "yes" : "no");
	seq_printf(s, "ap_started: %s\n", dev->ap_started ? "yes" : "no");
	seq_printf(s, "cb_registered: %s\n", dev->cb_registered ? "yes" : "no");
	seq_printf(s, "detached:   %s\n", dev->detached ? "yes" : "no");
	seq_printf(s, "\n--- fault injection ---\n");
	seq_printf(s, "force_ioctl_error: %u\n", dev->fi.force_ioctl_error);
	seq_printf(s, "force_txdata_error: %u\n", dev->fi.force_txdata_error);
	seq_printf(s, "drop_events: 0x%x\n", dev->fi.drop_events);
	seq_printf(s, "ctl_delay_ms: %u\n", dev->fi.ctl_delay_ms);
	seq_printf(s, "txdata_drop_pct: %u\n", dev->fi.txdata_drop_pct);
	seq_printf(s, "\n--- counters ---\n");
	seq_printf(s, "tx_count: %u\n", dev->fi.tx_count);
	seq_printf(s, "rx_count: %u\n", dev->fi.rx_count);
	seq_printf(s, "ctl_count: %u\n", dev->fi.ctl_count);
	seq_printf(s, "event_count: %u\n", dev->fi.event_count);
	mutex_unlock(&dev->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hwsim_state);

static void hwsim_debugfs_create(struct hwsim_dev *dev)
{
	struct dentry *dir;

	hwsim_debugfs_root = debugfs_create_dir("brcmfmac_hwsim", NULL);
	if (IS_ERR_OR_NULL(hwsim_debugfs_root)) {
		hwsim_debugfs_root = NULL;
		return;
	}
	dir = hwsim_debugfs_root;
	dev->debugfs_dir = dir;

	/* Fault injection knobs */
	debugfs_create_u32("force_ioctl_error", 0644, dir,
			   &dev->fi.force_ioctl_error);
	debugfs_create_u32("force_txdata_error", 0644, dir,
			   &dev->fi.force_txdata_error);
	debugfs_create_u32("drop_events", 0644, dir, &dev->fi.drop_events);
	debugfs_create_u32("ctl_delay_ms", 0644, dir, &dev->fi.ctl_delay_ms);
	debugfs_create_u32("txdata_drop_pct", 0644, dir,
			   &dev->fi.txdata_drop_pct);

	/* Read-only counters */
	debugfs_create_u32("tx_count", 0444, dir, &dev->fi.tx_count);
	debugfs_create_u32("rx_count", 0444, dir, &dev->fi.rx_count);
	debugfs_create_u32("ctl_count", 0444, dir, &dev->fi.ctl_count);
	debugfs_create_u32("event_count", 0444, dir, &dev->fi.event_count);

	/* Composite state file */
	debugfs_create_file("state", 0444, dir, dev, &hwsim_state_fops);
}

static void hwsim_debugfs_remove(void)
{
	debugfs_remove_recursive(hwsim_debugfs_root);
	hwsim_debugfs_root = NULL;
}

/* ======================================================================
 * Device alloc / free
 * ====================================================================== */

static struct hwsim_dev *hwsim_dev_alloc(void)
{
	struct hwsim_dev *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	mutex_init(&dev->lock);
	memcpy(dev->mac_addr, HWSIM_MAC_ADDR, ETH_ALEN);
	memcpy(dev->country, "US", 2);
	dev->fw_state = HWSIM_FW_OFF;
	dev->mode = HWSIM_MODE_STA;

	INIT_DELAYED_WORK(&dev->scan_work, hwsim_scan_work_fn);
	INIT_DELAYED_WORK(&dev->connect_work, hwsim_connect_work_fn);

	dev->wq = alloc_ordered_workqueue("brcmfmac_hwsim", 0);
	if (!dev->wq) {
		kfree(dev);
		return NULL;
	}

	return dev;
}

static void hwsim_dev_free(struct hwsim_dev *dev)
{
	if (!dev)
		return;

	cancel_delayed_work_sync(&dev->scan_work);
	cancel_delayed_work_sync(&dev->connect_work);
	if (dev->wq)
		destroy_workqueue(dev->wq);
	kfree(dev);
}

static int __init brcmfmac_hwsim_init(void)
{
	struct hwsim_dev *dev;

	dev = hwsim_dev_alloc();
	if (!dev)
		return -ENOMEM;

	mutex_lock(&hwsim_global_lock);
	hwsim_device = dev;
	mutex_unlock(&hwsim_global_lock);

	hwsim_debugfs_create(dev);

	pr_info("brcmfmac_hwsim: module loaded (virtual firmware ready)\n");
	return 0;
}

static void __exit brcmfmac_hwsim_exit(void)
{
	struct hwsim_dev *dev;

	hwsim_debugfs_remove();

	mutex_lock(&hwsim_global_lock);
	dev = hwsim_device;
	hwsim_device = NULL;
	mutex_unlock(&hwsim_global_lock);

	hwsim_dev_free(dev);
	pr_info("brcmfmac_hwsim: module unloaded\n");
}

module_init(brcmfmac_hwsim_init);
module_exit(brcmfmac_hwsim_exit);

MODULE_AUTHOR("brcmfmac hwsim contributors");
MODULE_DESCRIPTION("Simulated bus module for brcmfmac testing");
MODULE_LICENSE("Dual BSD/GPL");

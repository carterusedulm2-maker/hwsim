/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2024 brcmfmac hwsim contributors
 *
 * sim_bus_if.h - Interface between the brcmfmac bus shim (sim_sdio.c)
 *                and the mocked bus module (brcmfmac_hwsim.ko).
 *
 * The bus shim lives inside brcmfmac.ko and replaces the real SDIO bus
 * layer when CONFIG_BRCMFMAC_HWSIM_SDIO is enabled. It connects to the
 * separate brcmfmac_hwsim.ko module via symbol_get()/symbol_put().
 *
 * Locking model:
 *   - Control path (txctl/rxctl): synchronized via struct completion in
 *     the bus shim. Only one outstanding control transaction at a time
 *     (guaranteed by brcmf_proto layer).
 *   - Data path (txdata/rx_data): skb ownership is transferred on each
 *     call. No additional locks needed.
 *   - Module lifetime: symbol_get() holds a reference to brcmfmac_hwsim;
 *     symbol_put() releases it. No RCU needed.
 *   - Teardown: set shutting_down flag, call detach(), flush workqueues,
 *     then symbol_put().
 */

#ifndef BRCMFMAC_SIM_BUS_IF_H
#define BRCMFMAC_SIM_BUS_IF_H

#include <linux/types.h>
#include <linux/skbuff.h>

/**
 * struct brcmf_hwsim_ops - operations provided by brcmfmac_hwsim module
 *
 * These callbacks are obtained by the bus shim via symbol_get() on
 * brcmf_hwsim_get_ops(). The @ctx parameter is an opaque context
 * returned by brcmf_hwsim_get_ops() alongside this ops struct.
 *
 * @tx_ctl: Forward a BCDC control message (dcmd) to the virtual firmware.
 *          The message includes the full BCDC dcmd header + payload.
 *          Returns 0 on success, negative errno on transport failure.
 *          On success, the response is made available via the rx_ctl_ready
 *          callback, and the shim retrieves it with rx_ctl().
 *
 * @rx_ctl: Retrieve the BCDC control response from the virtual firmware.
 *          @msg: buffer to copy the response into (allocated by caller).
 *          @len: maximum buffer size.
 *          Returns the number of bytes copied (positive) on success,
 *          or negative errno on failure (e.g., -ETIMEDOUT).
 *          The return value is used directly as the byte count by the
 *          BCDC protocol layer (bcdc.c), so it MUST be a positive byte
 *          count on success, NOT zero.
 *
 * @tx_data: Forward a data frame (sk_buff with BCDC data header) to the
 *           virtual firmware for processing (loopback, routing, etc.).
 *           The callee MUST NOT free @skb — the bus shim retains
 *           ownership.  If the hwsim module needs the data after this
 *           call returns (e.g., for loopback), it must clone the skb.
 *           The bus shim calls brcmf_proto_bcdc_txcomplete() after
 *           this returns, regardless of success or failure.
 *           Returns 0 on success, negative errno on failure.
 *
 * @fw_download: Simulate firmware download to the virtual device.
 *               In Milestone 1, @fw and @nvram are always NULL with
 *               zero lengths (shim bypasses request_firmware entirely).
 *               The hwsim module sets its internal state to FW_BOOTED.
 *               This is a synchronous call. Idempotent (safe to call
 *               multiple times). Returns 0 on success.
 *               The hwsim module MUST NOT invoke any callbacks before
 *               this function returns.
 *
 * @detach: Notify the hwsim module that the bus shim is shutting down.
 *          After this call, the hwsim module MUST NOT invoke any
 *          callbacks (rx_ctl_ready, rx_data). The shim calls this
 *          before flushing workqueues and calling symbol_put().
 */
struct brcmf_hwsim_ops {
	int (*tx_ctl)(void *ctx, u8 *msg, uint len);
	int (*rx_ctl)(void *ctx, u8 *msg, uint len);
	int (*tx_data)(void *ctx, struct sk_buff *skb);
	int (*fw_download)(void *ctx, const u8 *fw, size_t fw_len,
			   const void *nvram, size_t nvram_len);
	void (*detach)(void *ctx);
};

/**
 * struct brcmf_hwsim_cb - callbacks provided by the bus shim to hwsim
 *
 * The bus shim registers these callbacks with the hwsim module during
 * probe, BEFORE calling fw_download(). The hwsim module uses them to
 * deliver asynchronous responses and data to the bus shim.
 *
 * @rx_ctl_ready: Signal that a BCDC control response is ready for
 *                retrieval. The bus shim wakes its completion and
 *                calls hwsim_ops->rx_ctl() to fetch the response.
 *                Called from workqueue context (not interrupt/softirq).
 *
 * @rx_data: Deliver a received data frame or firmware event to the
 *           bus shim. The @skb contains the full frame with BCDC data
 *           header (flags2 encodes the interface index). Ownership of
 *           @skb transfers to the bus shim; it will eventually call
 *           brcmf_rx_frame() to hand it to the brcmfmac core.
 *           Called from workqueue context (not interrupt/softirq).
 */
struct brcmf_hwsim_cb {
	void (*rx_ctl_ready)(void *ctx);
	void (*rx_data)(void *ctx, struct sk_buff *skb);
};

/**
 * brcmf_hwsim_get_ops - obtain hwsim operations and context
 * @cb: callback structure provided by the bus shim
 * @cb_ctx: opaque context passed back in cb invocations
 * @ops_out: [out] pointer to hwsim ops struct (filled on success)
 * @ctx_out: [out] opaque hwsim context for use in ops calls
 *
 * This function is exported by brcmfmac_hwsim.ko and obtained by the
 * bus shim via symbol_get("brcmf_hwsim_get_ops"). The shim must call
 * symbol_put("brcmf_hwsim_get_ops") during teardown to release the
 * module reference.
 *
 * Callback registration ordering: the shim passes @cb and @cb_ctx
 * in this call, so callbacks are registered BEFORE fw_download().
 * The hwsim module MUST NOT invoke callbacks until fw_download()
 * has been called and returned successfully.
 *
 * Return: 0 on success, negative errno on failure.
 */
int brcmf_hwsim_get_ops(const struct brcmf_hwsim_cb *cb, void *cb_ctx,
			 const struct brcmf_hwsim_ops **ops_out,
			 void **ctx_out);

#endif /* BRCMFMAC_SIM_BUS_IF_H */

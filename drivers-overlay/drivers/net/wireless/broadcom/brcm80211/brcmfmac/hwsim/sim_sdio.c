// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2024 brcmfmac hwsim contributors
 *
 * sim_sdio.c - HWSIM bus shim for brcmfmac.
 *
 * When CONFIG_BRCMFMAC_HWSIM_SDIO is enabled this file replaces the
 * real SDIO bus layer.  It creates a platform_device, self-probes,
 * and forwards every bus operation to the separate brcmfmac_hwsim.ko
 * module via the IPC interface defined in sim_bus_if.h.
 *
 * Load order: brcmfmac-wcc.ko → brcmfmac_hwsim.ko → brcmfmac.ko
 *
 * Probe flow (corrected by Design-Review D6/D8/D9):
 *   1. symbol_get("brcmf_hwsim_get_ops") → ops + ctx
 *   2. Register callbacks with hwsim BEFORE fw_download
 *   3. Allocate brcmf_bus, set chip=0xFFFF, proto=BCDC, bus_type=SIM
 *   4. dev_set_drvdata() BEFORE brcmf_alloc (D8)
 *   5. hwsim_ops->fw_download() → virtual FW ready
 *   6. brcmf_get_module_param() (D6)
 *   7. brcmf_alloc() + brcmf_attach() → wlan0 created
 *   8. Completely bypasses brcmf_fw_get_firmwares()
 *
 * Teardown (B5/D14):
 *   1. shutting_down = true
 *   2. hwsim_ops->detach() — hwsim stops callbacks
 *   3. flush workqueues
 *   4. brcmf_detach() + brcmf_free()
 *   5. brcmf_release_module_param()
 *   6. symbol_put()
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/completion.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_data/brcmfmac.h>

#include "bus.h"
#include "core.h"
#include "common.h"
#include "bcdc.h"
#include "debug.h"
#include "sim_bus_if.h"

/* Virtual chip ID — avoids all real chip-specific code paths */
#define SIM_CHIP_ID		0xFFFF
#define SIM_CHIP_REV		0
#define SIM_PLATFORM_NAME	"brcmfmac-sim-sdio"

/**
 * struct brcmf_sim_sdio - private data for the simulated SDIO bus
 *
 * @pdev: platform device we created for self-probe
 * @bus_if: brcmf_bus allocated during probe
 * @settings: module parameters obtained via brcmf_get_module_param()
 * @hwsim_ops: operations provided by brcmfmac_hwsim module
 * @hwsim_ctx: opaque context for hwsim_ops calls
 * @get_ops_fn: cached symbol_get pointer (for symbol_put on teardown)
 * @rxctl_ready: completion signalled by hwsim when ctrl response ready
 * @shutting_down: set true before teardown to reject late callbacks
 */
struct brcmf_sim_sdio {
	struct platform_device *pdev;
	struct brcmf_bus *bus_if;
	struct brcmf_mp_device *settings;

	const struct brcmf_hwsim_ops *hwsim_ops;
	void *hwsim_ctx;

	typeof(&brcmf_hwsim_get_ops) get_ops_fn;

	struct completion rxctl_ready;
	bool shutting_down;
};

/* Forward declarations */
static int sim_sdio_probe(struct platform_device *pdev);
static void sim_sdio_remove_pdev(struct platform_device *pdev);

/* ----------------------------------------------------------------
 * Callbacks from hwsim module (struct brcmf_hwsim_cb)
 * Called from workqueue context in the hwsim module.
 * ---------------------------------------------------------------- */

/**
 * sim_cb_rxctl_ready - hwsim signals a control response is ready
 * @ctx: our brcmf_sim_sdio *
 */
static void sim_cb_rxctl_ready(void *ctx)
{
	struct brcmf_sim_sdio *sim = ctx;

	if (sim->shutting_down)
		return;
	complete(&sim->rxctl_ready);
}

/**
 * sim_cb_rx_data - hwsim delivers a data/event frame
 * @ctx: our brcmf_sim_sdio *
 * @skb: frame with BCDC data header (ownership transferred to us)
 */
static void sim_cb_rx_data(void *ctx, struct sk_buff *skb)
{
	struct brcmf_sim_sdio *sim = ctx;

	if (sim->shutting_down) {
		dev_kfree_skb_any(skb);
		return;
	}

	/* Hand to brcmfmac core; handle_event=true lets firmware events
	 * be processed; inirq=false since we're in workqueue context. */
	brcmf_rx_frame(&sim->pdev->dev, skb, true, false);
}

static const struct brcmf_hwsim_cb sim_hwsim_cb = {
	.rx_ctl_ready	= sim_cb_rxctl_ready,
	.rx_data	= sim_cb_rx_data,
};

/* ----------------------------------------------------------------
 * Bus operations (struct brcmf_bus_ops)
 * These are called by the brcmfmac core/protocol layer.
 * ---------------------------------------------------------------- */

static struct brcmf_sim_sdio *dev_to_sim(struct device *dev)
{
	struct brcmf_bus *bus_if = dev_get_drvdata(dev);

	return bus_if->bus_priv.sim;
}

/**
 * sim_bus_stop - mandatory stop callback (DR-02: no NULL guard!)
 *
 * Called by brcmf_detach() → brcmf_bus_stop(). Must be safe to call
 * multiple times.
 */
static void sim_bus_stop(struct device *dev)
{
	struct brcmf_sim_sdio *sim = dev_to_sim(dev);

	brcmf_dbg(SIM, "bus stop\n");
	sim->shutting_down = true;
}

/**
 * sim_bus_txctl - send a BCDC control message to virtual firmware
 *
 * Forwards the raw BCDC dcmd buffer to hwsim, then waits for the
 * rx_ctl_ready callback (completion) which signals the response is
 * available for retrieval via rxctl.
 */
static int sim_bus_txctl(struct device *dev, unsigned char *msg, uint len)
{
	struct brcmf_sim_sdio *sim = dev_to_sim(dev);
	int ret;

	if (sim->shutting_down)
		return -ESHUTDOWN;

	reinit_completion(&sim->rxctl_ready);

	ret = sim->hwsim_ops->tx_ctl(sim->hwsim_ctx, msg, len);
	if (ret < 0)
		return ret;

	/* Wait for hwsim to signal response ready (5 second timeout) */
	if (!wait_for_completion_timeout(&sim->rxctl_ready,
					 msecs_to_jiffies(5000)))
		return -ETIMEDOUT;

	return 0;
}

/**
 * sim_bus_rxctl - retrieve BCDC control response from virtual firmware
 *
 * Returns positive byte count on success (D10: NOT zero!).
 * The BCDC layer uses the return value as the response length.
 */
static int sim_bus_rxctl(struct device *dev, unsigned char *msg, uint len)
{
	struct brcmf_sim_sdio *sim = dev_to_sim(dev);

	if (sim->shutting_down)
		return -ESHUTDOWN;

	return sim->hwsim_ops->rx_ctl(sim->hwsim_ctx, msg, len);
}

/**
 * sim_bus_txdata - forward data frame to virtual firmware
 *
 * After forwarding, MUST call brcmf_proto_bcdc_txcomplete() (D1).
 */
static int sim_bus_txdata(struct device *dev, struct sk_buff *skb)
{
	struct brcmf_sim_sdio *sim = dev_to_sim(dev);
	int ret;

	if (sim->shutting_down) {
		brcmf_proto_bcdc_txcomplete(dev, skb, false);
		return -ESHUTDOWN;
	}

	ret = sim->hwsim_ops->tx_data(sim->hwsim_ctx, skb);

	/* TX completion — skb ownership was transferred but we still
	 * need to signal completion to the protocol layer */
	brcmf_proto_bcdc_txcomplete(dev, skb, ret == 0);

	return ret;
}

/**
 * sim_bus_get_blob - firmware blob request handler
 *
 * Returns -ENOENT for all blob types (CLM, TxCap).
 * This op is MANDATORY — bus.h has no NULL guard (E1).
 */
static int sim_bus_get_blob(struct device *dev, const struct firmware **fw,
			    enum brcmf_blob_type type)
{
	return -ENOENT;
}

static const struct brcmf_bus_ops sim_sdio_bus_ops = {
	.stop		= sim_bus_stop,
	.txdata		= sim_bus_txdata,
	.txctl		= sim_bus_txctl,
	.rxctl		= sim_bus_rxctl,
	.get_blob	= sim_bus_get_blob,
};

/* ----------------------------------------------------------------
 * Platform driver probe / remove
 * ---------------------------------------------------------------- */

static int sim_sdio_probe(struct platform_device *pdev)
{
	struct brcmf_sim_sdio *sim;
	struct brcmf_bus *bus_if;
	int ret;

	brcmf_dbg(SIM, "probe: creating simulated SDIO bus\n");

	sim = kzalloc(sizeof(*sim), GFP_KERNEL);
	if (!sim)
		return -ENOMEM;

	sim->pdev = pdev;
	init_completion(&sim->rxctl_ready);
	sim->shutting_down = false;

	/* Step 1: Obtain hwsim operations via symbol_get (D16) */
	sim->get_ops_fn = symbol_get(brcmf_hwsim_get_ops);
	if (!sim->get_ops_fn) {
		brcmf_err("brcmfmac_hwsim module not loaded\n");
		ret = -ENODEV;
		goto fail_free_sim;
	}

	/* Step 2: Register callbacks + get ops (before fw_download, M12) */
	ret = sim->get_ops_fn(&sim_hwsim_cb, sim,
			      &sim->hwsim_ops, &sim->hwsim_ctx);
	if (ret) {
		brcmf_err("brcmf_hwsim_get_ops failed: %d\n", ret);
		goto fail_symbol_put;
	}

	/* Step 3: Allocate and configure brcmf_bus */
	bus_if = kzalloc(sizeof(*bus_if), GFP_KERNEL);
	if (!bus_if) {
		ret = -ENOMEM;
		goto fail_hwsim_detach;
	}
	sim->bus_if = bus_if;

	bus_if->dev = &pdev->dev;
	bus_if->bus_priv.sim = sim;
	bus_if->proto_type = BRCMF_PROTO_BCDC;
	bus_if->chip = SIM_CHIP_ID;
	bus_if->chiprev = SIM_CHIP_REV;
	bus_if->fwvid = BRCMF_FWVENDOR_WCC;
	bus_if->maxctl = 8192; /* overwritten by bcdc_attach to DCMD_MAXLEN+hdr */
	bus_if->ops = &sim_sdio_bus_ops;

	/* Step 4: dev_set_drvdata BEFORE brcmf_alloc (D8) —
	 * brcmf_alloc() calls dev_get_drvdata() internally */
	dev_set_drvdata(&pdev->dev, bus_if);

	/* Step 5: Simulate firmware download */
	ret = sim->hwsim_ops->fw_download(sim->hwsim_ctx, NULL, 0, NULL, 0);
	if (ret) {
		brcmf_err("hwsim fw_download failed: %d\n", ret);
		goto fail_free_bus;
	}

	/* Step 6: Get module parameters (D6) */
	sim->settings = brcmf_get_module_param(&pdev->dev,
					       BRCMF_BUSTYPE_SIM,
					       SIM_CHIP_ID, SIM_CHIP_REV);
	if (!sim->settings) {
		ret = -ENOMEM;
		goto fail_free_bus;
	}

	/* Step 7: Allocate driver state (wiphy, etc.) */
	ret = brcmf_alloc(&pdev->dev, sim->settings);
	if (ret) {
		brcmf_err("brcmf_alloc failed: %d\n", ret);
		goto fail_release_settings;
	}

	/* Step 8: Attach — creates wlan0, initializes BCDC proto, etc. */
	ret = brcmf_attach(&pdev->dev);
	if (ret) {
		brcmf_err("brcmf_attach failed: %d\n", ret);
		goto fail_brcmf_free;
	}

	brcmf_dbg(SIM, "probe complete — simulated SDIO bus attached\n");
	return 0;

fail_brcmf_free:
	brcmf_free(&pdev->dev);
fail_release_settings:
	brcmf_release_module_param(sim->settings);
	sim->settings = NULL;
fail_free_bus:
	dev_set_drvdata(&pdev->dev, NULL);
	kfree(bus_if);
	sim->bus_if = NULL;
fail_hwsim_detach:
	sim->hwsim_ops->detach(sim->hwsim_ctx);
fail_symbol_put:
	symbol_put(brcmf_hwsim_get_ops);
	sim->get_ops_fn = NULL;
fail_free_sim:
	kfree(sim);
	return ret;
}

/**
 * sim_sdio_remove_pdev - platform driver remove callback
 *
 * Full teardown protocol (B5/D14/DR-06):
 *   1. shutting_down = true (stop accepting callbacks)
 *   2. hwsim_ops->detach() (tell hwsim to stop sending)
 *   3. brcmf_detach() (calls bus_stop, removes interfaces)
 *   4. brcmf_free() (frees wiphy)
 *   5. brcmf_release_module_param()
 *   6. symbol_put() (release hwsim module reference)
 */
static void sim_sdio_remove_pdev(struct platform_device *pdev)
{
	struct brcmf_bus *bus_if = dev_get_drvdata(&pdev->dev);
	struct brcmf_sim_sdio *sim;

	if (!bus_if)
		return;

	sim = bus_if->bus_priv.sim;

	/* Step 1: Stop accepting callbacks */
	sim->shutting_down = true;

	/* Step 2: Tell hwsim to stop */
	if (sim->hwsim_ops)
		sim->hwsim_ops->detach(sim->hwsim_ctx);

	/* Step 3: Detach brcmfmac core (DR-06: calls bus_stop internally) */
	brcmf_detach(&pdev->dev);

	/* Step 4: Free wiphy and driver state */
	brcmf_free(&pdev->dev);

	/* Step 5: Release module parameters (D14) */
	if (sim->settings) {
		brcmf_release_module_param(sim->settings);
		sim->settings = NULL;
	}

	/* Step 6: Release hwsim module reference */
	if (sim->get_ops_fn) {
		symbol_put(brcmf_hwsim_get_ops);
		sim->get_ops_fn = NULL;
	}

	dev_set_drvdata(&pdev->dev, NULL);
	kfree(bus_if);
	kfree(sim);
}

static struct platform_driver sim_sdio_driver = {
	.probe		= sim_sdio_probe,
	.remove_new	= sim_sdio_remove_pdev,
	.driver		= {
		.name	= SIM_PLATFORM_NAME,
	},
};

/* We hold onto the platform device so we can unregister it in exit */
static struct platform_device *sim_pdev;
static struct work_struct sim_pdev_add_work;
static bool sim_driver_registered;

/*
 * Defer platform_device_add() to a work item so that sim_sdio_probe()
 * does not execute while brcmfmac's module_init() still holds
 * module_mutex.  The probe path calls __request_module() for the
 * vendor plugin (brcmfmac-wcc / bca / cyw), which must wait for
 * module_mutex; running it in-line from module_init deadlocks.
 */
static void sim_pdev_add_worker(struct work_struct *work)
{
	int ret;

	if (!sim_pdev)
		return;

	ret = platform_device_add(sim_pdev);
	if (ret) {
		brcmf_err("sim_sdio: platform_device_add failed: %d\n", ret);
		platform_device_put(sim_pdev);
		sim_pdev = NULL;
	}
}

/**
 * brcmf_sim_sdio_register - called from brcmf_core_init()
 *
 * Allocates a platform_device, registers the platform driver, and
 * schedules platform_device_add() to run asynchronously once
 * module_init has released module_mutex.
 */
int brcmf_sim_sdio_register(void)
{
	int ret;

	brcmf_dbg(SIM, "registering simulated SDIO bus\n");

	sim_pdev = platform_device_alloc(SIM_PLATFORM_NAME, PLATFORM_DEVID_NONE);
	if (!sim_pdev)
		return -ENOMEM;

	ret = platform_driver_register(&sim_sdio_driver);
	if (ret) {
		platform_device_put(sim_pdev);
		sim_pdev = NULL;
		return ret;
	}
	sim_driver_registered = true;

	INIT_WORK(&sim_pdev_add_work, sim_pdev_add_worker);
	schedule_work(&sim_pdev_add_work);

	return 0;
}

/**
 * brcmf_sim_sdio_exit - called from brcmf_core_exit()
 */
void brcmf_sim_sdio_exit(void)
{
	brcmf_dbg(SIM, "unregistering simulated SDIO bus\n");

	/* Ensure the deferred add has finished before we tear down */
	flush_work(&sim_pdev_add_work);

	if (sim_driver_registered) {
		platform_driver_unregister(&sim_sdio_driver);
		sim_driver_registered = false;
	}

	if (sim_pdev) {
		platform_device_unregister(sim_pdev);
		sim_pdev = NULL;
	}
}

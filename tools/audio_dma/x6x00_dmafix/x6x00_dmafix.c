// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * x6x00_dmafix: a TEST of mainline's c90b6973daa3 ("dmaengine: sun6i: fix non-atomic read of DMA position registers")
 * on a running 6.1.82 kernel, where sun6i-dma is built in. It points the controller's device_tx_status at a copy of
 * 6.1.82's sun6i_dma_tx_status (with 9096bdc8d930's NULL order) whose position read is:
 *   fix=1: mainline's: LLI_ADDR, CUR_CNT, LLI_ADDR again, repeated until both LLI_ADDR reads agree;
 *   fix=0: 6.1.82's: LLI_ADDR, CUR_CNT once (a third read only counts a descriptor change that may have fallen
 *          between them: "crossed").
 * retries / crossed count those cases. Unloading puts the built-in function back.
 * The struct layouts are 6.1.82's sun6i-dma.c, the headers its drivers/dma/virt-dma.h and dmaengine.h.
 * Not for the image: the fix itself goes into the kernel (board/X6100/patches/linux 9105).
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "x6x00_virt-dma.h"

#define DMA_CHAN_LLI_ADDR 0x08
#define DMA_CHAN_CUR_CNT  0x18
#define LLI_LAST_ITEM     0xfffff800

struct sun6i_dma_lli {
	u32 cfg;
	u32 src;
	u32 dst;
	u32 len;
	u32 para;
	u32 p_lli_next;
	struct sun6i_dma_lli *v_lli_next;
};

struct sun6i_desc {
	struct virt_dma_desc vd;
	dma_addr_t p_lli;
	struct sun6i_dma_lli *v_lli;
};

struct sun6i_vchan;

struct sun6i_pchan {
	u32 idx;
	void __iomem *base;
	struct sun6i_vchan *vchan;
	struct sun6i_desc *desc;
	struct sun6i_desc *done;
};

struct sun6i_vchan {
	struct virt_dma_chan vc;
	struct list_head node;
	struct dma_slave_config cfg;
	struct sun6i_pchan *phy;
	u8 port;
	u8 irq_type;
	bool cyclic;
};

static char *dev_name_p = "1c02000.dma-controller";
module_param_named(dev, dev_name_p, charp, 0444);
static bool fix = true;
module_param(fix, bool, 0644);
static atomic_t retries = ATOMIC_INIT(0), crossed = ATOMIC_INIT(0), calls = ATOMIC_INIT(0);
module_param_named(retries, retries.counter, int, 0444);
module_param_named(crossed, crossed.counter, int, 0444);
module_param_named(calls, calls.counter, int, 0444);

static struct dma_device *dd;
static enum dma_status (*orig_tx_status)(struct dma_chan *, dma_cookie_t, struct dma_tx_state *);

static size_t chan_size(struct sun6i_pchan *pchan)
{
	struct sun6i_desc *txd = pchan->desc;
	struct sun6i_dma_lli *lli;
	size_t bytes;
	dma_addr_t pos;

	if (READ_ONCE(fix)) {
		for (;;) {
			pos = readl(pchan->base + DMA_CHAN_LLI_ADDR);
			bytes = readl(pchan->base + DMA_CHAN_CUR_CNT);
			if (pos == readl(pchan->base + DMA_CHAN_LLI_ADDR))
				break;
			atomic_inc(&retries);
		}
	} else {
		pos = readl(pchan->base + DMA_CHAN_LLI_ADDR);
		bytes = readl(pchan->base + DMA_CHAN_CUR_CNT);
		if (pos != readl(pchan->base + DMA_CHAN_LLI_ADDR))
			atomic_inc(&crossed);
	}

	if (pos == LLI_LAST_ITEM)
		return bytes;

	for (lli = txd->v_lli; lli; lli = lli->v_lli_next) {
		if (lli->p_lli_next == pos) {
			for (lli = lli->v_lli_next; lli; lli = lli->v_lli_next)
				bytes += lli->len;
			break;
		}
	}
	return bytes;
}

static enum dma_status fixed_tx_status(struct dma_chan *chan, dma_cookie_t cookie, struct dma_tx_state *state)
{
	struct sun6i_vchan *vchan = container_of(chan, struct sun6i_vchan, vc.chan);
	struct sun6i_pchan *pchan = vchan->phy;
	struct sun6i_dma_lli *lli;
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;
	size_t bytes = 0;

	ret = dma_cookie_status(chan, cookie, state);
	if (ret == DMA_COMPLETE || !state)
		return ret;

	atomic_inc(&calls);
	spin_lock_irqsave(&vchan->vc.lock, flags);
	vd = vchan_find_desc(&vchan->vc, cookie);
	if (vd) {
		struct sun6i_desc *txd = container_of(&vd->tx, struct sun6i_desc, vd.tx);

		for (lli = txd->v_lli; lli != NULL; lli = lli->v_lli_next)
			bytes += lli->len;
	} else if (!pchan || !pchan->desc) {
		bytes = 0;
	} else {
		bytes = chan_size(pchan);
	}
	spin_unlock_irqrestore(&vchan->vc.lock, flags);

	dma_set_residue(state, bytes);
	return ret;
}

static int __init dmafix_init(void)
{
	struct device *dev = bus_find_device_by_name(&platform_bus_type, NULL, dev_name_p);

	if (!dev)
		return -ENODEV;
	if (!dev->driver || strcmp(dev->driver->name, "sun6i-dma")) {
		pr_err("x6x00_dmafix: %s is not bound to sun6i-dma\n", dev_name_p);
		put_device(dev);
		return -ENODEV;
	}
	dd = dev_get_drvdata(dev);         /* struct sun6i_dma_dev, whose first member is the dma_device */
	put_device(dev);
	if (!dd || dd->dev != dev || !dd->device_tx_status) {
		pr_err("x6x00_dmafix: %s: not the layout expected\n", dev_name_p);
		return -EINVAL;
	}
	orig_tx_status = dd->device_tx_status;
	WRITE_ONCE(dd->device_tx_status, fixed_tx_status);
	pr_info("x6x00_dmafix: %s: device_tx_status %ps -> %ps (fix=%d)\n", dev_name_p, orig_tx_status,
		fixed_tx_status, fix);
	return 0;
}

static void __exit dmafix_exit(void)
{
	WRITE_ONCE(dd->device_tx_status, orig_tx_status);
	msleep(500);                        /* a caller still in fixed_tx_status (microseconds, interrupts off) */
	pr_info("x6x00_dmafix: restored; calls %d, retries %d, crossed %d\n", atomic_read(&calls),
		atomic_read(&retries), atomic_read(&crossed));
}

module_init(dmafix_init);
module_exit(dmafix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEST: sun6i-dma position read fixed (mainline c90b6973daa3) on a running 6.1.82");

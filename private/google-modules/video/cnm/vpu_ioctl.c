// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Codec3P video accelerator
 *
 * Copyright 2022 Google LLC.
 *
 * Author: Vinay Kalia <vinaykalia@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fdtable.h>
#include <linux/dma-heap.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/units.h>
#include <uapi/linux/dma-buf.h>
#include <soc/google/google-cdd.h>

#include "vpu_priv.h"
#include "vpu_pm.h"
#include "vpu_of.h"
#include "vpu_secure.h"
#include "vpu_slc.h"
#include "vpu_coredump.h"
#include "wave6_regdefine.h"

#define VPU_DEVCLASS_NAME "vpu_codec"
#define VPU_CHRDEV_NAME "vpu"
#define VPU_FW_NAME "seurat.bin"
/* b/304879706#comment13, 1MB is enough for text + common_data section.
   Use 2MB for potential separation of text and common data */
#define VPU_FW_CODE_SIZE (2 * 1024 * 1024)

void vpu_update_system_state(struct vpu_core *core)
{
	/*
	 * vpu_power_status        bit[0:2]
	 * vpu_power_cycled        bit[3]
	 * user_idle               bit[4]
	 */
	google_cdd_set_system_dev_stat(CDD_SYSTEM_DEVICE_VPU,
						(core->power_status & 0x7) |
						((core->vpu_power_cycled & 0x1) << 3) |
						((core->user_idle & 0x1)        << 4));
}

/* Given inst_idx, get pointer to instance from core */
static inline struct vpu_inst *vpu_get_inst(struct vpu_core *core, uint32_t inst_idx)
{
	struct vpu_inst *inst;
	unsigned long flags;

	if (inst_idx >= MAX_NUM_INST) {
		pr_err("%s instance index exceeds max allowed: %u\n",
			__func__, inst_idx);
		return NULL;
	}
	spin_lock_irqsave(&core->inst_lock, flags);
	inst = core->instances[inst_idx];
	spin_unlock_irqrestore(&core->inst_lock, flags);
	return inst;
}

static int vpu_set_inst(struct vpu_core *core, uint32_t inst_idx, struct vpu_inst *inst)
{
	unsigned long flags;

	if (inst_idx >= MAX_NUM_INST) {
		pr_err("%s instance index exceeds max allowed: %u\n",
			__func__, inst_idx);
		return -ENODEV;
	}
	spin_lock_irqsave(&core->inst_lock, flags);
	if (inst && core->instances[inst_idx]) {
		spin_unlock_irqrestore(&core->inst_lock, flags);
		pr_err("%s previous instance index %u is not closed properly\n",
			__func__, inst_idx);
		return -EINVAL;
	}
	core->instances[inst_idx] = inst;
	spin_unlock_irqrestore(&core->inst_lock, flags);

	return 0;
}

static int vpu_alloc_intr_queue(struct vpu_intr_queue *intr_queue)
{
	int rc = 0;

	rc = kfifo_alloc(&intr_queue->intr_pending_q, MAX_INTERRUPT_QUEUE*sizeof(uint32_t), GFP_KERNEL);
	if (rc) {
		pr_err("failed to alloc interrupt pending queue\n");
		goto err_alloc_fail;
	}

	init_waitqueue_head(&intr_queue->wq);
	spin_lock_init(&intr_queue->kfifo_lock);

err_alloc_fail:
	return rc;
}

static void vpu_free_intr_queue(struct vpu_intr_queue *intr_queue)
{
	kfifo_free(&intr_queue->intr_pending_q);
}

static int vpu_open_inst(struct vpu_core *core, uint32_t inst_idx)
{
	int rc = 0;
	struct vpu_inst *inst;

	if (!core) {
		rc = -ENOMEM;
		pr_err("failed to get VPU core\n");
		goto err;
	}

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst) {
		rc = -ENOMEM;
		pr_err("failed to create instance\n");
		goto err;
	}

	rc = vpu_alloc_intr_queue(&inst->intr_queue);
	if (rc) {
		pr_err("failed to alloc interrupt pending queue\n");
		goto err_intr_queue_alloc;
	}

	rc = vpu_set_inst(core, inst_idx, inst);
	if (rc) {
		pr_err("failed to set vpu instance\n");
		goto err_inst_idx;
	}

	inst->core = core;
	inst->idx = inst_idx;
	pr_info("opened VPU instance index %d\n", inst_idx);
	return rc;

err_inst_idx:
	vpu_free_intr_queue(&inst->intr_queue);
err_intr_queue_alloc:
	kfree(inst);
err:
	return rc;
}

static struct list_head *vpu_get_alloc_list(struct vpu_core *core, uint32_t inst_idx,
					    struct mutex **lock)
{
	*lock = NULL;

	if (inst_idx == VPU_NO_INST) {
		*lock = &core->lock;
		return &core->allocs;
	}

	if (inst_idx >= MAX_NUM_INST) {
		pr_err("get alloc with invalid index %d\n", inst_idx);
		return NULL;
	}

	*lock = &core->dmabuf_list[inst_idx].lock;
	return &core->dmabuf_list[inst_idx].allocs;
}

struct vpu_intr_queue *vpu_get_intr_queue(struct vpu_core *core, uint32_t inst_idx)
{
	struct vpu_inst *inst;

	if (inst_idx == VPU_NO_INST) {
		return &core->intr_queue;
	}

	inst = vpu_get_inst(core, inst_idx);
	if (inst == NULL) {
		pr_err("get intr queue failed with index %d\n", inst_idx);
		return NULL;
	}

	return &inst->intr_queue;
}

static int vpu_close_inst(struct vpu_inst *inst)
{
	struct vpu_core *core;
	unsigned int inst_idx;

	if (!inst) {
		pr_err("VPU instance is NULL");
		return -EINVAL;
	}

	core = inst->core;
	inst_idx = inst->idx;
	vpu_set_inst(core, inst->idx, NULL);
	vpu_free_intr_queue(&inst->intr_queue);
	kfree(inst);

	pr_info("closed VPU instance index %d\n", inst_idx);
	return 0;
}

static int __vpu_map_dma_buf(struct vpu_dmabuf_info *dma_info, struct device *dev)
{
	if (!dma_info) {
		pr_err("dma_info is NULL");
		return -EINVAL;
	}

	dma_info->attachment = dma_buf_attach(dma_info->dma_buf, dev);
	if (IS_ERR(dma_info->attachment)) {
		pr_err("Failed to dma_buf_attach: %ld\n", PTR_ERR(dma_info->attachment));
		goto err_attach;
	}

	if (dma_info->skip_cmo)
		dma_info->attachment->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	dma_info->sgt = dma_buf_map_attachment_unlocked(dma_info->attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(dma_info->sgt)) {
		pr_err("Failed to get sgt: %ld\n", PTR_ERR(dma_info->sgt));
		goto err_map_attachment;
	}

	dma_info->pa = page_to_phys(sg_page(dma_info->sgt->sgl));
	dma_info->iova = sg_dma_address(dma_info->sgt->sgl);
	if (IS_ERR_VALUE(dma_info->iova)) {
		pr_err("Failed to get iova\n");
		goto err_iova;
	}
	return 0;
err_iova:
	dma_buf_unmap_attachment_unlocked(dma_info->attachment, dma_info->sgt, DMA_BIDIRECTIONAL);
	dma_info->iova = 0;

err_map_attachment:
	dma_buf_detach(dma_info->dma_buf, dma_info->attachment);
	dma_info->sgt = NULL;
err_attach:
	return -ENOMEM;
}

static void __vpu_unmap_dma_buf(struct vpu_dmabuf_info *dma_info)
{
	if (!dma_info) {
		pr_err("dma_info is NULL");
		return;
	}

	if (dma_info->attachment && dma_info->sgt) {
		dma_buf_unmap_attachment_unlocked(dma_info->attachment,
				dma_info->sgt, DMA_BIDIRECTIONAL);
	}
	if (dma_info->attachment)
		dma_buf_detach(dma_info->dma_buf, dma_info->attachment);
}

static struct vpu_dmabuf_info* vpu_alloc_fw_buf(struct vpu_core *core, uint32_t size)
{
	struct dma_heap *dma_heap;
	struct vpu_dmabuf_info *dma_info;
	const char *heapname = "vpu_fw-secure";

	dma_info = kzalloc(sizeof(*dma_info), GFP_KERNEL);
	if (!dma_info) {
		pr_err("Failed to allocate memory for vpu_dmabuf_info\n");
		return NULL;
	}
	INIT_LIST_HEAD(&dma_info->list);

	dma_heap = dma_heap_find(heapname);
	if (!dma_heap) {
		pr_err("Failed to find dma heap for %s\n", heapname);
		goto err_find_dma_heap;
	}

	dma_info->dma_buf = dma_heap_buffer_alloc(dma_heap, size, 0, 0);
	if (IS_ERR(dma_info->dma_buf)) {
		pr_err("Failed to allocate dma buffer: %ld\n", PTR_ERR(dma_info->dma_buf));
		goto err_dma_alloc;
	}

	if (__vpu_map_dma_buf(dma_info, core->dev)){
		pr_err("Failed to map dma buffer\n");
		goto err_map_dma_buf;
	}

	if (dma_buf_vmap_unlocked(dma_info->dma_buf, &dma_info->vmap)) {
		pr_err("Failed to get kernel virtual address\n");
		goto err_vmap;
	}

	dma_heap_put(dma_heap);
	dma_info->size = size;
	return dma_info;
err_vmap:
	__vpu_unmap_dma_buf(dma_info);
err_map_dma_buf:
	dma_buf_put(dma_info->dma_buf);
err_dma_alloc:
	dma_heap_put(dma_heap);
	dma_info->dma_buf = NULL;
err_find_dma_heap:
	kfree(dma_info);
	return NULL;
}

static int vpu_alloc_dma_buf(struct vpu_core *core, struct list_head *alloc_list, struct mutex *list_lock, struct vpu_dmabuf *dmabuf)
{
	struct dma_heap *dma_heap;
	struct vpu_dmabuf_info *dma_info;
	const char *heapname = dmabuf->heap_name;

	if (!alloc_list || !dmabuf) {
		pr_err("alloc list or dmabuf is NULL\n");
		return -EINVAL;
	}

	dma_info = kzalloc(sizeof(*dma_info), GFP_KERNEL);
	if (!dma_info) {
		pr_err("Failed to allocate memory for vpu_dmabuf_info\n");
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&dma_info->list);

	dma_heap = dma_heap_find(heapname);
	if (!dma_heap) {
		pr_err("Failed to find dma heap for %s\n", heapname);
		goto err_find_dma_heap;
	}

	dma_info->dma_buf = dma_heap_buffer_alloc(dma_heap, dmabuf->size, O_RDWR, 0);
	if (IS_ERR(dma_info->dma_buf)) {
		pr_err("Failed to allocate dma buffer: %ld\n", PTR_ERR(dma_info->dma_buf));
		goto err_dma_alloc;
	}

	if (__vpu_map_dma_buf(dma_info, core->dev)){
		pr_err("Failed to map dma buffer\n");
		goto err_map_dma_buf;
	}

	// acquire one more reference for user space fd
	get_dma_buf(dma_info->dma_buf);

	dma_info->fd = dma_buf_fd(dma_info->dma_buf, O_ACCMODE);
	if (dma_info->fd < 0) {
		pr_err("Failed to get fd\n");
		goto err_fd;
	}

	dma_heap_put(dma_heap);
	dma_info->size = dmabuf->size;
	mutex_lock(list_lock);
	list_add_tail(&dma_info->list, alloc_list);
	mutex_unlock(list_lock);
	dmabuf->iova = dma_info->iova;
	dmabuf->fd = dma_info->fd;
	return 0;

err_fd:
	__vpu_unmap_dma_buf(dma_info);
err_map_dma_buf:
	dma_buf_put(dma_info->dma_buf);
err_dma_alloc:
	dma_heap_put(dma_heap);
	dma_info->dma_buf = NULL;
err_find_dma_heap:
	kfree(dma_info);
	return -ENOMEM;
}

static struct vpu_dmabuf_info * pull_dmabuf_info(struct list_head *l, struct mutex *list_lock, struct vpu_dmabuf *dmabuf)
{
	struct vpu_dmabuf_info *curr, *next;
	struct vpu_dmabuf_info *found = NULL;

	mutex_lock(list_lock);
	list_for_each_entry_safe(curr, next, l, list) {
		if (curr->fd == dmabuf->fd) {
			list_del(&curr->list);
			found = curr;
			break;
		}
	}
	mutex_unlock(list_lock);
	return found;
}

static void _vpu_free_dma_info(struct vpu_dmabuf_info *dma_info)
{
	if (!dma_info)
		return;

	__vpu_unmap_dma_buf(dma_info);
	if (dma_info->vmap.vaddr)
		dma_buf_vunmap_unlocked(dma_info->dma_buf, &dma_info->vmap);
	if (dma_info->dma_buf)
		dma_buf_put(dma_info->dma_buf);
	kfree(dma_info);
}

static void vpu_free_dma_buf(struct list_head *l, struct mutex *list_lock, struct vpu_dmabuf *dmabuf)
{
	struct vpu_dmabuf_info *dma_info;

	dma_info = pull_dmabuf_info(l, list_lock, dmabuf);
	if (!dma_info) {
		pr_err("Failed to find dmabuf fd : %d\n", dmabuf->fd);
		return;
	}
	_vpu_free_dma_info(dma_info);
}

static int vpu_map_dma_buf(struct vpu_inst *inst, struct vpu_dmabuf *dmabuf)
{
	struct vpu_dmabuf_info *dma_info = kzalloc(sizeof(*dma_info), GFP_KERNEL);
	struct vpu_core *core;
	struct vpu_dmabuf_list *dmabuf_list;

	if (!dma_info) {
		pr_err("Failed to allocate memory for vpu_dmabuf_info\n");
		return -ENOMEM;
	}
	dma_info->dma_buf = dma_buf_get(dmabuf->fd);
	if (IS_ERR(dma_info->dma_buf)) {
		int rc = PTR_ERR(dma_info->dma_buf);
		pr_err("failed to get dma buf(%d): %d\n", dmabuf->fd, rc);
		goto err_buf_get;
	}

	dma_info->skip_cmo = dmabuf->skip_cmo;
	if (__vpu_map_dma_buf(dma_info, inst->core->dev)){
		pr_err("Failed to map dma buffer\n");
		goto err_map_dma_buf;
	}
	dma_info->fd = dmabuf->fd;
	dma_info->size = dmabuf->size;

	core = inst->core;
	dmabuf_list = &core->dmabuf_list[inst->idx];
	mutex_lock(&dmabuf_list->lock);
	list_add_tail(&dma_info->list, &dmabuf_list->mappings);
	mutex_unlock(&dmabuf_list->lock);

	dmabuf->iova = dma_info->iova;
	return 0;
err_map_dma_buf:
	dma_buf_put(dma_info->dma_buf);
err_buf_get:
	kfree(dma_info);
	return -ENOMEM;
}

static int vpu_reload_fw(struct vpu_core *core)
{
	int rc;
	const struct firmware *fw_blob;

	rc = request_firmware(&fw_blob, VPU_FW_NAME, core->dev);
	if (rc) {
		dev_err(core->dev, "Failed to request firmware: %s\n", VPU_FW_NAME);
		goto exit;
	}
	/* un-protect fw buffer and clear smmu mapping */
	rc = vpu_secure_fw_unprot(&core->secure);
	if (rc) {
		dev_err(core->dev, "Failed to un-protect firmware %d\n", rc);
		goto exit;
	}

	if (dma_buf_vmap_unlocked(core->fw_buf->dma_buf, &core->fw_buf->vmap)) {
		dev_err(core->dev, "Failed to get kernel virtual address\n");
		rc = -ENOMEM;
		goto exit;
	}
	dma_sync_sgtable_for_cpu(core->dev, core->fw_buf->sgt, DMA_FROM_DEVICE);
	memcpy(core->fw_buf->vmap.vaddr, fw_blob->data, fw_blob->size);

	/* cache flush for memcpy by CPU */
	dma_sync_sgtable_for_device(core->dev, core->fw_buf->sgt, DMA_TO_DEVICE);
	dev_info(core->dev, "start to prot fw. pa %pad size %zu blob size %zu\n",
		&core->fw_buf->pa, core->fw_buf->size, fw_blob->size);

	/* unmap fw buffer to avoid speculative accesses from non-secure */
	dma_buf_vunmap_unlocked(core->fw_buf->dma_buf, &core->fw_buf->vmap);

	/* protect fw buffer and create smmu mapping */
	rc = vpu_secure_fw_prot(&core->secure, core->fw_buf, fw_blob->size);
	if (rc) {
		dev_err(core->dev, "Failed to protect firmware %d\n", rc);
		goto exit;
	}

exit:
	release_firmware(fw_blob);
	return rc;
}

static int vpu_unload_fw(struct vpu_core *core)
{
	int rc = 0;

	if (!core->fw_buf)
		return 0;

	/* un-protect fw buffer and clear smmu mapping when disconnecting the tipc channel */
	vpu_secure_deinit(&core->secure);
	_vpu_free_dma_info(core->fw_buf);
	core->fw_buf = NULL;

	return rc;
}

static int vpu_load_fw(struct vpu_core *core)
{
	int rc = 0;
	const struct firmware *fw_blob;

	rc = request_firmware(&fw_blob, VPU_FW_NAME, core->dev);
	if (rc) {
		pr_err("Failed to request firmware: %s", VPU_FW_NAME);
		goto err_req_fw;
	}
	if (core->fw_buf) {
		_vpu_free_dma_info(core->fw_buf);
	}
	core->fw_buf = vpu_alloc_fw_buf(core, VPU_FW_CODE_SIZE);
	if (!core->fw_buf) {
		pr_err("Failed to allocate buffer of size %zu bytes for firmware",
				fw_blob->size);
		rc = -ENOMEM;
		goto err_fw_alloc;
	}
	memcpy(core->fw_buf->vmap.vaddr, fw_blob->data, fw_blob->size);

	/* unmap fw buffer to avoid speculative accesses from non-secure */
	dma_buf_vunmap_unlocked(core->fw_buf->dma_buf, &core->fw_buf->vmap);

	// cache flush for memcpy by CPU
	dma_sync_sgtable_for_device(core->dev, core->fw_buf->sgt, DMA_TO_DEVICE);
	pr_info("start to prot fw. pa %pad size %zu blob size %zu",
		&core->fw_buf->pa, core->fw_buf->size, fw_blob->size);

	/* connect the tipc channel */
	rc = vpu_secure_init(&core->secure);
	if (rc) {
		pr_err("failed to init secure channel\n");
		goto err_secure_init;
	}
	/* protect fw buffer and create smmu mapping */
	rc = vpu_secure_fw_prot(&core->secure, core->fw_buf, fw_blob->size);
	if (rc) {
		pr_err("Failed to prot fw\n");
		goto err_prot_fw;
	}
	release_firmware(fw_blob);
	return rc;
err_prot_fw:
	vpu_secure_deinit(&core->secure);
err_secure_init:
	_vpu_free_dma_info(core->fw_buf);
err_fw_alloc:
	release_firmware(fw_blob);
err_req_fw:
	return rc;
}

static int vpu_signal_interrupt(struct vpu_core *core, const struct vpu_intr_info *intr_info)
{
	struct vpu_intr_queue *intr_queue;

	intr_queue = vpu_get_intr_queue(core, intr_info->inst_idx);
	if (intr_queue == NULL) {
		pr_err("%s Failed to get intr_queue for index %u\n", __func__, intr_info->inst_idx);
		return -EINVAL;
	}

	if (kfifo_is_empty_spinlocked(&intr_queue->intr_pending_q, &intr_queue->kfifo_lock)) {
		kfifo_in_spinlocked(&intr_queue->intr_pending_q,
							&intr_info->reason,
							sizeof(u32),
							&intr_queue->kfifo_lock);
		wake_up(&intr_queue->wq);
		pr_debug("inst %d is signalled\n", intr_info->inst_idx);
	}

	return 0;
}

static int vpu_set_clk_rate(struct vpu_core *core, uint32_t clk_rate)
{
	int rc;

	if (!core->dev_freq.df) {
		pr_err("set rate without dev_freq\n");
		return -EINVAL;
	}

	rc = dev_pm_qos_update_request(&core->dev_freq.qos_req, clk_rate / HZ_PER_KHZ);
	if (rc < 0) {
		pr_err("dev_pm_qos_update_request failed: %d\n", rc);
		return rc;
	}

	return rc;
}

/*
 * defined in document Quality_of_Service_PAS section Virtual Channels
 * Soft Real time(VC1) is designed for the clients that have the specific purpose and
 * thus can define the target bandwidth by scenarios, e.g. image post process, codec
 */
#define VC_SOFT_REALTIME 1

static int vpu_set_icc_update(struct google_icc_path *icc_path,
			      const struct vpu_bandwidth_info *bandwidth_info)
{
	int rc;

	rc = google_icc_set_read_bw_gmc(icc_path, bandwidth_info->read_avg_bw,
					bandwidth_info->read_peak_bw, 0, VC_SOFT_REALTIME);
	if (rc) {
		pr_err("google_icc_set_read_bw_gmc failed %d\n", rc);
		return rc;
	}

	rc = google_icc_set_write_bw_gmc(icc_path, bandwidth_info->write_avg_bw,
					 bandwidth_info->write_peak_bw, 0, VC_SOFT_REALTIME);
	if (rc) {
		pr_err("google_icc_set_write_bw_gmc failed %d\n", rc);
		return rc;
	}

	rc = google_icc_update_constraint_async(icc_path);
	if (rc) {
		pr_err("google_icc_update_constraint_async failed %d\n", rc);
		return rc;
	}

	return rc;
}

static int vpu_set_bandwidth(struct vpu_core *core, const struct vpu_bandwidth_info *bandwidth_info)
{
	int rc = 0;

	dev_dbg(core->dev,
		"set bandwidth read avg %dMBps peak %dMBps write avg %dMBps peak %dMBps",
		bandwidth_info->read_avg_bw,
		bandwidth_info->read_peak_bw,
		bandwidth_info->write_avg_bw,
		bandwidth_info->write_peak_bw);

	/*
	* GMC bandwidth means votes go through SSWRP -> fabric -> GSLC -> GMC
	* GSLC bandwidth means votes go through SSWRP -> fabric -> GSLC
	*/

	rc = vpu_set_icc_update(core->icc_path.path_gmc, bandwidth_info);
	if (rc) {
		dev_err(core->dev, "update icc for gmc fails %d\n", rc);
		return rc;
	}

	/* so far no need to vote for path_gslc because we haven't enabled SLC */

	return rc;
}

int vpu_dma_buf_sync(struct vpu_buf_sync *sync)
{
	int ret;
	struct dma_buf *dmabuf;
	enum dma_data_direction direction;
	u64 flags;

	flags = sync->flags;
	if (flags & ~DMA_BUF_SYNC_VALID_FLAGS_MASK)
		return -EINVAL;

	switch (flags & DMA_BUF_SYNC_RW) {
	case DMA_BUF_SYNC_READ:
		direction = DMA_FROM_DEVICE;
		break;
	case DMA_BUF_SYNC_WRITE:
		direction = DMA_TO_DEVICE;
		break;
	case DMA_BUF_SYNC_RW:
		direction = DMA_BIDIRECTIONAL;
		break;
	default:
		return -EINVAL;
	}

	dmabuf = dma_buf_get(sync->fd);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		pr_err("failed to get dma buf(%d): %d\n", sync->fd, ret);
		return ret;
	}

	ret = -1;

	if (flags & DMA_BUF_SYNC_END) {
		if (dmabuf->ops->end_cpu_access_partial)
			ret = dma_buf_end_cpu_access_partial(dmabuf, direction,
						sync->offset, sync->size);
		if (ret < 0)
			ret = dma_buf_end_cpu_access(dmabuf, direction);
	} else {
		if (dmabuf->ops->begin_cpu_access_partial)
			ret = dma_buf_begin_cpu_access_partial(dmabuf, direction,
						sync->offset, sync->size);
		if (ret < 0)
			ret = dma_buf_begin_cpu_access(dmabuf, direction);
	}
	dma_buf_put(dmabuf);

	return ret;
}

static long vpu_unlocked_ioctl(struct file *fp, unsigned int cmd,
				unsigned long arg)
{
	int rc = 0;
	struct vpu_inst *inst = NULL;
	struct vpu_core *core =
		container_of(fp->f_inode->i_cdev, struct vpu_core, cdev);
	void __user *user_desc = (void __user *)arg;

	switch (cmd) {
		case VPU_IOCX_GET_REG_SZ:
			{
				pr_debug("Inside VPU_IOCX_GET_REG_SZ\n");
				if (copy_to_user(user_desc, &core->regs_size, sizeof(uint32_t))) {
					pr_err("Failed to copy to user\n");
					rc = -EFAULT;
				}
				pr_debug("VDI_IOCTL_GET_REG_SZ, size=%u\n", core->regs_size);
				break;
			}
		case VPU_IOCX_OPEN_INSTANCE:
			{
				pr_debug("Inside VPU_IOCX_OPEN_INSTANCE\n");
				rc = vpu_open_inst(core, (uint32_t)arg);
				break;
			}
		case VPU_IOCX_CLOSE_INSTANCE:
			pr_debug("Inside VPU_IOCX_CLOSE_INSTANCE\n");
			inst = vpu_get_inst(core, (uint32_t)arg);
			if (!inst) {
				pr_err("Failed to get inst for index %u\n", (uint32_t)arg);
				return -EINVAL;
			}
			rc = vpu_close_inst(inst);
			break;
		case VPU_IOCX_ALLOC_DMABUF:
			{
				struct vpu_dmabuf dmabuf;
				struct list_head *alloc_list;
				struct mutex *list_lock;

				pr_debug("Inside VPU_IOCX_ALLOC_DMABUF\n");
				if (copy_from_user(&dmabuf, user_desc, sizeof(dmabuf))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				alloc_list = vpu_get_alloc_list(core, dmabuf.inst_idx, &list_lock);
				if (!alloc_list) {
					pr_err("Failed to find alloc list for index %u", dmabuf.inst_idx);
					rc = -EINVAL;
					break;
				}
				/* add string terminator */
				dmabuf.heap_name[MAX_HEAP_NAME - 1] = 0;
				rc = vpu_alloc_dma_buf(core, alloc_list, list_lock, &dmabuf);
				if (rc) {
					pr_err("Failed to allocate dmabuf\n");
					break;
				}
				if (copy_to_user(user_desc, &dmabuf, sizeof(dmabuf))) {
					pr_err("Failed to copy to user\n");
					rc = -EFAULT;
				}
				break;
			}
		case VPU_IOCX_FREE_DMABUF:
			{
				struct vpu_dmabuf dmabuf;
				struct list_head *alloc_list;
				struct mutex *list_lock;

				pr_debug("Inside VPU_IOCX_FREE_DMABUF\n");
				if (copy_from_user(&dmabuf, user_desc, sizeof(dmabuf))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				alloc_list = vpu_get_alloc_list(core, dmabuf.inst_idx, &list_lock);
				if (!alloc_list) {
					pr_err("Failed to find alloc list for index %u", dmabuf.inst_idx);
					rc = -EINVAL;
					break;
				}
				vpu_free_dma_buf(alloc_list, list_lock, &dmabuf);
				if (copy_to_user(user_desc, &dmabuf, sizeof(dmabuf))) {
					pr_err("Failed to copy to user\n");
					rc = -EFAULT;
				}
				break;
			}
		case VPU_IOCX_GET_IOVA:
			{
				struct vpu_dmabuf dmabuf;

				if (copy_from_user(&dmabuf, user_desc, sizeof(dmabuf))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				inst = vpu_get_inst(core, dmabuf.inst_idx);
				if (!inst) {
					pr_err("Failed to get inst for index %u\n", dmabuf.inst_idx);
					return -EINVAL;
				}
				rc = vpu_map_dma_buf(inst, &dmabuf);
				if (rc) {
					pr_err("Failed to allocate dmabuf\n");
					break;
				}
				if (copy_to_user(user_desc, &dmabuf, sizeof(dmabuf))) {
					pr_err("Failed to copy to user\n");
					rc = -EFAULT;
				}
				break;
			}
		case VPU_IOCX_PUT_IOVA:
			{
				struct vpu_dmabuf dmabuf;

				if (copy_from_user(&dmabuf, user_desc, sizeof(dmabuf))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				if (dmabuf.inst_idx >= MAX_NUM_INST) {
					pr_err("Put iova with invalid index %u\n", dmabuf.inst_idx);
					return -EINVAL;
				}
				vpu_free_dma_buf(&core->dmabuf_list[dmabuf.inst_idx].mappings,
						 &core->dmabuf_list[dmabuf.inst_idx].lock,
						 &dmabuf);
				if (copy_to_user(user_desc, &dmabuf, sizeof(dmabuf))) {
					pr_err("Failed to copy to user\n");
					rc = -EFAULT;
				}
				break;
			}
		case VPU_IOCX_WAIT_INTERRUPT:
			{
				struct vpu_intr_queue *intr_queue;
				struct vpu_intr_info intr_info;
				long ret = 0;
				uint32_t intr_reason;
				uint32_t num_elements;

				if (copy_from_user(&intr_info, user_desc, sizeof(intr_info))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				intr_queue = vpu_get_intr_queue(core, intr_info.inst_idx);
				if (!intr_queue) {
					pr_err("Failed to get inst for index %u\n", intr_info.inst_idx);
					return -EINVAL;
				}

				ret = wait_event_interruptible_timeout(intr_queue->wq,
							kfifo_len(&intr_queue->intr_pending_q),
							msecs_to_jiffies(intr_info.timeout));
				if (!ret) {
					pr_info("Either video is paused or timed out waiting on HW.\n");
					rc = -ETIMEDOUT;
				} else {
					num_elements = kfifo_out_spinlocked(&intr_queue->intr_pending_q, &intr_reason,
							sizeof(u32), &intr_queue->kfifo_lock);
					if (num_elements > 0) {
						intr_info.reason = intr_reason;
					} else {
						//TODO (vinaykalia@): Why would this ever happen?
						intr_info.reason = 0;
					}
					rc = 0;
				}
				if (copy_to_user(user_desc, &intr_info, sizeof(intr_info))) {
					pr_err("Failed to copy to user\n");
					rc = -EFAULT;
				}
				break;
			}
		case VPU_IOCX_SIGNAL_INTERRUPT:
			{
				struct vpu_intr_info intr_info;
				if (copy_from_user(&intr_info, user_desc, sizeof(intr_info))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				pr_debug("Inside VPU_IOCX_SIGNAL_INTERRUPT\n");
				rc = vpu_signal_interrupt(core, &intr_info);
				break;
			}
		case VPU_IOCX_SET_CLK_RATE:
			{
				pr_debug("Inside VPU_IOCX_SET_CLK_RATE %d\n", (uint32_t)arg);
				if (core->debugfs.fixed_rate) {
					pr_info("Ignore set_clk_rate when fixed_rate is set\n");
					return -EPERM;
				}
				rc = vpu_set_clk_rate(core, (uint32_t)arg);
				break;
			}
		case VPU_IOCX_SET_BW:
			{
				struct vpu_bandwidth_info bandwidth_info;

				pr_debug("Inside VPU_IOCX_SET_BW\n");
				if (copy_from_user(&bandwidth_info, user_desc, sizeof(bandwidth_info))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}
				rc = vpu_set_bandwidth(core, &bandwidth_info);
				break;
			}
		case VPU_IOCX_DMA_BUF_SYNC:
			{
				struct vpu_buf_sync sync;
				if (copy_from_user(&sync, user_desc, sizeof(sync))) {
					pr_err("Failed to copy from user\n");
					rc = -EFAULT;
					break;
				}
				rc = vpu_dma_buf_sync(&sync);
				if (rc)
					pr_err("Error dma sync: %d\n", sync.fd);
				break;
			}
		case VPU_IOCX_SSCD_COREDUMP:
			{
				struct dma_buf *dma_buf;
				struct iosys_map vmap;
				struct vpu_dump_info dbg_info;
				struct vpu_coredump_info dump_info;

				pr_debug("Inside VPU_IOCX_SSCD_COREDUMP\n");
				if (copy_from_user(&dump_info, user_desc, sizeof(dump_info))) {
					pr_err("Failed to copy from user\n");
					return -EFAULT;
				}

				dma_buf = dma_buf_get(dump_info.fd);

				if (dma_buf_vmap_unlocked(dma_buf, &vmap)) {
					pr_err("Failed to get kernel virtual address for coredump\n");
					dma_buf_put(dma_buf);
					return -ENOMEM;
				}

				dbg_info.size = dump_info.size;
				dbg_info.addr = vmap.vaddr;
				core->need_reload_fw = true;
				vpu_do_sscoredump(core, &dbg_info);
				dma_buf_vunmap_unlocked(dma_buf, &vmap);
				dma_buf_put(dma_buf);
				break;
			}
		case VPU_IOCX_NOTIFY_IDLE:
			if (core->user_idle == arg)
				break;

			core->user_idle = arg;

			if (arg)
				rc = vpu_pm_suspend(core->dev);
			else
				rc = vpu_pm_resume(core->dev);

			break;

		default:
			pr_err("Inside default cmd 0x%x\n", cmd);
			rc = -EINVAL;
			break;
	}

	return rc;
}

static int vpu_mmap(struct file *fp, struct vm_area_struct *vm)
{
	unsigned long pfn;
	struct vpu_core *core =
		container_of(fp->f_inode->i_cdev, struct vpu_core, cdev);

	vm_flags_set(vm, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	/* This is a CSRs mapping, use pgprot_device */
	vm->vm_page_prot = pgprot_device(vm->vm_page_prot);
	pfn = core->paddr >> PAGE_SHIFT;

	return remap_pfn_range(vm, vm->vm_start, pfn, vm->vm_end-vm->vm_start, vm->vm_page_prot) ? -EAGAIN : 0;
}

static int vpu_open(struct inode *inode, struct file *file)
{
	struct vpu_core *core =
		container_of(file->f_inode->i_cdev, struct vpu_core, cdev);
	int rc;
	pr_debug("Inside vpu open\n");

	if (core->need_reload_fw) {
		/* after coredump FW may be corrupted (b/389175194#comment30) */
		dev_info(core->dev, "reload FW since coredump has been triggered\n");
		rc = vpu_reload_fw(core);
		if (rc < 0)
			return rc;
		core->need_reload_fw = false;
	}

	rc = vpu_pm_power_on(core);
	if (rc < 0)
		return rc;

	if (core->debugfs.fixed_rate) {
		pr_info("set rate at %d\n", core->debugfs.fixed_rate);
		vpu_set_clk_rate(core, core->debugfs.fixed_rate);
	}

	rc = vpu_pt_client_enable(core);
	if (rc < 0) {
		pr_err("failed to enable SLC\n");
		if (vpu_pm_power_off(core) < 0)
			pr_err("failed to power off\n");
		return rc;
	}

	return rc;
}

static int vpu_release(struct inode *inode, struct file *file)
{
	struct vpu_dmabuf_info *curr, *temp;
	struct vpu_core *core =
		container_of(file->f_inode->i_cdev, struct vpu_core, cdev);
	uint32_t inst_idx;
	struct vpu_bandwidth_info zero_bandwidth = {0};

	vpu_pt_client_disable(core);

	core->user_idle = false;
	if (vpu_pm_power_off(core))
		return -EPERM;

	/* reset bandwidth setting in case codec hal process crashes */
	vpu_set_bandwidth(core, &zero_bandwidth);

	/* we could safely free resources after F/W is stopped by powering off */
	mutex_lock(&core->lock);
	list_for_each_entry_safe(curr, temp, &core->allocs, list) {
		dev_warn(core->dev, "core has leaked allocation fd %d iova %pad",
				curr->fd, &curr->iova);
		list_del(&curr->list);
		_vpu_free_dma_info(curr);
	}
	mutex_unlock(&core->lock);

	for (inst_idx = 0; inst_idx < MAX_NUM_INST; inst_idx++) {
		struct vpu_inst *inst = vpu_get_inst(core, inst_idx);

		if (inst == NULL)
			continue;
		dev_warn(core->dev, "instance %d is leaked\n", inst_idx);
		vpu_close_inst(inst);
	}

	for (inst_idx = 0; inst_idx < MAX_NUM_INST; inst_idx++) {
		struct vpu_dmabuf_list *dmabuf_list = &core->dmabuf_list[inst_idx];

		mutex_lock(&dmabuf_list->lock);
		list_for_each_entry_safe(curr, temp, &dmabuf_list->allocs, list) {
			dev_warn(core->dev, "inst[%d] has leaked allocation fd %d iova %pad\n",
					inst_idx, curr->fd, &curr->iova);
			list_del(&curr->list);
			_vpu_free_dma_info(curr);
		}

		list_for_each_entry_safe(curr, temp, &dmabuf_list->mappings, list) {
			dev_warn(core->dev, "inst[%d] has leaked mapping fd %d iova %pad\n",
					inst_idx, curr->fd, &curr->iova);
			list_del(&curr->list);
			_vpu_free_dma_info(curr);
		}
		mutex_unlock(&dmabuf_list->lock);
	}

	return 0;
}

static const struct file_operations vpu_fops = {
	.owner = THIS_MODULE,
	.open = vpu_open,
	.release = vpu_release,
	.unlocked_ioctl = vpu_unlocked_ioctl,
	.compat_ioctl = vpu_unlocked_ioctl,
	.mmap = vpu_mmap,
};

static int init_chardev(struct vpu_core *core)
{
	int rc;

	cdev_init(&core->cdev, &vpu_fops);
	core->cdev.owner = THIS_MODULE;
	rc = alloc_chrdev_region(&core->devno, 0, 1, VPU_CHRDEV_NAME);
	if (rc < 0) {
		pr_err("Failed to alloc chrdev region\n");
		goto err;
	}
	rc = cdev_add(&core->cdev, core->devno, 1);
	if (rc) {
		pr_err("Failed to register chrdev\n");
		goto err_cdev_add;
	}

	core->_class = class_create(VPU_DEVCLASS_NAME);
	if (IS_ERR(core->_class)) {
		rc = PTR_ERR(core->_class);
		goto err_class_create;
	}

	core->svc_dev = device_create(core->_class, NULL, core->cdev.dev, core,
			VPU_CHRDEV_NAME);
	if (IS_ERR(core->svc_dev)) {
		pr_err("device_create err\n");
		rc = PTR_ERR(core->svc_dev);
		goto err_device_create;
	}
	return rc;

err_device_create:
	class_destroy(core->_class);
err_class_create:
	cdev_del(&core->cdev);
err_cdev_add:
	unregister_chrdev_region(core->devno, 1);
err:
	return rc;
}

static void deinit_chardev(struct vpu_core *core)
{
	if (!core)
		return;

	device_destroy(core->_class, core->devno);
	class_destroy(core->_class);
	cdev_del(&core->cdev);
	unregister_chrdev_region(core->devno, 1);
}

static inline u32 get_inst_idx(u32 reg_val)
{
	u32 inst_idx;
	int i;

	for (i = 0; i < MAX_NUM_INST; i++)
	{
		if(((reg_val >> i) & 0x01) == 1)
			break;
	}
	inst_idx = i;
	return inst_idx;
}

static irqreturn_t vpu_isr(int irq, void *arg)
{
	uint32_t inst_idx = 0;
	uint32_t intr_reason, report_intr_reason = 0;
	uint32_t done_inst;
	struct vpu_core *core = (struct vpu_core *)arg;
	struct vpu_intr_queue *intr_queue = NULL;
	unsigned long flags;

	//unblock the instance.
	intr_reason = READ_VPU_REGISTER(core, W6_VPU_VINT_REASON);
	done_inst = READ_VPU_REGISTER(core, W6_CMD_DONE_INST);

	report_intr_reason = intr_reason;
	spin_lock_irqsave(&core->inst_lock, flags);
	if (intr_reason & (1 << INT_WAVE6_SLEEP_VPU)) {
		uint32_t sleep_reason = READ_VPU_REGISTER(core, W6_RET_SLEEP_INT_REASON);

		if (sleep_reason == 0)
			report_intr_reason = (1 << INT_WAVE6_SLEEP_VPU_SUCCESS);
		else if (sleep_reason == 0x02)
			report_intr_reason = (1 << INT_WAVE6_SLEEP_VPU_IDLE);
		intr_queue = &core->intr_queue;
	} else {
		do {
			struct vpu_inst *inst;

			inst_idx = get_inst_idx(done_inst);
			if (inst_idx >= MAX_NUM_INST) {
				pr_err("core wide interrupt or something wrong: %u\n", inst_idx);
				break;
			}
			inst = core->instances[inst_idx];
			if (!inst) {
				pr_warn("inst for index %u is null\n", inst_idx);
				break;
			}
			intr_queue = &inst->intr_queue;
		} while (0);
	}
	WRITE_VPU_REGISTER(core, W6_VPU_VINT_REASON_CLR, intr_reason);
	WRITE_VPU_REGISTER(core, W6_VPU_VINT_CLEAR, 1);

	if (intr_queue == NULL) {
		pr_warn("%s null intr_queue with intr_reason 0x%x: %u\n",
				__func__, intr_reason, inst_idx);
		spin_unlock_irqrestore(&core->inst_lock, flags);
		return IRQ_HANDLED;
	}
	if (!kfifo_is_full(&intr_queue->intr_pending_q)) {
		kfifo_in_spinlocked(&intr_queue->intr_pending_q,
				    &report_intr_reason,
				    sizeof(u32),
				    &intr_queue->kfifo_lock);
		wake_up(&intr_queue->wq);
		if (report_intr_reason == INT_WAVE6_INIT_VPU)
			pr_warn("received INT_WAVE6_INIT_VPU for inst %d", done_inst);
	} else {
		pr_err("interrupt pending queue is full: %u. Dropping interrupt 0x%x\n",
				kfifo_len(&intr_queue->intr_pending_q), report_intr_reason);
	}
	spin_unlock_irqrestore(&core->inst_lock, flags);

	return IRQ_HANDLED;
}

static int vpu_init_io(struct vpu_core *core)
{
	struct platform_device *pdev = to_platform_device(core->dev);
	int rc;

	rc = devm_request_irq(&pdev->dev, core->irq, vpu_isr, IRQF_SHARED,
			dev_name(&pdev->dev), core);

	if (rc < 0)
		pr_err("failed to request irq: %d\n", rc);
	return rc;
}

static void vpu_release_io(struct vpu_core *core)
{
}

static int vpu_probe(struct platform_device *pdev)
{
	int rc = 0, i;
	struct vpu_core *core;

	dev_info(&pdev->dev, "Inside VPU probe");

	/* No limit on DMA segment size */
	dma_set_max_seg_size(&pdev->dev, UINT_MAX);

	core = devm_kzalloc(&pdev->dev, sizeof(struct vpu_core), GFP_KERNEL);
	if (!core) {
		rc = -ENOMEM;
		goto err;
	}

	core->dev = &pdev->dev;
	platform_set_drvdata(pdev, core);

	rc = init_chardev(core);
	if (rc) {
		dev_err(&pdev->dev, "Failed to initialize chardev for vpu: %d\n", rc);
		goto err_init_chardev;
	}

	rc = vpu_of_dt_parse(core);
	if (rc) {
		dev_err(&pdev->dev, "Failed to parse DT node\n");
		goto err_dt_parse;
	}

	rc = vpu_init_io(core);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to request irq\n");
		goto err_io;
	}

	rc = vpu_alloc_intr_queue(&core->intr_queue);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to alloc global interrupt queue\n");
		goto err_intr_queue_alloc;
	}

	rc = vpu_pm_init(core);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to init power domain %d\n", rc);
		goto err_pm_init;
	}

	rc = vpu_load_fw(core);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to load fw %d\n", rc);
		goto err_load_fw;
	}

	rc = vpu_pt_client_register(pdev->dev.of_node, core);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to register pt client %d\n", rc);
		goto err_load_fw;
	}

	vpu_init_debugfs(&core->debugfs);
	INIT_LIST_HEAD(&core->allocs);
	for (i = 0; i < MAX_NUM_INST; i++) {
		INIT_LIST_HEAD(&core->dmabuf_list[i].allocs);
		INIT_LIST_HEAD(&core->dmabuf_list[i].mappings);
		mutex_init(&core->dmabuf_list[i].lock);
	}
	mutex_init(&core->lock);
	spin_lock_init(&core->inst_lock);
	core->power_status = POWER_OFF_RELEASED;
	vpu_update_system_state(core);

	dev_info(&pdev->dev, "Inside VPU probe success");

	rc = vpu_sscd_dev_register(core);
	if (rc)
		dev_err(&pdev->dev, "failed to register sscd\n");

	return rc;
err_load_fw:
	vpu_pm_deinit(core);
err_pm_init:
	vpu_free_intr_queue(&core->intr_queue);
err_intr_queue_alloc:
	vpu_release_io(core);
err_io:
	vpu_of_dt_release(core);
err_dt_parse:
	deinit_chardev(core);
err_init_chardev:
	platform_set_drvdata(pdev, NULL);
err:
	return rc;
}

static int vpu_remove(struct platform_device *pdev)
{
	struct vpu_core *core = (struct vpu_core *)platform_get_drvdata(pdev);

	vpu_sscd_dev_unregister(core);

	vpu_deinit_debugfs(&core->debugfs);
	vpu_pt_client_unregister(core);
	vpu_unload_fw(core);
	vpu_pm_deinit(core);
	vpu_free_intr_queue(&core->intr_queue);
	vpu_release_io(core);
	vpu_of_dt_release(core);
	deinit_chardev(core);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct dev_pm_ops vpu_pm_ops = {
	SET_RUNTIME_PM_OPS(vpu_runtime_suspend, vpu_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(vpu_pm_suspend, vpu_pm_resume)
};

static const struct of_device_id vpu_dt_match[] = {
	{ .compatible = "google,vpu" },
	{}
};

static struct platform_driver vpu_driver = {
	.probe = vpu_probe,
	.remove = vpu_remove,
	.driver = {
		.name = "google,vpu",
		.owner = THIS_MODULE,
		.pm = &vpu_pm_ops,
		.of_match_table = vpu_dt_match,
	},
};

module_platform_driver(vpu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vinay Kalia <vinaykalia@google.com>");
MODULE_DESCRIPTION("Codec3P VPU driver");
MODULE_IMPORT_NS(DMA_BUF);

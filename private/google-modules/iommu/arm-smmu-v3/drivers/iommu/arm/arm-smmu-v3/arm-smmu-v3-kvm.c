// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_pkvm.h>
#include <asm/kvm_mmu.h>
#include <linux/cma.h>
#include <linux/dma-map-ops.h>
#include <linux/local_lock.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/panic_notifier.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>

#include "pkvm/arm_smmu_v3.h"

#include "arm-smmu-v3.h"

struct host_arm_smmu_device {
	struct arm_smmu_device		smmu;
	pkvm_handle_t			id;
	u32				boot_gbpa;
	bool				hvc_pd;
	struct io_pgtable_cfg		cfg_s1;
	struct io_pgtable_cfg		cfg_s2;
};

#define smmu_to_host(_smmu) \
	container_of(_smmu, struct host_arm_smmu_device, smmu);

struct kvm_arm_smmu_master {
	struct arm_smmu_device		*smmu;
	struct device			*dev;
	struct xarray			domains;
	struct kvm_arm_smmu_stream	*streams;
	unsigned int			num_streams;
	u32				ssid_bits;
	bool				idmapped; /* Stage-2 is transparently identity mapped*/
	bool				force_cacheable;
	bool				single_page_size;
};

struct kvm_arm_smmu_stream {
	u32				id;
	struct kvm_arm_smmu_master	*master;
	struct rb_node			node;
};

struct kvm_arm_smmu_domain {
	struct iommu_domain		domain;
	struct arm_smmu_device		*smmu;
	struct mutex			init_mutex;
	pkvm_handle_t			id;
	unsigned long			type;
};

#define to_kvm_smmu_domain(_domain) \
	container_of(_domain, struct kvm_arm_smmu_domain, domain)

#ifdef MODULE
static unsigned long                   pkvm_module_token;

#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(pkvm_el2_mod_va(&kvm_nvhe_sym(x), pkvm_module_token)))
#else
#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(kern_hyp_va(lm_alias(&kvm_nvhe_sym(x)))))
#endif

static size_t				kvm_arm_smmu_cur;
static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;
static struct hyp_arm_smmu_v3_err	*kvm_arm_smmu_v3_err;
static struct host_arm_smmu_device	**host_arm_smmu_array;
static DEFINE_IDA(kvm_arm_smmu_domain_ida);
static DEFINE_PER_CPU(local_lock_t, err_lock) = INIT_LOCAL_LOCK(err_lock);

int kvm_nvhe_sym(smmu_init_hyp_module)(const struct pkvm_module_ops *ops);
extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

/*
 * Pre allocated pages that can be used from the EL2 part of the driver from atomic
 * context, ideally used for page table pages for identity domains.
 */
static int atomic_pages;
module_param(atomic_pages, int, 0);

/*
 * Load pKVM SMMUv3 module, but without probing, so the kernel driver takes over
 * control over the SMMUs, this is useful during bring up, where we can have prebuilts
 * that can run with and without the module and can just be toggled from kernel cmdline.
 */
static bool disable;
module_param(disable, bool, 0);

phys_addr_t __topup_virt_to_phys(void *virt)
{
	return __pa(virt);
}

static struct page *__kvm_arm_smmu_alloc_from_cma(gfp_t gfp)
{
	bool from_spare = (gfp & GFP_ATOMIC) == GFP_ATOMIC;
	static atomic64_t spare_p;
	struct page *p = NULL;

again:
	if (from_spare)
		return (struct page *)atomic64_cmpxchg(&spare_p, atomic64_read(&spare_p), 0);

	p = kvm_iommu_cma_alloc();
	if (!p) {
		from_spare = true;
		goto again;
	}

	/*
	 * Top-up the spare block if necessary. If we failed to update spare_p
	 * then someone did it already and we can proceed with that page.
	 */
	if (!atomic64_read(&spare_p)) {
		if (!atomic64_cmpxchg(&spare_p, 0, (u64)p))
			goto again;
	}

	return p;
}

static int __kvm_arm_smmu_topup_from_cma(size_t size, gfp_t gfp, size_t *allocated)
{
	*allocated = 0;

	while (*allocated < size) {
		struct page *p = __kvm_arm_smmu_alloc_from_cma(gfp);
		struct kvm_hyp_memcache mc;

		if (!p)
			return -ENOMEM;

		init_hyp_memcache(&mc);
		push_hyp_memcache(&mc, page_to_virt(p), __topup_virt_to_phys,
				  PMD_SHIFT - PAGE_SHIFT);

		if (kvm_call_hyp_nvhe(__pkvm_hyp_alloc_mgt_refill,
				      HYP_ALLOC_MGT_IOMMU_ID, mc.head, 1)) {
			kvm_iommu_cma_release(p);
			return -EINVAL;
		}

		*allocated += PMD_SIZE;
	}

	return 0;
}

static int kvm_arm_smmu_topup_memcache(struct arm_smccc_res *res, gfp_t gfp)
{
	struct kvm_hyp_req req;

	hyp_reqs_smccc_decode(res, &req);

	if ((res->a1 == -ENOMEM) && (req.type != KVM_HYP_REQ_TYPE_MEM)) {
		/*
		 * There is no way for drivers to populate hyp_alloc requests,
		 * so -ENOMEM + no request indicates that.
		 */
		return __pkvm_topup_hyp_alloc(1);
	} else if (req.type != KVM_HYP_REQ_TYPE_MEM) {
		return -EBADE;
	}

	if (req.mem.dest == REQ_MEM_DEST_HYP_IOMMU) {
		size_t nr_pages, from_cma = 0;
		int ret;

		nr_pages = req.mem.nr_pages;

		if (req.mem.sz_alloc < PMD_SIZE) {
			size_t size = req.mem.sz_alloc * nr_pages;

			ret = __kvm_arm_smmu_topup_from_cma(size, gfp, &from_cma);
			if (!ret)
				return 0;

			nr_pages -= from_cma / req.mem.sz_alloc;
		}

		return __pkvm_topup_hyp_alloc_mgt_gfp(HYP_ALLOC_MGT_IOMMU_ID,
						      nr_pages,
						      req.mem.sz_alloc,
						      gfp);
	} else if (req.mem.dest == REQ_MEM_DEST_HYP_ALLOC) {
		/* Fill hyp alloc*/
		return __pkvm_topup_hyp_alloc(req.mem.nr_pages);
	}

	pr_err("Bogus mem request");
	return -EBADE;
}

/*
 * Issue hypercall, and retry after filling the memcache if necessary.
 */
#define kvm_call_hyp_nvhe_mc(...)					\
({									\
	struct arm_smccc_res __res;					\
	do {								\
		__res = kvm_call_hyp_nvhe_smccc(__VA_ARGS__);		\
	} while (__res.a1 && !kvm_arm_smmu_topup_memcache(&__res, GFP_KERNEL));\
	__res.a1;							\
})

static struct platform_driver kvm_arm_smmu_driver;

static struct arm_smmu_device *
kvm_arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev;

	dev = driver_find_device_by_fwnode(&kvm_arm_smmu_driver.driver, fwnode);
	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_ops kvm_arm_smmu_ops;

static int kvm_arm_smmu_streams_cmp_key(const void *lhs, const struct rb_node *rhs)
{
	struct kvm_arm_smmu_stream *stream_rhs = rb_entry(rhs, struct kvm_arm_smmu_stream, node);
	const u32 *sid_lhs = lhs;

	if (*sid_lhs < stream_rhs->id)
		return -1;
	if (*sid_lhs > stream_rhs->id)
		return 1;
	return 0;
}

static int kvm_arm_smmu_streams_cmp_node(struct rb_node *lhs, const struct rb_node *rhs)
{
	return kvm_arm_smmu_streams_cmp_key(&rb_entry(lhs, struct kvm_arm_smmu_stream, node)->id,
					    rhs);
}

static int kvm_arm_smmu_insert_master(struct arm_smmu_device *smmu,
				      struct kvm_arm_smmu_master *master)
{
	int i;
	int ret = 0;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);

	master->streams = kcalloc(fwspec->num_ids, sizeof(*master->streams), GFP_KERNEL);
	if (!master->streams)
		return -ENOMEM;
	master->num_streams = fwspec->num_ids;

	mutex_lock(&smmu->streams_mutex);
	for (i = 0; i < fwspec->num_ids; i++) {
		struct kvm_arm_smmu_stream *new_stream = &master->streams[i];
		struct rb_node *existing;
		u32 sid = fwspec->ids[i];

		new_stream->id = sid;
		new_stream->master = master;

		existing = rb_find_add(&new_stream->node, &smmu->streams,
				       kvm_arm_smmu_streams_cmp_node);
		if (existing) {
			struct kvm_arm_smmu_master *existing_master = rb_entry(existing,
									struct kvm_arm_smmu_stream,
									node)->master;

			/* Bridged PCI devices may end up with duplicated IDs */
			if (existing_master == master)
				continue;

			dev_warn(master->dev,
				 "Aliasing StreamID 0x%x (from %s) unsupported, expect DMA to be broken\n",
				 sid, dev_name(existing_master->dev));
			ret = -ENODEV;
			break;
		}
	}

	if (ret) {
		for (i--; i >= 0; i--)
			rb_erase(&master->streams[i].node, &smmu->streams);
		kfree(master->streams);
	}
	mutex_unlock(&smmu->streams_mutex);

	return ret;
}

static void kvm_arm_smmu_remove_master(struct kvm_arm_smmu_master *master)
{
	int i;
	struct arm_smmu_device *smmu = master->smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);

	if (!smmu || !master->streams)
		return;

	mutex_lock(&smmu->streams_mutex);
	for (i = 0; i < fwspec->num_ids; i++)
		rb_erase(&master->streams[i].node, &smmu->streams);
	mutex_unlock(&smmu->streams_mutex);

	kfree(master->streams);
}

static struct iommu_device *kvm_arm_smmu_probe_device(struct device *dev)
{
	int ret;
	struct device *of_dev = dev;
	struct device *bridge = NULL;
	struct arm_smmu_device *smmu;
	struct kvm_arm_smmu_master *master;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!fwspec || fwspec->ops != &kvm_arm_smmu_ops)
		return ERR_PTR(-ENODEV);

	if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))
		return ERR_PTR(-EBUSY);

	smmu = kvm_arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return ERR_PTR(-ENODEV);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->smmu = smmu;

	ret = kvm_arm_smmu_insert_master(smmu, master);
	if (ret)
		goto err_free;

	if (dev_is_pci(dev)) {
		bridge = arm_smmu_pci_get_host_bridge_device(to_pci_dev(dev));

		if (bridge->parent)
			of_dev = bridge->parent;
	}

	device_property_read_u32(of_dev, "pasid-num-bits", &master->ssid_bits);
	master->ssid_bits = min(smmu->ssid_bits, master->ssid_bits);
	xa_init(&master->domains);
	master->idmapped = device_property_read_bool(of_dev, "iommu-idmapped");
	master->force_cacheable = device_property_read_bool(of_dev, "iommu-force-cacheable");
	master->single_page_size = device_property_read_bool(of_dev, "iommu-single-page-size");

	if (bridge)
		arm_smmu_pci_put_host_bridge_device(bridge);

	dev_iommu_priv_set(dev, master);

	if (!device_link_add(dev, smmu->dev,
			     DL_FLAG_PM_RUNTIME |
			     DL_FLAG_AUTOREMOVE_SUPPLIER)) {
		ret = -ENOLINK;
		goto err_remove_master;
	}

	return &smmu->iommu;

err_remove_master:
	kvm_arm_smmu_remove_master(master);
err_free:
	kfree(master);
	return ERR_PTR(ret);
}

static struct iommu_domain *kvm_arm_smmu_domain_alloc(unsigned type)
{
	struct kvm_arm_smmu_domain *kvm_smmu_domain;

	/*
	 * We don't support
	 * - IOMMU_DOMAIN_DMA_FQ because lazy unmap would clash with memory
	 *   donation to guests.
	 */
	if (type != IOMMU_DOMAIN_DMA &&
	    type != IOMMU_DOMAIN_UNMANAGED &&
	    type != IOMMU_DOMAIN_IDENTITY)
		return NULL;

	kvm_smmu_domain = kzalloc(sizeof(*kvm_smmu_domain), GFP_KERNEL);
	if (!kvm_smmu_domain)
		return NULL;

	mutex_init(&kvm_smmu_domain->init_mutex);

	return &kvm_smmu_domain->domain;
}

static int kvm_arm_smmu_domain_finalize(struct kvm_arm_smmu_domain *kvm_smmu_domain,
					struct kvm_arm_smmu_master *master)
{
	int ret = 0;
	struct arm_smmu_device *smmu = master->smmu;
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);
	unsigned int max_domains;

	if (kvm_smmu_domain->smmu) {
		if (kvm_smmu_domain->smmu != smmu)
			return -EINVAL;
		return 0;
	}

	kvm_smmu_domain->smmu = smmu;

	if (kvm_smmu_domain->domain.type == IOMMU_DOMAIN_IDENTITY) {
		kvm_smmu_domain->id = KVM_IOMMU_DOMAIN_IDMAP_ID;
		/*
		 * Identity domains doesn't use the DMA API, so no need to
		 * set the  domain aperture.
		 */
		return 0;
	}

	/* Default to stage-1. */
	if (smmu->features & ARM_SMMU_FEAT_TRANS_S1) {
		kvm_smmu_domain->type = KVM_ARM_SMMU_DOMAIN_S1;
		kvm_smmu_domain->domain.pgsize_bitmap = host_smmu->cfg_s1.pgsize_bitmap;
		kvm_smmu_domain->domain.geometry.aperture_end = (1UL << host_smmu->cfg_s1.ias) - 1;
		max_domains = 1 << smmu->asid_bits;
	} else {
		kvm_smmu_domain->type = KVM_ARM_SMMU_DOMAIN_S2;
		kvm_smmu_domain->domain.pgsize_bitmap = host_smmu->cfg_s2.pgsize_bitmap;
		kvm_smmu_domain->domain.geometry.aperture_end = (1UL << host_smmu->cfg_s2.ias) - 1;
		max_domains = 1 << smmu->vmid_bits;
	}
	kvm_smmu_domain->domain.geometry.force_aperture = true;

	if (master->single_page_size)
		kvm_smmu_domain->domain.pgsize_bitmap &= (SZ_4K | SZ_16K | SZ_64K);

	/*
	 * The hypervisor uses the domain_id for asid/vmid so it has to be
	 * unique, and it has to be in range of this smmu, which can be
	 * either 8 or 16 bits, this can be improved a bit to make
	 * 16 bit asids or vmids allocate from the end of the range to
	 * give more chance to the smmus with 8 bits.
	 */
	ret = ida_alloc_range(&kvm_arm_smmu_domain_ida, KVM_IOMMU_DOMAIN_NR_START,
			      min(KVM_IOMMU_MAX_DOMAINS, max_domains), GFP_KERNEL);
	if (ret < 0)
		return ret;

	kvm_smmu_domain->id = ret;

	ret = kvm_call_hyp_nvhe_mc(__pkvm_host_iommu_alloc_domain,
				   kvm_smmu_domain->id, kvm_smmu_domain->type);

	return ret;
}

static void kvm_arm_smmu_domain_free(struct iommu_domain *domain)
{
	int ret;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smmu_device *smmu = kvm_smmu_domain->smmu;

	if (smmu && (kvm_smmu_domain->domain.type != IOMMU_DOMAIN_IDENTITY)) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_iommu_free_domain, kvm_smmu_domain->id);
		ida_free(&kvm_arm_smmu_domain_ida, kvm_smmu_domain->id);
	}
	kfree(kvm_smmu_domain);
}

static int kvm_arm_smmu_detach_dev_pasid(struct host_arm_smmu_device *host_smmu,
					 struct kvm_arm_smmu_master *master,
					 ioasid_t pasid)
{
	int i, ret;
	struct arm_smmu_device *smmu = &host_smmu->smmu;
	struct kvm_arm_smmu_domain *domain = xa_load(&master->domains, pasid);

	if (!domain)
		return 0;

	for (i = 0; i < master->num_streams; i++) {
		int sid = master->streams[i].id;

		ret = kvm_call_hyp_nvhe(__pkvm_host_iommu_detach_dev,
					host_smmu->id, domain->id, sid, pasid);
		if (ret) {
			dev_err(smmu->dev, "cannot detach device %s (0x%x): %d\n",
				dev_name(master->dev), sid, ret);
			break;
		}
	}

	/*
	 * smmu->streams_mutex is taken to provide synchronization with respect to
	 * kvm_arm_smmu_handle_event(), since that acquires the same lock. Taking the
	 * lock makes domain removal atomic with respect to domain usage when reporting
	 * faults related to a domain to an IOMMU client driver. This makes it so that
	 * the domain doesn't go away while it is being used in the fault reporting
	 * logic.
	 */
	mutex_lock(&smmu->streams_mutex);
	xa_erase(&master->domains, pasid);
	mutex_unlock(&smmu->streams_mutex);

	return ret;
}

static int kvm_arm_smmu_detach_dev(struct host_arm_smmu_device *host_smmu,
				   struct kvm_arm_smmu_master *master)
{
	return kvm_arm_smmu_detach_dev_pasid(host_smmu, master, 0);
}

static void kvm_arm_smmu_release_device(struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(master->smmu);

	kvm_arm_smmu_detach_dev(host_smmu, master);
	xa_destroy(&master->domains);
	kvm_arm_smmu_remove_master(master);
	kfree(master);
	iommu_fwspec_free(dev);
}

static void kvm_arm_smmu_remove_dev_pasid(struct device *dev, ioasid_t pasid)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(master->smmu);

	kvm_arm_smmu_detach_dev_pasid(host_smmu, master, pasid);
}

static int kvm_arm_smmu_set_dev_pasid(struct iommu_domain *domain,
				      struct device *dev, ioasid_t pasid)
{
	int i, ret;
	struct arm_smmu_device *smmu;
	struct host_arm_smmu_device *host_smmu;
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	u32 ssid_bits;

	if (!master)
		return -ENODEV;

	smmu = master->smmu;
	host_smmu = smmu_to_host(smmu);

	ret = kvm_arm_smmu_detach_dev_pasid(host_smmu, master, pasid);
	if (ret)
		return ret;

	mutex_lock(&kvm_smmu_domain->init_mutex);
	ret = kvm_arm_smmu_domain_finalize(kvm_smmu_domain, master);
	mutex_unlock(&kvm_smmu_domain->init_mutex);
	if (ret)
		return ret;

	ssid_bits = master->ssid_bits;
	if (master->force_cacheable)
		ssid_bits |= ARM_SMMU_FORCE_CACHEABLE;

	for (i = 0; i < master->num_streams; i++) {
		int sid = master->streams[i].id;

		ret = kvm_call_hyp_nvhe_mc(__pkvm_host_iommu_attach_dev,
					   host_smmu->id, kvm_smmu_domain->id,
					   sid, pasid, ssid_bits);
		if (ret) {
			dev_err(smmu->dev, "cannot attach device %s (0x%x): %d\n",
				dev_name(dev), sid, ret);
			goto out_ret;
		}
	}
	ret = xa_insert(&master->domains, pasid, kvm_smmu_domain, GFP_KERNEL);

out_ret:
	if (ret)
		kvm_arm_smmu_detach_dev(host_smmu, master);
	return ret;
}

static int kvm_arm_smmu_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	/* If anything other than pasid 0 attached, we can't support through attach_dev. */
	if(!xa_empty(&master->domains) && !xa_load(&master->domains, 0))
		return -EBUSY;

	return kvm_arm_smmu_set_dev_pasid(domain, dev, 0);
}

static int kvm_arm_smmu_map_pages(struct iommu_domain *domain,
				  unsigned long iova, phys_addr_t paddr,
				  size_t pgsize, size_t pgcount, int prot,
				  gfp_t gfp, size_t *total_mapped)
{
	size_t mapped;
	size_t size = pgsize * pgcount;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smccc_res res;
	int ret;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_map_pages,
					      kvm_smmu_domain->id,
					      iova, paddr, pgsize, pgcount, prot);
		mapped = res.a1;
		ret = res.a0;
		iova += mapped;
		paddr += mapped;
		WARN_ON(mapped % pgsize);
		WARN_ON(mapped > pgcount * pgsize);
		pgcount -= mapped / pgsize;
		*total_mapped += mapped;
	} while (*total_mapped < size && !kvm_arm_smmu_topup_memcache(&res, gfp));
	if (*total_mapped < size) {
		dev_err(kvm_smmu_domain->smmu->dev,
			"failed to map iova=0x%lx paddr=%pap, mapped [0x%zx/0x%zx] bytes ret=%d\n",
			iova, &paddr,  *total_mapped, size, ret);
		return -EINVAL;
	}

	return 0;
}

static void kvm_arm_smmu_consume_err(struct arm_smmu_device *smmu)
{
	int cpu = raw_smp_processor_id();
	struct hyp_arm_smmu_v3_err *err = &kvm_arm_smmu_v3_err[cpu];

	lockdep_assert_held(this_cpu_ptr(&err_lock));

	if (err->type == HYP_ARM_SMMU_V3_ERR_CMDQ_TIMEOUT) {
		int ret;

		dev_err(smmu->dev, "cmdq time out: hw prod 0x%x hw cons 0x%x sw prod 0x%llx",
		       err->cmdq_prod, err->cmdq_cons, err->cmdq_prod_sw);
		err->type = HYP_ARM_SMMU_V3_ERR_NONE; /* Consumed */
		ret = pm_runtime_resume_and_get(smmu->dev);
		if (ret >= 0) {
			arm_smmu_print_mmu700_status(smmu);
			pm_runtime_put(smmu->dev);
		} else if (ret < 0) {
			dev_err(smmu->dev, "Failed to resume device: %d\n", ret);
		}
	}

}

static size_t kvm_arm_smmu_unmap_pages(struct iommu_domain *domain,
				       unsigned long iova, size_t pgsize,
				       size_t pgcount,
				       struct iommu_iotlb_gather *iotlb_gather)
{
	size_t unmapped;
	size_t total_unmapped = 0;
	size_t size = pgsize * pgcount;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smccc_res res;
	unsigned long flags;

	do {
		local_lock_irqsave(&err_lock, flags);
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_unmap_pages,
					      kvm_smmu_domain->id,
					      iova, pgsize, pgcount);
		kvm_arm_smmu_consume_err(kvm_smmu_domain->smmu);
		local_unlock_irqrestore(&err_lock, flags);

		unmapped = res.a1;
		total_unmapped += unmapped;
		iova += unmapped;
		WARN_ON(unmapped % pgsize);
		pgcount -= unmapped / pgsize;

		/*
		 * The page table driver can unmap less than we asked for. If it
		 * didn't unmap anything at all, then it either reached the end
		 * of the range, or it needs a page in the memcache to break a
		 * block mapping.
		 */
	} while (total_unmapped < size &&
		 (unmapped || !kvm_arm_smmu_topup_memcache(&res, GFP_ATOMIC)));

	if (total_unmapped < size)
		pr_err("smmu: failed to unmap iova=0x%lx, unmapped [0x%lx/0x%lx] bytes\n",
		       iova, total_unmapped, size);

	return total_unmapped;
}

static phys_addr_t kvm_arm_smmu_iova_to_phys(struct iommu_domain *domain,
					     dma_addr_t iova)
{
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);

	return kvm_call_hyp_nvhe(__pkvm_host_iommu_iova_to_phys, kvm_smmu_domain->id, iova);
}

static int kvm_arm_smmu_def_domain_type(struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	if (master->idmapped && atomic_pages)
		return IOMMU_DOMAIN_IDENTITY;
	return 0;
}

static bool kvm_arm_smmu_capable(struct device *dev, enum iommu_cap cap)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		/* Assume that a coherent TCU implies coherent TBUs */
		return master->smmu->features & ARM_SMMU_FEAT_COHERENCY;
	case IOMMU_CAP_NOEXEC:
	case IOMMU_CAP_DEFERRED_FLUSH:
		return true;
	default:
		return false;
	}
}

static struct iommu_ops kvm_arm_smmu_ops = {
	.capable		= kvm_arm_smmu_capable,
	.device_group		= arm_smmu_device_group,
	.of_xlate		= arm_smmu_of_xlate,
	.get_resv_regions	= arm_smmu_get_resv_regions,
	.probe_device		= kvm_arm_smmu_probe_device,
	.release_device		= kvm_arm_smmu_release_device,
	.domain_alloc		= kvm_arm_smmu_domain_alloc,
	.pgsize_bitmap		= -1UL,
	.remove_dev_pasid	= kvm_arm_smmu_remove_dev_pasid,
	.owner			= THIS_MODULE,
	.def_domain_type	= kvm_arm_smmu_def_domain_type,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= kvm_arm_smmu_attach_dev,
		.free		= kvm_arm_smmu_domain_free,
		.map_pages	= kvm_arm_smmu_map_pages,
		.unmap_pages	= kvm_arm_smmu_unmap_pages,
		.iova_to_phys	= kvm_arm_smmu_iova_to_phys,
		.set_dev_pasid	= kvm_arm_smmu_set_dev_pasid,
	}
};

static bool kvm_arm_smmu_validate_features(struct arm_smmu_device *smmu)
{
	unsigned int required_features =
		ARM_SMMU_FEAT_TT_LE;
	unsigned int forbidden_features =
		ARM_SMMU_FEAT_STALL_FORCE;
	unsigned int keep_features =
		ARM_SMMU_FEAT_2_LVL_STRTAB	|
		ARM_SMMU_FEAT_2_LVL_CDTAB	|
		ARM_SMMU_FEAT_TT_LE		|
		ARM_SMMU_FEAT_SEV		|
		ARM_SMMU_FEAT_COHERENCY		|
		ARM_SMMU_FEAT_TRANS_S1		|
		ARM_SMMU_FEAT_TRANS_S2		|
		ARM_SMMU_FEAT_VAX		|
		ARM_SMMU_FEAT_RANGE_INV;
	unsigned int known_options =
		ARM_SMMU_OPT_SKIP_PREFETCH	|
		ARM_SMMU_OPT_PAGE0_REGS_ONLY	|
		ARM_SMMU_OPT_MSIPOLL		|
		ARM_SMMU_OPT_CMDQ_FORCE_SYNC	|
		ARM_SMMU_OPT_OVR_INSTCFG_DATA	|
		ARM_SMMU_OPT_RPM_DISABLE	|
		ARM_SMMU_OPT_NON_COHERENT_TTW;

	if (smmu->options & ARM_SMMU_OPT_PAGE0_REGS_ONLY) {
		dev_err(smmu->dev, "unsupported layout\n");
		return false;
	}

	if ((smmu->features & required_features) != required_features) {
		dev_err(smmu->dev, "missing features 0x%x\n",
			required_features & ~smmu->features);
		return false;
	}

	if (smmu->features & forbidden_features) {
		dev_err(smmu->dev, "features 0x%x forbidden\n",
			smmu->features & forbidden_features);
		return false;
	}

	/*
	 * At the time of writing this driver, these are the known options
	 * that are supported and understood by the driver, any new unsupported
	 * option might break the driver or undermine its security.
	 */
	if (smmu->options & ~known_options) {
		dev_err(smmu->dev, "unknown options found 0x%x\n",
			smmu->options & ~known_options);
		return false;
	}

	smmu->features &= keep_features;

	return true;
}

static struct kvm_arm_smmu_master * kvm_arm_smmu_find_master(struct arm_smmu_device *smmu, u32 sid)
{
	struct rb_node *node;

	lockdep_assert_held(&smmu->streams_mutex);

	node = rb_find(&sid, &smmu->streams, kvm_arm_smmu_streams_cmp_key);
	if (!node)
		return NULL;
	return rb_entry(node, struct kvm_arm_smmu_stream, node)->master;
}

static void kvm_arm_smmu_decode_event(struct arm_smmu_device *smmu, u64 *raw,
				      struct arm_smmu_event *event)
{
	struct kvm_arm_smmu_master *master;

	event->id = FIELD_GET(EVTQ_0_ID, raw[0]);
	event->sid = FIELD_GET(EVTQ_0_SID, raw[0]);
	event->ssv = FIELD_GET(EVTQ_0_SSV, raw[0]);
	event->ssid = event->ssv ? FIELD_GET(EVTQ_0_SSID, raw[0]) : IOMMU_NO_PASID;
	event->read = FIELD_GET(EVTQ_1_RnW, raw[1]);
	event->iova = FIELD_GET(EVTQ_2_ADDR, raw[2]);
	event->dev = NULL;

	mutex_lock(&smmu->streams_mutex);
	master = kvm_arm_smmu_find_master(smmu, event->sid);
	if (master)
		event->dev = get_device(master->dev);
	mutex_unlock(&smmu->streams_mutex);
}

static int kvm_arm_smmu_handle_event(struct arm_smmu_device *smmu, u64 *evt,
				     struct arm_smmu_event *event)
{
	int ret = 0;
	struct kvm_arm_smmu_master *master;
	struct kvm_arm_smmu_domain *smmu_domain;

	switch (event->id) {
	case EVT_ID_TRANSLATION_FAULT:
	case EVT_ID_ADDR_SIZE_FAULT:
	case EVT_ID_ACCESS_FAULT:
	case EVT_ID_PERMISSION_FAULT:
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&smmu->streams_mutex);
	master = kvm_arm_smmu_find_master(smmu, event->sid);
	if (!master) {
		ret = -EINVAL;
		goto out_unlock;
	}

	smmu_domain = xa_load(&master->domains, event->ssid);
	if (!smmu_domain) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = report_iommu_fault(&smmu_domain->domain, master->dev, event->iova,
				 event->read ? IOMMU_FAULT_READ : IOMMU_FAULT_WRITE);

out_unlock:
	mutex_unlock(&smmu->streams_mutex);
	return ret;
}

static void kvm_arm_smmu_dump_event(struct arm_smmu_device *smmu, u64 *raw,
				    struct arm_smmu_event *evt, struct ratelimit_state *rs)
{
	int i;

	if (!__ratelimit(rs))
		return;

	dev_info(smmu->dev, "event 0x%02x received:\n", evt->id);
	for (i = 0; i < EVTQ_ENT_DWORDS; ++i)
		dev_info(smmu->dev, "\t0x%016llx\n", (unsigned long long)raw[i]);

	arm_smmu_print_evt_info(smmu, raw, evt->dev ? dev_name(evt->dev) : "(unassigned SID)");
}

static irqreturn_t kvm_arm_smmu_evt_handler(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;
	struct arm_smmu_queue *q = &smmu->evtq.q;
	struct arm_smmu_ll_queue *llq = &q->llq;
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	u64 evt[EVTQ_ENT_DWORDS];
	struct arm_smmu_event event = {0};

	if (pm_runtime_get_if_active(smmu->dev, true) == 0) {
		dev_err(smmu->dev, "Unable to handle event interrupt because device not runtime active\n");
		return IRQ_HANDLED;
	}

	do {
		while (!queue_remove_raw(q, evt)) {
			kvm_arm_smmu_decode_event(smmu, evt, &event);
			if (kvm_arm_smmu_handle_event(smmu, evt, &event))
				kvm_arm_smmu_dump_event(smmu, evt, &event, &rs);

			put_device(event.dev);
			cond_resched();
		}

		/*
		 * Not much we can do on overflow, so scream and pretend we're
		 * trying harder.
		 */
		if (queue_sync_prod_in(q) == -EOVERFLOW)
			dev_err(smmu->dev, "EVTQ overflow detected -- events lost\n");
	} while (!queue_empty(llq));

	/* Sync our overflow flag, as we believe we're up to speed */
	queue_sync_cons_ovf(q);
	pm_runtime_put(smmu->dev);
	return IRQ_HANDLED;
}

static irqreturn_t kvm_arm_smmu_gerror_handler(int irq, void *dev)
{
	u32 gerror, gerrorn, active;
	struct arm_smmu_device *smmu = dev;

	if (pm_runtime_get_if_active(smmu->dev, true) == 0) {
		dev_err(smmu->dev, "Unable to handle global error interrupt because device not runtime active\n");
		return IRQ_HANDLED;
	}

	gerror = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	gerrorn = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);

	active = gerror ^ gerrorn;
	if (!(active & GERROR_ERR_MASK)) {
		pm_runtime_put(smmu->dev);
		return IRQ_NONE; /* No errors pending */
	}

	dev_warn(smmu->dev,
		 "unexpected global error reported (0x%08x), this could be serious\n",
		 active);

	/* There is no API to reconfigure the device at the moment.*/
	if (active & GERROR_SFM_ERR)
		dev_err(smmu->dev, "device has entered Service Failure Mode!\n");

	if (active & GERROR_MSI_GERROR_ABT_ERR)
		dev_warn(smmu->dev, "GERROR MSI write aborted\n");

	if (active & GERROR_MSI_PRIQ_ABT_ERR)
		dev_warn(smmu->dev, "PRIQ MSI write aborted\n");

	if (active & GERROR_MSI_EVTQ_ABT_ERR)
		dev_warn(smmu->dev, "EVTQ MSI write aborted\n");

	if (active & GERROR_MSI_CMDQ_ABT_ERR)
		dev_warn(smmu->dev, "CMDQ MSI write aborted\n");

	if (active & GERROR_PRIQ_ABT_ERR)
		dev_err(smmu->dev, "PRIQ write aborted -- events may have been lost\n");

	if (active & GERROR_EVTQ_ABT_ERR)
		dev_err(smmu->dev, "EVTQ write aborted -- events may have been lost\n");

	if (active & GERROR_CMDQ_ERR) {
		dev_err(smmu->dev, "CMDQ ERR -- Hypervisor corruption\n");
		BUG();
	}

	writel(gerror, smmu->base + ARM_SMMU_GERRORN);

	arm_smmu_print_mmu700_status(smmu);

	pm_runtime_put(smmu->dev);
	return IRQ_HANDLED;
}

static irqreturn_t kvm_arm_smmu_pri_handler(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;

	dev_err(smmu->dev, "PRI not supported in KVM driver!\n");

	return IRQ_HANDLED;
}

static int kvm_arm_smmu_device_reset(struct host_arm_smmu_device *host_smmu)
{
	int ret;
	u32 reg;
	struct arm_smmu_device *smmu = &host_smmu->smmu;
	u32 irqen_flags = IRQ_CTRL_EVTQ_IRQEN | IRQ_CTRL_GERROR_IRQEN;

	reg = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (reg & CR0_SMMUEN)
		dev_warn(smmu->dev, "SMMU currently enabled! Resetting...\n");

	/* Disable bypass */
	host_smmu->boot_gbpa = readl_relaxed(smmu->base + ARM_SMMU_GBPA);
	ret = arm_smmu_update_gbpa(smmu, GBPA_ABORT, 0);
	if (ret)
		return ret;

	ret = arm_smmu_device_disable(smmu);
	if (ret)
		return ret;

	/* Stream table */
	writeq_relaxed(smmu->strtab_cfg.strtab_base,
		       smmu->base + ARM_SMMU_STRTAB_BASE);
	writel_relaxed(smmu->strtab_cfg.strtab_base_cfg,
		       smmu->base + ARM_SMMU_STRTAB_BASE_CFG);

	/* Command queue */
	writeq_relaxed(smmu->cmdq.q.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);

	/* Event queue */
	writeq_relaxed(smmu->evtq.q.q_base, smmu->base + ARM_SMMU_EVTQ_BASE);
	writel_relaxed(smmu->evtq.q.llq.prod, smmu->base + SZ_64K + ARM_SMMU_EVTQ_PROD);
	writel_relaxed(smmu->evtq.q.llq.cons, smmu->base + SZ_64K + ARM_SMMU_EVTQ_CONS);

	/* Disable IRQs first */
	ret = arm_smmu_write_reg_sync(smmu, 0, ARM_SMMU_IRQ_CTRL,
				      ARM_SMMU_IRQ_CTRLACK);
	if (ret) {
		dev_err(smmu->dev, "failed to disable irqs\n");
		return ret;
	}

	/*
	 * We don't support combined irqs for now, no specific reason, they are uncommon
	 * so we just try to avoid bloating the code.
	 */
	if (smmu->combined_irq)
		dev_err(smmu->dev, "Combined irqs not supported by this driver\n");
	else
		arm_smmu_setup_unique_irqs(smmu, kvm_arm_smmu_evt_handler,
					   kvm_arm_smmu_gerror_handler,
					   kvm_arm_smmu_pri_handler);

	if (smmu->features & ARM_SMMU_FEAT_PRI)
		irqen_flags |= IRQ_CTRL_PRIQ_IRQEN;

	/* Enable interrupt generation on the SMMU */
	ret = arm_smmu_write_reg_sync(smmu, irqen_flags,
				      ARM_SMMU_IRQ_CTRL, ARM_SMMU_IRQ_CTRLACK);
	if (ret)
		dev_warn(smmu->dev, "failed to enable irqs\n");

	return 0;
}

/* TODO: Move this. None of it is specific to SMMU */
static int kvm_arm_probe_power_domain(struct device *dev,
				      struct kvm_power_domain *pd)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (!of_get_property(dev->of_node, "power-domains", NULL)) {
		/* SMMU MUST RESET TO BLOCK DMA. */
		dev_warn(dev, "No power-domains assuming host control\n");
	}

	pd->type = KVM_POWER_DOMAIN_HOST_HVC;
	pd->device_id = kvm_arm_smmu_cur;
	host_smmu->hvc_pd = true;
	return 0;
}

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;
	phys_addr_t mmio_addr;
	bool coherent_walk;
	struct io_pgtable_cfg cfg_s1, cfg_s2;
	size_t mmio_size, pgd_size;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	struct host_arm_smmu_device *host_smmu;
	struct hyp_arm_smmu_v3_device *hyp_smmu;
	struct kvm_power_domain power_domain = {};
	unsigned long ias;

	if (kvm_arm_smmu_cur >= kvm_arm_smmu_count)
		return -ENOSPC;

	hyp_smmu = &kvm_arm_smmu_array[kvm_arm_smmu_cur];

	host_smmu = devm_kzalloc(dev, sizeof(*host_smmu), GFP_KERNEL);
	if (!host_smmu)
		return -ENOMEM;

	smmu = &host_smmu->smmu;
	smmu->dev = dev;

	ret = arm_smmu_fw_probe(pdev, smmu);
	if (ret)
		return ret ?: -EINVAL;

	mutex_init(&smmu->streams_mutex);
	smmu->streams = RB_ROOT;

	platform_set_drvdata(pdev, smmu);

	ret = kvm_arm_probe_power_domain(dev, &power_domain);
	if (ret)
		return ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mmio_size = resource_size(res);
	if (mmio_size < SZ_128K) {
		dev_err(dev, "unsupported MMIO region size (%pr)\n", res);
		return -EINVAL;
	}
	mmio_addr = res->start;
	host_smmu->id = kvm_arm_smmu_cur;
	host_arm_smmu_array[host_smmu->id] = host_smmu;

	smmu->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);

	arm_smmu_probe_irq(pdev, smmu);

	/* Use one page per level-2 table */
	smmu->strtab_cfg.split = PAGE_SHIFT - (ilog2(STRTAB_STE_DWORDS) + 3);

	ret = arm_smmu_device_hw_probe(smmu);
	if (ret)
		return ret;

	if (!kvm_arm_smmu_validate_features(smmu))
		return -ENODEV;

	if (kvm_arm_smmu_ops.pgsize_bitmap == -1UL)
		kvm_arm_smmu_ops.pgsize_bitmap = smmu->pgsize_bitmap;
	else
		kvm_arm_smmu_ops.pgsize_bitmap |= smmu->pgsize_bitmap;

	ias = (smmu->features & ARM_SMMU_FEAT_VAX) ? 52 : 48;

	coherent_walk = (smmu->features & ARM_SMMU_FEAT_COHERENCY) &&
			!(smmu->options & ARM_SMMU_OPT_NON_COHERENT_TTW);
	dev_dbg(dev, "coherent TTWs: %s\n", str_yes_no(coherent_walk));
	/*
	 * SMMU will hold possible configuration for both S1 and S2 as any of
	 * them can be chosen when a device is attached.
	 */
	cfg_s1 = (struct io_pgtable_cfg) {
		.fmt = ARM_64_LPAE_S1,
		.pgsize_bitmap = smmu->pgsize_bitmap,
		.ias = min_t(unsigned long, ias, VA_BITS),
		.oas = smmu->ias,
		.coherent_walk = coherent_walk,
	};
	cfg_s2 = (struct io_pgtable_cfg) {
		  .fmt = ARM_64_LPAE_S2,
		  .pgsize_bitmap = smmu->pgsize_bitmap,
		  .ias = smmu->ias,
		  .oas = smmu->oas,
		  .coherent_walk = coherent_walk,
	};

	/*
	 * Choose the page and address size. Compute the PGD size as well, so we
	 * know how much memory to pre-allocate.
	 */
	if (smmu->features & ARM_SMMU_FEAT_TRANS_S1) {
		ret = io_pgtable_configure_pixel(&cfg_s1, &pgd_size);
		if (ret)
			return ret;
		host_smmu->cfg_s1 = cfg_s1;
	}
	if (smmu->features & ARM_SMMU_FEAT_TRANS_S2) {
		ret = io_pgtable_configure_pixel(&cfg_s2, &pgd_size);
		if (ret)
			return ret;
		host_smmu->cfg_s2 = cfg_s2;
	}
	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, smmu->base,
				      ARM_SMMU_CMDQ_PROD, ARM_SMMU_CMDQ_CONS,
				      CMDQ_ENT_DWORDS, "cmdq");
	if (ret)
		return ret;

	/* evtq */
	ret = arm_smmu_init_one_queue(smmu, &smmu->evtq.q, smmu->base + SZ_64K,
				      ARM_SMMU_EVTQ_PROD, ARM_SMMU_EVTQ_CONS,
				      EVTQ_ENT_DWORDS, "evtq");
	if (ret)
		return ret;

	ret = arm_smmu_init_strtab(smmu);
	if (ret)
		return ret;

	ret = kvm_arm_smmu_device_reset(host_smmu);
	if (ret)
		return ret;

	arm_smmu_print_mmu700_status(smmu);

	ret = arm_smmu_register_iommu(smmu, &kvm_arm_smmu_ops, mmio_addr);
	if (ret)
		return ret;

	/* Hypervisor parameters */
	hyp_smmu->mmio_addr = mmio_addr;
	hyp_smmu->mmio_size = mmio_size;
	hyp_smmu->features = smmu->features;
	hyp_smmu->pgtable_cfg_s1 = cfg_s1;
	hyp_smmu->pgtable_cfg_s2 = cfg_s2;
	hyp_smmu->iommu.power_domain = power_domain;
	hyp_smmu->ssid_bits = smmu->ssid_bits;
	hyp_smmu->options = smmu->options;

	kvm_arm_smmu_cur++;

	/*
	 * The state of endpoints dictates when the SMMU is powered off. To turn
	 * the SMMU on and off, a genpd driver uses SCMI over the SMC transport,
	 * or some other platform-specific SMC. Those power requests are caught
	 * by the hypervisor, so that the hyp driver doesn't touch the hardware
	 * state while it is off.
	 *
	 * We are making a big assumption here, that TLBs and caches are invalid
	 * on power on, and therefore we don't need to wake the SMMU when
	 * modifying page tables, stream tables and context tables. If this
	 * assumption does not hold on some systems, then we'll need to grab RPM
	 * reference in map(), attach(), etc, so the hyp driver can send
	 * invalidations.
	 */
	hyp_smmu->caches_clean_on_power_on = true;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	/*
	 * Take a reference to keep the SMMU powered on while the hypervisor
	 * initializes it.
	 */
	pm_runtime_resume_and_get(dev);

	return 0;
}

static int kvm_arm_smmu_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	/*
	 * There was an error during hypervisor setup. The hyp driver may
	 * have already enabled the device, so disable it.
	 */
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	arm_smmu_unregister_iommu(smmu);
	arm_smmu_device_disable(smmu);
	arm_smmu_update_gbpa(smmu, host_smmu->boot_gbpa, GBPA_ABORT);
	host_arm_smmu_array[host_smmu->id] = NULL;
	return 0;
}

int kvm_arm_smmu_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->hvc_pd)
		return pkvm_iommu_suspend(dev);
	return 0;
}

int kvm_arm_smmu_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->hvc_pd)
		return pkvm_iommu_resume(dev);
	return 0;
}

static const struct dev_pm_ops kvm_arm_smmu_pm_ops = {
	SET_RUNTIME_PM_OPS(kvm_arm_smmu_suspend, kvm_arm_smmu_resume, NULL)
};

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver kvm_arm_smmu_driver = {
	.driver = {
		.name = "kvm-arm-smmu-v3",
		.of_match_table = arm_smmu_of_match,
		.pm = &kvm_arm_smmu_pm_ops,
	},
	.remove = kvm_arm_smmu_remove,
};

static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order, err_size;
	struct device_node *np;

	kvm_arm_smmu_count = 0;
	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		if (of_device_is_available(np))
			kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return 0;

	host_arm_smmu_array = kcalloc(kvm_arm_smmu_count,
				      sizeof(*host_arm_smmu_array), GFP_KERNEL);
	if (!host_arm_smmu_array)
		return -ENOMEM;

	/* Allocate the parameter list shared with the hypervisor */
	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
						      smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;

	err_size = NR_CPUS * sizeof(*kvm_arm_smmu_v3_err);
	kvm_arm_smmu_v3_err = (void *)alloc_pages_exact(err_size, GFP_KERNEL | __GFP_ZERO);
	if (!kvm_arm_smmu_v3_err)
		return -ENOMEM;
	return 0;
}

int smmu_put_device(struct device *dev, void *data)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	/* Keep RPM disable powered on. */
	if (!(smmu->options & ARM_SMMU_OPT_RPM_DISABLE))
		pm_runtime_put(dev);

	return 0;
}

int smmu_unregister_smmu(struct device *dev, void *data)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	arm_smmu_unregister_iommu(smmu);
	return 0;
}

static int smmu_alloc_atomic_mc(struct kvm_hyp_memcache *atomic_mc)
{
	int ret;
#ifndef MODULE
	u64 i;
	phys_addr_t start, end;

	/*
	 * Allocate pages to cover mapping with PAGE_SIZE for all memory
	 * Then allocate extra for 1GB of MMIO.
	 * Add 10 extra pages as we map the rest with first level blocks
	 * for PAGE_SIZE = 4KB, that should cover 5TB of address space.
	 */
	for_each_mem_range(i, &start, &end) {
		atomic_pages += __hyp_pgtable_max_pages((end - start) >> PAGE_SHIFT);
	}

	atomic_pages += __hyp_pgtable_max_pages(SZ_1G >> PAGE_SHIFT) + 10;
#endif

	/* Module didn't set that parameter. */
	if (!atomic_pages)
		return 0;

	/* For PGD*/
	ret = topup_hyp_memcache(atomic_mc, 1, 3);
	if (ret)
		return ret;
	ret = topup_hyp_memcache(atomic_mc, atomic_pages, 0);
	if (ret)
		return ret;
	pr_info("smmuv3: Allocated %d MiB for atomic usage\n",
		(atomic_pages << PAGE_SHIFT) / SZ_1M);
	/* Topup hyp alloc so IOMMU driver can allocate domains. */
	__pkvm_topup_hyp_alloc(1);

	return ret;
}

static int smmu_panic_err_dump(struct notifier_block *self,
			       unsigned long v, void *p)
{
	int cpu;

	for_each_present_cpu(cpu) {
		struct hyp_arm_smmu_v3_err *err = &kvm_arm_smmu_v3_err[cpu];
		struct host_arm_smmu_device *host_smmu = host_arm_smmu_array[err->smmu_id];
		struct device *dev = NULL;

		if (host_smmu)
			dev = host_smmu->smmu.dev;

		if (err->type == HYP_ARM_SMMU_V3_ERR_CMDQ_TIMEOUT) {
			dev_err(dev, "cmdq time out: hw prod 0x%x hw cons 0x%x sw prod 0x%llx",
				err->cmdq_prod, err->cmdq_cons, err->cmdq_prod_sw);
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block smmu_panic_block = {
	.notifier_call = smmu_panic_err_dump,
};

static int kvm_arm_smmu_v3_init_block_region(void)
{
	struct device_node *np;
	struct resource res;
	u64 start, size;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "pkvm,smmu-v3-block-region");
	if (!np)
		return 0;

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		pr_err("pKVM SMMUv3 invalid block region\n");
		return -EINVAL;
	}

	start = res.start;
	size = resource_size(&res);
	if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(size)) {
		pr_err("pKVM SMMUv3 block region %pr not properly aligned\n", &res);
		return -EINVAL;
	}

	kvm_hyp_smmu_global_config.block_region_start = start;
	kvm_hyp_smmu_global_config.block_region_size = size;

	return 0;
}

/**
 * kvm_arm_smmu_v3_init() - Reserve the SMMUv3 for KVM
 * Return 0 if all present SMMUv3 were probed successfully, or an error.
 *   If no SMMU was found, return 0, with a count of 0.
 */
static int kvm_arm_smmu_v3_init(void)
{
	int ret;
	struct kvm_hyp_memcache atomic_mc = {};

	if (disable) {
		pr_warn("Skip probing pKVM SMMUv3 due to disable param.\n");
		return 0;
	}
	/*
	 * Check whether any device owned by the host is behind an SMMU.
	 */
	ret = kvm_arm_smmu_array_alloc();
	if (ret || !kvm_arm_smmu_count)
		return ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		goto err_unregister;

	if (kvm_arm_smmu_cur != kvm_arm_smmu_count) {
		/* A device exists but failed to probe */
		ret = -EUNATCH;
		goto err_unregister;
	}

#ifdef MODULE
	ret = pkvm_load_el2_module(kvm_nvhe_sym(smmu_init_hyp_module),
				   &pkvm_module_token);

	if (ret) {
		pr_err("Failed to load SMMUv3 IOMMU EL2 module: %d\n", ret);
		goto err_unregister;
	}
#endif
	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 *
	 * kvm_hyp_smmu_last_err is shared between hypervisor and host.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_count;
	kvm_hyp_smmu_last_err = kvm_arm_smmu_v3_err;
	ret = smmu_alloc_atomic_mc(&atomic_mc);
	if (ret)
		goto err_free_mc;

	ret = kvm_arm_smmu_v3_init_block_region();
	if (ret)
		goto err_free_mc;

	ret = kvm_iommu_init_hyp(ksym_ref_addr_nvhe(smmu_ops), &atomic_mc, 0);
	if (ret)
		goto err_free_mc;

	/* Preemptively allocate the identity domain. */
	if (atomic_pages) {
		ret = kvm_call_hyp_nvhe_mc(__pkvm_host_iommu_alloc_domain,
					   KVM_IOMMU_DOMAIN_IDMAP_ID,
					   KVM_IOMMU_DOMAIN_IDMAP_TYPE);
		if (ret) {
			pr_err("pKVM SMMUv3 identity domain failed allocation %d\n", ret);
			return ret;
		}
	}

	WARN_ON(driver_for_each_device(&kvm_arm_smmu_driver.driver, NULL,
				       NULL, smmu_put_device));
	atomic_notifier_chain_register(&panic_notifier_list, &smmu_panic_block);
	return 0;
err_free_mc:
	free_hyp_memcache(&atomic_mc);
err_unregister:
	pr_err("pKVM SMMUv3 init failed with %d\n", ret);
	WARN_ON(driver_for_each_device(&kvm_arm_smmu_driver.driver, NULL,
				       NULL, smmu_unregister_smmu));
	return 0;
}

static void kvm_arm_smmu_v3_remove(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list, &smmu_panic_block);
	platform_driver_unregister(&kvm_arm_smmu_driver);
}

pkvm_handle_t kvm_arm_smmu_v3_id(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	return host_smmu->id;
}

struct kvm_iommu_driver kvm_smmu_v3_ops = {
	.init_driver = kvm_arm_smmu_v3_init,
	.remove_driver = kvm_arm_smmu_v3_remove,
	.get_iommu_id = kvm_arm_smmu_v3_id,
};

static int kvm_arm_smmu_v3_register(void)
{
	if (!is_protected_kvm_enabled())
		return 0;

	return kvm_iommu_register_driver(&kvm_smmu_v3_ops);
}

/*
 * Register must be run before de-privliage before kvm_iommu_init_driver
 * for module case, it should be loaded using pKVM early loading which
 * loads it before this point.
 * For builtin drivers we use core_initcall
 */
#ifdef MODULE
module_init(kvm_arm_smmu_v3_register);
#else
core_initcall(kvm_arm_smmu_v3_register);
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ARM SMMUv3 pKVM support");

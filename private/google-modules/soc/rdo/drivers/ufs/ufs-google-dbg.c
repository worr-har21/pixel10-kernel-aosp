// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC
 */

#include <linux/debugfs.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp_types.h>
#include <linux/pm_runtime.h>

#include <trace/hooks/ufshcd.h>
#include <ufs/ufshcd.h>

#include "ufs-google-dbg.h"
#include "ufs-google.h"

struct ufs_google_dbg {
	struct ufs_google_host *host;
	struct dentry *debugfs;
};

int ufs_google_init_debugfs(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;
	struct dentry *root;

	host = ufshcd_get_variant(hba);
	dbg = host->dbg;

	root = debugfs_create_dir("google", hba->debugfs_root);
	if (IS_ERR_OR_NULL(root))
		return -EPERM;

	dbg->debugfs = root;

	return 0;
}

void ufs_google_remove_debugfs(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;

	host = ufshcd_get_variant(hba);
	dbg = host->dbg;

	debugfs_remove_recursive(dbg->debugfs);
}

int ufs_google_init_dbg(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;

	host = ufshcd_get_variant(hba);

	dbg = devm_kzalloc(hba->dev, sizeof(struct ufs_google_dbg), GFP_KERNEL);
	if (!dbg)
		return -ENOMEM;

	host->dbg = dbg;
	dbg->host = host;

	return 0;
}

void ufs_google_remove_dbg(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct ufs_google_dbg *dbg;

	host = ufshcd_get_variant(hba);
	dbg = host->dbg;

	devm_kfree(hba->dev, dbg);
}

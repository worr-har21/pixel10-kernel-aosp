/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _GOOGLE_ICC_INTERNAL_H
#define _GOOGLE_ICC_INTERNAL_H

#include <linux/interconnect.h>
#include <linux/mutex.h>

#define NUM_VC 5

enum {
	ICC_TYPE_GMC = 0,
	ICC_TYPE_GSLC,
	NUM_ICC_TYPE
};

enum {
	ICC_READ = 0,
	ICC_WRITE,
	NUM_ICC_RW
};

struct icc_vote_block {
	/* avg_bw per VC */
	u64 avg_bw_vc[NUM_VC];
	/* peak_bw per VC */
	u64 peak_bw_vc[NUM_VC];
	/* latency per VC */
	u32 latency_vc[NUM_VC];
	/* latency tolerance vote per VC */
	u32 ltv_vc[NUM_VC];
	/* rt_bw per VC */
	u32 rt_bw_vc[NUM_VC];
	/* Total avg_bw */
	u64 avg_bw;
	/* Total peak_bw */
	u64 peak_bw;
	/* Total latency */
	u32 latency;
	/* Total latency tolerance vote */
	u32 ltv;
	/* rt_bw vote */
	u32 rt_bw;
};

struct icc_vote {
	u32 prop;
	struct icc_vote_block block[NUM_ICC_TYPE][NUM_ICC_RW];
};

struct google_icc_path {
	struct icc_path *path;
	/* protect calling sequences in google_icc_*() */
	struct mutex mutex;
	struct icc_vote vote;
};

#endif /* _GOOGLE_ICC_INTERNAL_H */

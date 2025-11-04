/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 */


#include <linux/devfreq.h>
#include <linux/cpufreq.h>

#if IS_ENABLED(CONFIG_GOOGLE_VOTE_MANAGER)
extern int vote_manager_init_cpufreq(struct cpufreq_policy *policy);
extern int vote_manager_init_devfreq(struct devfreq *devfreq);
extern int vote_manager_remove_cpufreq(struct cpufreq_policy *policy);
extern int vote_manager_remove_devfreq(struct devfreq *devfreq);
#else
static inline int vote_manager_init_cpufreq(struct cpufreq_policy *policy)
{
	return 0;
}

static inline int vote_manager_init_devfreq(struct devfreq *devfreq)
{
	return 0;
}

static inline int vote_manager_remove_cpufreq(struct cpufreq_policy *policy)
{
	return 0;
}

static inline int vote_manager_remove_devfreq(struct devfreq *devfreq)
{
	return 0;
}

#endif // enabled(GOOGLE_VOTE_MANAGER)

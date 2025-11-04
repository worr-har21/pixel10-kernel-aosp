/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cdev_devfreq_helper_mock.h Helper to declare and use devfreq cooling device
 * mock functions.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */

#ifndef _CDEV_DEVFREQ_HELPER_MOCK_H_
#define _CDEV_DEVFREQ_HELPER_MOCK_H_

#include <linux/cpu.h>
#include <linux/devfreq.h>
#include <linux/energy_model.h>

#include "cdev_devfreq_helper.h"
#include "perf/core/google_pm_qos.h"
#include "thermal_msg_helper.h"

#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
struct devfreq *mock_cdev_get_devfreq_by_node(struct device_node *node);
int mock_pm_qos_add_and_register_devfreq_request(struct devfreq *devfreq,
						 struct dev_pm_qos_request *req,
						 enum dev_pm_qos_req_type type,
						 s32 value);
int mock_pm_qos_remove_devfreq_request(struct devfreq *devfreq,
				       struct dev_pm_qos_request *req);
struct em_perf_state *mock_em_perf_state_from_pd(struct em_perf_domain *pd);
int mock_dev_pm_opp_get_opp_count(struct device *dev);
struct dev_pm_opp *mock_dev_pm_opp_find_freq_ceil(struct device *dev,
						  unsigned long *freq);
int mock_msg_tmu_get_power_table(enum hw_dev_type cdev_id, u8 idx,
				 int *val, int *max_state_idx);
struct device *mock_get_cpu_device(unsigned int cpu);
unsigned long mock_dev_pm_opp_get_voltage(struct dev_pm_opp *opp);
#else
static inline struct devfreq *mock_cdev_get_devfreq_by_node(struct device_node *node)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int mock_pm_qos_add_and_register_devfreq_request(
		struct devfreq *devfreq, struct dev_pm_qos_request *req,
		enum dev_pm_qos_req_type type, s32 value)
{
	return -EOPNOTSUPP;
}

static inline int mock_pm_qos_remove_devfreq_request(struct devfreq *devfreq,
						     struct dev_pm_qos_request *req)
{
	return -EOPNOTSUPP;
}

static inline struct em_perf_state *mock_em_perf_state_from_pd(struct em_perf_domain *pd)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int mock_dev_pm_opp_get_opp_count(struct device *dev)
{
	return -EOPNOTSUPP;
}

static inline struct dev_pm_opp *mock_dev_pm_opp_find_freq_ceil(struct device *dev,
								unsigned long *freq)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int mock_msg_tmu_get_power_table(enum hw_dev_type cdev_id, u8 idx,
					       int *val, int *max_state_idx)
{
	return -EOPNOTSUPP;
}

static inline struct device *mock_get_cpu_device(unsigned int cpu)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline unsigned long mock_dev_pm_opp_get_voltage(struct dev_pm_opp *opp)
{
	return -EOPNOTSUPP;
}
#endif // CONFIG_CDEV_HELPER_KUNIT_TEST

static inline struct devfreq *cdev_get_devfreq_by_node(struct device_node *node)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_cdev_get_devfreq_by_node(node);
#else
	return devfreq_get_devfreq_by_node(node);
#endif
}

static inline int cdev_pm_qos_add_devfreq_request(struct devfreq *devfreq,
						 struct dev_pm_qos_request *req,
						 enum dev_pm_qos_req_type type,
						 s32 value)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_pm_qos_add_and_register_devfreq_request(devfreq, req, type, value);
#else
	return google_pm_qos_add_devfreq_request(devfreq, req, type, value);
#endif
}


static inline struct em_perf_state *cdev_em_perf_state_from_pd(struct em_perf_domain *pd)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_em_perf_state_from_pd(pd);
#else
	return em_perf_state_from_pd(pd);
#endif
}

static inline int cdev_pm_qos_remove_devfreq_request(struct devfreq *devfreq,
						     struct dev_pm_qos_request *req)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_pm_qos_remove_devfreq_request(devfreq, req);
#else
	return google_pm_qos_remove_devfreq_request(devfreq, req);
#endif
}

static inline int cdev_dev_pm_qos_update_request(struct dev_pm_qos_request *req, s32 new_value)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return 0;
#else
	return dev_pm_qos_update_request(req, new_value);
#endif
}

static inline int cdev_freq_qos_update_request(struct freq_qos_request *req, s32 new_value)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return 0;
#else
	return freq_qos_update_request(req, new_value);
#endif
}

static inline int cdev_dev_pm_opp_get_opp_count(struct device *dev)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_dev_pm_opp_get_opp_count(dev);
#else
	return dev_pm_opp_get_opp_count(dev);
#endif
}

static inline struct dev_pm_opp *cdev_dev_pm_opp_find_freq_ceil(struct device *dev,
							       unsigned long *freq)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_dev_pm_opp_find_freq_ceil(dev, freq);
#else
	return dev_pm_opp_find_freq_ceil(dev, freq);
#endif
}

static inline int cdev_msg_tmu_get_power_table(enum hw_dev_type cdev_id, u8 idx,
					      int *val, int *max_state_idx)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_msg_tmu_get_power_table(cdev_id, idx, val, max_state_idx);
#else
	return msg_tmu_get_power_table(cdev_id, idx, val, max_state_idx);
#endif
}

static inline void cdev_dev_pm_opp_put(struct dev_pm_opp *opp)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return;
#else
	return dev_pm_opp_put(opp);
#endif
}

static inline struct device *cdev_get_cpu_device(unsigned int cpu)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_get_cpu_device(cpu);
#else
	return get_cpu_device(cpu);
#endif
}

static inline unsigned long cdev_dev_pm_opp_get_voltage(struct dev_pm_opp *opp)
{
#if IS_ENABLED(CONFIG_CDEV_HELPER_KUNIT_TEST)
	return mock_dev_pm_opp_get_voltage(opp);
#else
	return dev_pm_opp_get_voltage(opp);
#endif
}
#endif  // _CDEV_DEVFREQ_HELPER_MOCK_H_

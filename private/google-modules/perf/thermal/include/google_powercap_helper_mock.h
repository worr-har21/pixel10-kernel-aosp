/* SPDX-License-Identifier: GPL-2.0 */
/*
 * google_powercap_helper_mock.h Helper to declare and use powercap mock
 * functions.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */

#ifndef _GOOGLE_POWERCAP_HELPER_MOCK_H_
#define _GOOGLE_POWERCAP_HELPER_MOCK_H_

#include <linux/powercap.h>

#include "cdev_cpufreq_helper.h"
#include "cdev_devfreq_helper.h"
#include "cpufreq/thermal_pressure.h"
#include "google_powercap_helper.h"
#include "google_powercap_cpu.h"
#include "perf/core/google_pm_qos.h"
#include "thermal_msg_helper.h"

extern struct list_head gpowercap_cpu_list;

#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
const struct of_device_id *mock_match_of_node(const struct of_device_id *matches,
					 const struct device_node *node);
struct powercap_control_type *mock_powercap_register_control_type(
		struct powercap_control_type *control_type, const char *name,
		const struct powercap_control_type_ops *ops);
int mock_powercap_unregister_control_type(struct powercap_control_type *control_type);
struct gpowercap *gpc_test_setup(const struct gpowercap_node *hierarchy,
				 struct gpowercap *parent);
int gpc_test_setup_dt(struct gpowercap *gpowercap, struct device_node *np,
		      enum hw_dev_type cdev_id);
void gpc_test_exit(void);
struct powercap_zone *mock_powercap_register_zone(
			struct powercap_zone *power_zone,
			struct powercap_control_type *control_type,
			const char *name,
			struct powercap_zone *parent,
			const struct powercap_zone_ops *ops,
			int nr_constraints,
			const struct powercap_zone_constraint_ops *const_ops);
int mock_cdev_devfreq_init(struct cdev_devfreq_data *cdev,
			   struct device_node *np,
			   enum hw_dev_type cdev_id,
			   cdev_cb success_cb,
			   cdev_cb release_cb);
struct cpufreq_policy *mock_cpufreq_get_policy(unsigned int cpu);
int mock_gpowercap_register(const char *name, struct gpowercap *gpowercap,
			    struct gpowercap *parent);
int mock_apply_thermal_pressure(const cpumask_t cpus, const unsigned long frequency,
				enum thermal_pressure_type type);
int mock_pm_qos_add_cpufreq_request(struct cpufreq_policy *policy,
				   struct freq_qos_request *req,
				   enum freq_qos_req_type type,
				   s32 value);
int mock_pm_qos_remove_cpufreq_request(struct cpufreq_policy *policy,
				       struct freq_qos_request *req);
unsigned int mock_cpufreq_quick_get(unsigned int cpu);
int mock_cdev_cpufreq_get_opp_count(unsigned int cpu);
int mock_cdev_cpufreq_update_opp_table(unsigned int cpu,
				  enum hw_dev_type cdev_id,
				  struct cdev_opp_table *cdev_table,
				  unsigned int num_opp);
#else
static inline const struct of_device_id *mock_match_of_node(const struct of_device_id *matches,
					 const struct device_node *node)
{
	return NULL;
}

static inline struct powercap_control_type *mock_powercap_register_control_type(
		struct powercap_control_type *control_type, const char *name,
		const struct powercap_control_type_ops *ops)
{
	return ERR_PTR(-ENOSYS);
}

static inline int mock_powercap_unregister_control_type(struct powercap_control_type *control_type)
{
	return -ENOSYS;
}

static inline struct gpowercap *gpc_test_setup(const struct gpowercap_node *hierarchy,
					       struct gpowercap *parent)
{
	return ERR_PTR(-ENOSYS);
}

static inline int gpowercap_devfreq_setup(struct gpowercap *gpowercap, struct device_node *np)
{
	return -ENOSYS;
}

static inline int gpc_test_setup_dt(struct gpowercap *gpowercap, struct device_node *np,
				    enum hw_dev_type cdev_id)
{
	return -ENOSYS;
}

static inline void gpc_test_exit(void)
{}

static inline struct powercap_zone *mock_powercap_register_zone(
			struct powercap_zone *power_zone,
			struct powercap_control_type *control_type,
			const char *name,
			struct powercap_zone *parent,
			const struct powercap_zone_ops *ops,
			int nr_constraints,
			const struct powercap_zone_constraint_ops *const_ops)
{
	return ERR_PTR(-ENOSYS);
}

static inline int mock_cdev_devfreq_init(struct cdev_devfreq_data *cdev,
					 struct device_node *np,
					 enum hw_dev_type cdev_id,
					 cdev_cb success_cb,
					 cdev_cb release_cb)
{
	return -EOPNOTSUPP;
}

static inline struct cpufreq_policy *mock_cpufreq_get_policy(unsigned int cpu)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int mock_gpowercap_register(const char *name, struct gpowercap *gpowercap,
					 struct gpowercap *parent)
{
	return -EOPNOTSUPP;
}

static inline int mock_apply_thermal_pressure(const cpumask_t cpus,
					      const unsigned long frequency,
					      enum thermal_pressure_type type)
{
	return -EOPNOTSUPP;
}

static inline int mock_pm_qos_add_cpufreq_request(struct cpufreq_policy *policy,
				   struct freq_qos_request *req,
				   enum freq_qos_req_type type,
				   s32 value)
{
	return -EOPNOTSUPP;
}

static inline int mock_pm_qos_remove_cpufreq_request(struct cpufreq_policy *policy,
				       struct freq_qos_request *req)
{
	return -EOPNOTSUPP;
}

static inline unsigned int mock_cpufreq_quick_get(unsigned int cpu)
{
	return 0;
}

static inline int mock_cdev_cpufreq_get_opp_count(unsigned int cpu)
{
	return -EOPNOTSUPP;
}

static inline int mock_cdev_cpufreq_update_opp_table(unsigned int cpu,
				  enum hw_dev_type cdev_id,
				  struct cdev_opp_table *cdev_table,
				  unsigned int num_opp)
{
	return -EOPNOTSUPP;
}
#endif // CONFIG_GOOGLE_POWERCAP_KUNIT_TEST

static inline const struct of_device_id *match_of_node(const struct of_device_id *matches,
						       const struct device_node *node)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_match_of_node(matches, node);
#else
	return of_match_node(matches, node);
#endif
}

static inline struct powercap_control_type *register_control_type(
		struct powercap_control_type *control_type, const char *name,
		const struct powercap_control_type_ops *ops)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_powercap_register_control_type(control_type, name, ops);
#else
	return powercap_register_control_type(control_type, name, ops);
#endif
}

static inline int unregister_control_type(struct powercap_control_type *control_type)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_powercap_unregister_control_type(control_type);
#else
	return powercap_unregister_control_type(control_type);
#endif
}

static inline struct powercap_zone *register_zone(
			struct powercap_zone *power_zone,
			struct powercap_control_type *control_type,
			const char *name,
			struct powercap_zone *parent,
			const struct powercap_zone_ops *ops,
			int nr_constraints,
			const struct powercap_zone_constraint_ops *const_ops)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_powercap_register_zone(power_zone, control_type, name, parent, ops,
					   nr_constraints, const_ops);
#else
	return powercap_register_zone(power_zone, control_type, name, parent, ops, nr_constraints,
				      const_ops);
#endif
}

static inline int gpc_cdev_devfreq_init(struct cdev_devfreq_data *cdev,
					struct device_node *np,
					enum hw_dev_type cdev_id,
					cdev_cb success_cb,
					cdev_cb release_cb)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_cdev_devfreq_init(cdev, np, cdev_id, success_cb, release_cb);
#else
	return cdev_devfreq_init(cdev, np, cdev_id, success_cb, release_cb);
#endif
}

static inline int dev_create_file(struct device *device,
					  const struct device_attribute *entry)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return 0;
#else
	return device_create_file(device, entry);
#endif
}

static inline int unregister_zone(struct powercap_control_type *control_type,
					   struct powercap_zone *power_zone)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return 0;
#else
	return powercap_unregister_zone(control_type, power_zone);
#endif
}

static inline int gpc_freq_qos_update_request(struct freq_qos_request *req, s32 new_value)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return 0;
#else
	return freq_qos_update_request(req, new_value);
#endif
}

/*
 * This function should be called for kernel version < 6.12.
 */
static inline int gpc_apply_thermal_pressure(const cpumask_t cpus,
					     const unsigned long frequency,
					     enum thermal_pressure_type type)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_apply_thermal_pressure(cpus, frequency, type);
#else
	return apply_thermal_pressure(cpus, frequency, type);
#endif
}

static inline struct cpufreq_policy *gpc_cpufreq_get_policy(unsigned int cpu)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_cpufreq_get_policy(cpu);
#else
	return cpufreq_cpu_get(cpu);
#endif
}

static inline int gpc_gpowercap_register(const char *name, struct gpowercap *gpowercap,
					 struct gpowercap *parent)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_gpowercap_register(name, gpowercap, parent);
#else
	return gpowercap_register(name, gpowercap, parent);
#endif
}

static inline void gpc_cpufreq_cpu_put(struct cpufreq_policy *policy)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return;
#else
	return cpufreq_cpu_put(policy);
#endif
}

static inline int gpc_pm_qos_add_cpufreq_request(struct cpufreq_policy *policy,
				   struct freq_qos_request *req,
				   enum freq_qos_req_type type,
				   s32 value)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_pm_qos_add_cpufreq_request(policy, req, type, value);
#else
	return google_pm_qos_add_cpufreq_request(policy, req, type, value);
#endif
}

static inline int gpc_pm_qos_remove_cpufreq_request(struct cpufreq_policy *policy,
						    struct freq_qos_request *req)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_pm_qos_remove_cpufreq_request(policy, req);
#else
	return google_pm_qos_remove_cpufreq_request(policy, req);
#endif
}

static inline unsigned int gpc_cpufreq_quick_get(unsigned int cpu)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_cpufreq_quick_get(cpu);
#else
	return cpufreq_quick_get(cpu);
#endif
}

static inline void gpc_fatal_error(void)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return;
#else
	WARN_ON_ONCE(1);
#endif
}

static inline int gpc_cdev_cpufreq_get_opp_count(unsigned int cpu)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_cdev_cpufreq_get_opp_count(cpu);
#else
	return cdev_cpufreq_get_opp_count(cpu);
#endif
}

static inline int gpc_cdev_cpufreq_update_opp_table(unsigned int cpu,
				  enum hw_dev_type cdev_id,
				  struct cdev_opp_table *cdev_table,
				  unsigned int num_opp)
{
#if IS_ENABLED(CONFIG_GOOGLE_POWERCAP_KUNIT_TEST)
	return mock_cdev_cpufreq_update_opp_table(cpu, cdev_id, cdev_table, num_opp);
#else
	return cdev_cpufreq_update_opp_table(cpu, cdev_id, cdev_table, num_opp);
#endif
}
#endif  // _GOOGLE_POWERCAP_HELPER_MOCK_H_

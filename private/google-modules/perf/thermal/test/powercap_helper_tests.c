// SPDX-License-Identifier: GPL-2.0
/*
 * powercap_helper_tests.c Test suite to test all the powercap helper functions.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */

#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/powercap.h>
#include <linux/version.h>
#include <kunit/test.h>
#include <kunit/test-bug.h>

#include "google_powercap_devfreq.h"
#include "google_powercap_helper.h"
#include "google_powercap_helper_mock.h"

#define GPC_TEST_OPP_CT		3
#define GPC_FREQ_INIT		300000
#define GPC_FREQ_INCREMENT	500000
#define GPC_POWER_INIT		500000
#define GPC_POWER_INCREMENT	1000000
#define GPC_VOLT_INIT		100
#define GPC_VOLT_INCREMENT	200
#define GPC_TREE_NODE_CT	4
#define GPC_MOCK_POWER_UW	1000005
#define GPC_TEST_CPU		0
#define GPC_TEST_CPU_MAX	4

struct powercap_test_data {
	struct gpowercap gpc;
};

static struct cdev_opp_table *opp_table;
static struct powercap_test_data *test_data;
static u64 power_limit_uw;
static int update_power_ret, reg_ct_ret, gpc_setup_ct, gpc_setup_ret, match_node_ret;
static int virt_setup_ret, setup_ret, gpc_setup_dt_ct, reg_zone_ret;
static int qos_add_ret,
	   gpc_register_ret,
	   cpu_policy_ret,
	   cpu_get_opp_ct_ret,
	   cpu_get_opp_ret;
static unsigned int gpc_cpufreq;
static enum thermal_pressure_type apply_tp_type;
static cpumask_t apply_tp_cpus;
static bool gpc_release_called, pc_unreg_called, gpc_exit_called, remove_qos_called;
static bool gpc_register_called;
static struct powercap_control_type *pct_test;
static struct powercap_zone pc_zone_test;
static struct gpowercap *virt_node, *leaf_node;
static struct cpufreq_policy *test_policy;

static struct gpowercap_node gpc_test_tree[GPC_TREE_NODE_CT] = {
	[0] { .name = "soc",
		.type =  GPOWERCAP_NODE_TEST_VIRTUAL },
	[1] { .name = "cpu_cluster",
		.type = GPOWERCAP_NODE_TEST_VIRTUAL,
		.parent = &gpc_test_tree[0] },
	[2] { .name = "/cpus/cpu@0",
		.type = GPOWERCAP_NODE_TEST_DT,
		.parent = &gpc_test_tree[1],
		.cdev_id = HW_CDEV_LIT, },
	[3] { },
};
static struct of_device_id powercap_test_match_data[] = {
	{
		.compatible = "google,powercap-test",
		.data = gpc_test_tree,
	},
	{}
};

static u64 gpc_test_set_power_uw(struct gpowercap *gpc, u64 power_uw)
{
	power_limit_uw = power_uw;

	return power_limit_uw;
}

static u64 gpc_test_get_power_uw(struct gpowercap *gpc)
{
	return GPC_MOCK_POWER_UW;
}

static int gpc_test_update_power_uw(struct gpowercap *gpc)
{
	gpc->power_min = opp_table[0].power;
	gpc->power_max = opp_table[GPC_TEST_OPP_CT - 1].power;
	gpc->num_opps = GPC_TEST_OPP_CT;
	gpc->opp_table = opp_table;

	return update_power_ret;
}

static void gpc_test_release(struct gpowercap *gpc)
{
	gpc_release_called = true;
}

static struct gpowercap_ops test_ops = {
	.set_power_uw = gpc_test_set_power_uw,
	.get_power_uw = gpc_test_get_power_uw,
	.update_power_uw = gpc_test_update_power_uw,
	.release = gpc_test_release,
};

const struct of_device_id *mock_match_of_node(const struct of_device_id *matches,
					 const struct device_node *node)
{
	return (match_node_ret) ? NULL : powercap_test_match_data;
}

static struct gpowercap *gpc_alloc_and_create_node(const char *name, struct gpowercap *parent,
						   struct gpowercap_ops *ops)
{
	struct gpowercap *gpowercap;
	struct kunit *test = kunit_get_current_test();
	int ret = 0;

	gpowercap = kunit_kzalloc(test, sizeof(*gpowercap), GFP_KERNEL);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, gpowercap);
	gpowercap_init(gpowercap, ops);
	ret = __gpowercap_register(name, gpowercap, parent);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret) {
		pr_err("powercap node:%s reg failed. err:%d\n", name, ret);
		return ERR_PTR(ret);
	}

	return gpowercap;
}

struct gpowercap *gpc_test_setup(const struct gpowercap_node *hierarchy,
				 struct gpowercap *parent)
{
	gpc_setup_ct++;
	if (virt_setup_ret)
		return ERR_PTR(virt_setup_ret);

	virt_node = gpc_alloc_and_create_node(hierarchy->name, parent, NULL);
	return virt_node;
}

int gpc_test_setup_dt(struct gpowercap *parent, struct device_node *np, enum hw_dev_type cdev_id)
{
	gpc_setup_dt_ct++;
	if (setup_ret)
		return setup_ret;

	leaf_node = gpc_alloc_and_create_node("leaf_node", parent, &test_ops);
	return 0;
}

void gpc_test_exit(void)
{
	gpc_exit_called = true;
}

struct powercap_control_type *mock_powercap_register_control_type(
		struct powercap_control_type *control_type, const char *name,
		const struct powercap_control_type_ops *ops)
{
	return (reg_ct_ret) ? ERR_PTR(reg_ct_ret) : pct_test;

}

struct powercap_zone *mock_powercap_register_zone(
			struct powercap_zone *power_zone,
			struct powercap_control_type *control_type,
			const char *name,
			struct powercap_zone *parent,
			const struct powercap_zone_ops *ops,
			int nr_constraints,
			const struct powercap_zone_constraint_ops *const_ops)
{
	return (reg_zone_ret) ? ERR_PTR(reg_zone_ret) : &pc_zone_test;
}

int mock_powercap_unregister_control_type(struct powercap_control_type *control_type)
{
	pc_unreg_called = true;
	return 0;
}

struct cpufreq_policy *mock_cpufreq_get_policy(unsigned int cpu)
{
	return cpu_policy_ret ? NULL : test_policy;
}

int mock_gpowercap_register(const char *name, struct gpowercap *gpowercap,
			    struct gpowercap *parent)
{
	gpc_register_called = true;
	return gpc_register_ret;
}

int mock_apply_thermal_pressure(const cpumask_t cpus, const unsigned long frequency,
				enum thermal_pressure_type type)
{
	cpumask_copy(&apply_tp_cpus, &cpus);
	apply_tp_type = type;
	return 0;
}

int mock_pm_qos_add_cpufreq_request(struct cpufreq_policy *policy,
				   struct freq_qos_request *req,
				   enum freq_qos_req_type type,
				   s32 value)
{
	return qos_add_ret;
}

int mock_pm_qos_remove_cpufreq_request(struct cpufreq_policy *policy,
				       struct freq_qos_request *req)
{
	remove_qos_called = true;
	return 0;
}

unsigned int mock_cpufreq_quick_get(unsigned int cpu)
{
	return gpc_cpufreq;
}

int mock_cdev_cpufreq_get_opp_count(unsigned int cpu)
{
	return cpu_get_opp_ct_ret;
}

int mock_cdev_cpufreq_update_opp_table(unsigned int cpu,
				  enum hw_dev_type cdev_id,
				  struct cdev_opp_table *cdev_table,
				  unsigned int num_opp)
{
	int i = 0;

	if (cpu_get_opp_ret)
		return cpu_get_opp_ret;

	for (i = 0; i < GPC_TEST_OPP_CT; i++) {
		cdev_table[i].power = opp_table[i].power;
		cdev_table[i].freq = opp_table[i].freq;
		cdev_table[i].voltage = opp_table[i].voltage;
	}
	return 0;
}
static void gpc_test_init_data(struct powercap_test_data *data)
{
	cpu_get_opp_ct_ret = GPC_TEST_OPP_CT;
	cpu_get_opp_ret = 0;
	gpc_cpufreq = 0;
	gpc_register_ret = 0;
	cpu_policy_ret = 0;
	gpc_register_called = false;
	remove_qos_called = false;
	qos_add_ret = 0;
	virt_node = leaf_node = NULL;
	power_limit_uw = 0;
	reg_zone_ret = 0;
	update_power_ret = 0;
	reg_ct_ret = 0;
	gpc_setup_ct = 0;
	gpc_setup_dt_ct = 0;
	gpc_setup_ret = 0;
	match_node_ret = 0;
	virt_setup_ret = 0;
	setup_ret = 0;
	gpc_exit_called = false;
	gpc_release_called = false;
	apply_tp_type = THERMAL_PRESSURE_TYPE_MAX;
	cpumask_clear(&apply_tp_cpus);
	__gpowercap_destroy_hierarchy();
	pc_unreg_called = false;
}

static void powercap_init_test(struct kunit *test)
{
	struct powercap_test_data *data = test->priv;

	data->gpc.ops = NULL;
	// NULL pointer as input. Nothing to check. but make sure no abnormal behavior.
	gpowercap_init(NULL, NULL);
	gpowercap_init(NULL, &test_ops);

	gpowercap_init(&data->gpc, NULL);
	KUNIT_EXPECT_NULL(test, data->gpc.ops);

	gpowercap_init(&data->gpc, &test_ops);
	KUNIT_EXPECT_PTR_EQ(test, data->gpc.ops, &test_ops);
	KUNIT_EXPECT_PTR_EQ(test, data->gpc.children.next, &data->gpc.children);
	KUNIT_EXPECT_PTR_EQ(test, data->gpc.children.prev, &data->gpc.children);
	KUNIT_EXPECT_PTR_EQ(test, data->gpc.siblings.next, &data->gpc.siblings);
	KUNIT_EXPECT_PTR_EQ(test, data->gpc.siblings.prev, &data->gpc.siblings);
	KUNIT_EXPECT_NOT_NULL(test, data->gpc.bypass_work.work.func);
}

static void powercap_create_hierarchy_test(struct kunit *test)
{
	struct powercap_test_data *data = test->priv;

	// power cap control reg error.
	reg_ct_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data), reg_ct_ret);
	KUNIT_EXPECT_FALSE(test, pc_unreg_called);

	// match node error.
	gpc_test_init_data(data);
	match_node_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data),
			-ENODEV);
	KUNIT_EXPECT_TRUE(test, pc_unreg_called);

	// No device specific data.
	gpc_test_init_data(data);
	powercap_test_match_data[0].data = NULL;
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data),
			-EFAULT);
	KUNIT_EXPECT_TRUE(test, pc_unreg_called);
	powercap_test_match_data[0].data = gpc_test_tree;

	// virt node setup error.
	gpc_test_init_data(data);
	virt_setup_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data),
			0);
	// Only the root node setup might have been called.
	KUNIT_EXPECT_EQ(test, gpc_setup_ct, 1);
	KUNIT_EXPECT_FALSE(test, pc_unreg_called);

	// Invalid cdev ID in the input tree.
	gpc_test_init_data(data);
	gpc_test_tree[2].cdev_id = HW_CDEV_MAX;
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data),
			0);
	KUNIT_EXPECT_EQ(test, gpc_setup_dt_ct, 0);
	KUNIT_EXPECT_EQ(test, gpc_setup_ct, GPC_TREE_NODE_CT - 2);
	gpc_test_tree[2].cdev_id = HW_CDEV_LIT;

	// Success case
	gpc_test_init_data(data);
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data), 0);
	KUNIT_EXPECT_EQ(test, gpc_setup_ct + gpc_setup_dt_ct, GPC_TREE_NODE_CT - 1);
	KUNIT_EXPECT_FALSE(test, pc_unreg_called);

	// Multiple registration.
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data), -EBUSY);

	// Device node setup error.
	gpc_test_init_data(data);
	setup_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data),
			0);
	KUNIT_EXPECT_EQ(test, gpc_setup_ct + gpc_setup_dt_ct, GPC_TREE_NODE_CT - 1);
	KUNIT_EXPECT_FALSE(test, pc_unreg_called);
	gpc_test_init_data(data);
}

static void powercap_destroy_hierarchy_test(struct kunit *test)
{
	// Call with empty tree
	__gpowercap_destroy_hierarchy();
	KUNIT_EXPECT_FALSE(test, pc_unreg_called);
	KUNIT_EXPECT_FALSE(test, gpc_exit_called);

	// Success case.
	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data), 0);
	__gpowercap_destroy_hierarchy();
	KUNIT_EXPECT_TRUE(test, gpc_exit_called);
}

static void powercap_setup_virtual_test(struct kunit *test)
{
	struct gpowercap *ret;
	struct gpowercap_node new_node = {
		.name = "test",
		.type =  GPOWERCAP_NODE_TEST_VIRTUAL
	};

	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data), 0);
	ret = __gpowercap_setup_virtual(&new_node, virt_node);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, ret);
	gpowercap_unregister(ret);

	reg_zone_ret = -ENODEV;
	ret = __gpowercap_setup_virtual(&new_node, NULL);
	KUNIT_EXPECT_TRUE(test, IS_ERR(ret));
	__gpowercap_destroy_hierarchy();
}

static void powercap_gpc_register_and_ops_test(struct kunit *test)
{
	struct powercap_test_data *data = test->priv;
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	u64 power_limit;

	gpowercap_init(&data->gpc, NULL);
	//No control type.
	KUNIT_EXPECT_EQ(test, __gpowercap_register("test", &data->gpc, NULL), -EAGAIN);

	KUNIT_EXPECT_EQ(test, __gpowercap_create_hierarchy(powercap_test_match_data), 0);
	// Root already exists.
	KUNIT_EXPECT_EQ(test, __gpowercap_register("test", &data->gpc, NULL), -EBUSY);

	// parent has ops
	gpowercap_init(&data->gpc, &test_ops);
	KUNIT_EXPECT_EQ(test, __gpowercap_register("test", virt_node, &data->gpc), -EINVAL);
	// NULL gpc node.
	KUNIT_EXPECT_EQ(test, __gpowercap_register("test", NULL, virt_node), -EINVAL);
	test_ops.release = NULL;
	//Invalid ops
	KUNIT_EXPECT_EQ(test, __gpowercap_register("test", &data->gpc, virt_node), -EINVAL);
	test_ops.release = gpc_test_release;

	reg_zone_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, __gpowercap_register("test", &data->gpc, virt_node), reg_zone_ret);

	// Ops test.
	// 24 caharaters == "2500000 1500000 500000 \n".
	KUNIT_EXPECT_EQ(test, __power_levels_uw_show(leaf_node, buf), 24);
	//kunit_info(test, "%s\n", buf);
	leaf_node->num_opps = 0;
	KUNIT_EXPECT_EQ(test, __power_levels_uw_show(leaf_node, buf), -EAGAIN);
	leaf_node->num_opps = GPC_TEST_OPP_CT;
	// virt node should return -EAGAIN.
	// We will not be registering this sysfs for virt node.
	// But worth checking the case.
	KUNIT_EXPECT_EQ(test, __power_levels_uw_show(virt_node, buf), -EAGAIN);
	kunit_kfree(test, buf);

	// set power limit with a very high value.
	KUNIT_EXPECT_EQ(test, __set_power_limit_uw(leaf_node, INT_MAX), 0);
	KUNIT_EXPECT_EQ(test, power_limit_uw, opp_table[GPC_TEST_OPP_CT - 1].power);
	KUNIT_EXPECT_EQ(test, leaf_node->power_limit, opp_table[GPC_TEST_OPP_CT - 1].power);

	// Test power_limit bypass flag skips the power limit CB.
	// temporarily votes for max power_limit.
	// And then votes back the changed power_limit after the timer expires.
	KUNIT_EXPECT_EQ(test, __set_power_limit_uw(leaf_node,
						   opp_table[1].power), 0);
	KUNIT_EXPECT_EQ(test, __power_limit_bypass(leaf_node, 200), 200);
	KUNIT_EXPECT_EQ(test, power_limit_uw, opp_table[GPC_TEST_OPP_CT - 1].power);
	// change the power limit and check if the power_limit CB is not called.
	KUNIT_EXPECT_EQ(test, __set_power_limit_uw(leaf_node,
						   opp_table[0].power), 0);
	KUNIT_EXPECT_EQ(test, power_limit_uw, opp_table[GPC_TEST_OPP_CT - 1].power);
	// Wait for the bypass time to expire.
	msleep(300);
	KUNIT_EXPECT_FALSE(test, test_bit(GPOWERCAP_POWER_LIMIT_BYPASS_FLAG,
					  &leaf_node->flags));
	KUNIT_EXPECT_EQ(test, power_limit_uw, opp_table[0].power);

	//get power.
	KUNIT_EXPECT_EQ(test, __get_power_uw(leaf_node, &power_limit), 0);
	KUNIT_EXPECT_EQ(test, power_limit, GPC_MOCK_POWER_UW);
	power_limit = 0;
	KUNIT_EXPECT_EQ(test, __get_power_uw(virt_node, &power_limit), 0);
	KUNIT_EXPECT_EQ(test, power_limit, GPC_MOCK_POWER_UW);

	//Test sub/add power.
	__gpowercap_sub_power(leaf_node);
	KUNIT_EXPECT_EQ(test, virt_node->power_min, 0);
	KUNIT_EXPECT_EQ(test, virt_node->power_max, 0);
	__gpowercap_add_power(leaf_node);
	KUNIT_EXPECT_EQ(test, virt_node->power_min, opp_table[0].power);
	KUNIT_EXPECT_EQ(test, virt_node->power_max, opp_table[GPC_TEST_OPP_CT - 1].power);

	//Release zone test.
	KUNIT_EXPECT_EQ(test, __gpowercap_release_zone(&virt_node->zone), -EBUSY);
	KUNIT_EXPECT_EQ(test, __gpowercap_release_zone(&leaf_node->zone), 0);
	// Make sure the top nodes power value are updated.
	KUNIT_EXPECT_EQ(test, virt_node->power_min, 0);
	KUNIT_EXPECT_EQ(test, virt_node->power_max, 0);
	KUNIT_EXPECT_TRUE(test, gpc_release_called);
	__gpowercap_destroy_hierarchy();
}

static void powercap_power_limit_bypass(struct kunit *test)
{
	struct powercap_test_data *data = test->priv;

	gpowercap_init(&data->gpc, &test_ops);

	// Out of range time limit.
	KUNIT_EXPECT_EQ(test, __power_limit_bypass(&data->gpc, -1), 0);
	msleep(100);
	KUNIT_EXPECT_FALSE(test, test_bit(GPOWERCAP_POWER_LIMIT_BYPASS_FLAG,
					  &data->gpc.flags));
	data->gpc.flags = 0;
	KUNIT_EXPECT_EQ(test,
			__power_limit_bypass(&data->gpc,
					     GPOWERCAP_POWER_LIMIT_BYPASS_TIME_MSEC_MAX + 2),
			GPOWERCAP_POWER_LIMIT_BYPASS_TIME_MSEC_MAX);
	KUNIT_EXPECT_TRUE(test, test_bit(GPOWERCAP_POWER_LIMIT_BYPASS_FLAG, &data->gpc.flags));
	data->gpc.flags = 0;
}

static void powercap_devfreq_setup_test(struct kunit *test)
{
	struct gpowercap_devfreq *gpc_devfreq = kunit_kzalloc(test,
							      sizeof(*gpc_devfreq),
							      GFP_KERNEL);

	KUNIT_EXPECT_EQ(test, __gpc_devfreq_setup(&gpc_devfreq->gpowercap, NULL,
						  HW_CDEV_MAX),
			-EINVAL);
}

static struct gpowercap_devfreq *__create_and_init_devfreq(struct kunit *test)
{
	struct gpowercap_devfreq *gpc_devfreq;
	int i = 0;

	// Inactive qos request
	gpc_devfreq = kzalloc(sizeof(*gpc_devfreq), GFP_KERNEL);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, gpc_devfreq);
	gpc_devfreq->cdev.devfreq = kunit_kzalloc(test, sizeof(*gpc_devfreq->cdev.devfreq),
						  GFP_KERNEL);
	gpc_devfreq->cdev.num_opps = GPC_TEST_OPP_CT;
	gpc_devfreq->cdev.opp_table = kcalloc(GPC_TEST_OPP_CT,
					      sizeof(*gpc_devfreq->cdev.opp_table),
					      GFP_KERNEL);
	for (i = 0; i < GPC_TEST_OPP_CT; i++) {
		gpc_devfreq->cdev.opp_table[i].power = opp_table[i].power;
		gpc_devfreq->cdev.opp_table[i].freq = opp_table[i].freq;
	}

	return gpc_devfreq;
}

static void powercap_devfreq_update_power_test(struct kunit *test)
{
	struct gpowercap_devfreq *gpc_devfreq;
	struct gpowercap *gpc;
	int i = 0;
	struct devfreq *devfreq;

	// success
	gpc_devfreq = __create_and_init_devfreq(test);
	gpc = &gpc_devfreq->gpowercap;
	KUNIT_EXPECT_EQ(test, gpc_devfreq_update_pd_power_uw(gpc), 0);
	KUNIT_EXPECT_EQ(test, gpc->num_opps, GPC_TEST_OPP_CT);
	for (i = 0; i < GPC_TEST_OPP_CT; i++) {
		KUNIT_EXPECT_EQ(test, gpc->opp_table[i].power,
				opp_table[i].power);
		KUNIT_EXPECT_EQ(test, gpc->opp_table[i].freq,
				opp_table[i].freq);
	}
	KUNIT_EXPECT_EQ(test, gpc->power_limit, opp_table[GPC_TEST_OPP_CT - 1].power);
	KUNIT_EXPECT_EQ(test, gpc->power_max, opp_table[GPC_TEST_OPP_CT - 1].power);
	KUNIT_EXPECT_EQ(test, gpc->power_min, opp_table[0].power);

	// No devfreq
	devfreq = gpc_devfreq->cdev.devfreq;
	gpc_devfreq->cdev.devfreq = NULL;
	KUNIT_EXPECT_EQ(test, gpc_devfreq_update_pd_power_uw(gpc), -ENODEV);
	gpc_devfreq_pd_release(&gpc_devfreq->gpowercap);
}

static void powercap_devfreq_get_power_test(struct kunit *test)
{
	struct gpowercap_devfreq *gpc_devfreq;
	struct gpowercap *gpc;
	int i = 0;

	// success
	gpc_devfreq = __create_and_init_devfreq(test);
	gpc = &gpc_devfreq->gpowercap;
	KUNIT_EXPECT_EQ(test, gpc_devfreq_update_pd_power_uw(gpc), 0);
	for (i = 0; i < GPC_TEST_OPP_CT; i++) {
		gpc_devfreq->cdev.devfreq->last_status.current_frequency =
				opp_table[i].freq * HZ_PER_KHZ;
		KUNIT_EXPECT_EQ(test, gpc_devfreq_get_pd_power_uw(gpc),
				opp_table[i].power);
	}

	// When devfreq is not available.
	gpc_devfreq->cdev.devfreq = NULL;
	KUNIT_EXPECT_EQ(test, gpc_devfreq_get_pd_power_uw(gpc), 0);
	gpc_devfreq_pd_release(&gpc_devfreq->gpowercap);
}

static void powercap_devfreq_set_power_test(struct kunit *test)
{
	struct gpowercap_devfreq *gpc_devfreq;
	struct gpowercap *gpc;
	int i = 0;
	struct devfreq *devfreq;

	// success
	gpc_devfreq = __create_and_init_devfreq(test);
	gpc = &gpc_devfreq->gpowercap;
	KUNIT_EXPECT_EQ(test, gpc_devfreq_update_pd_power_uw(gpc), 0);
	for (i = 0; i < GPC_TEST_OPP_CT; i++) {
		KUNIT_EXPECT_EQ(test, gpc_devfreq_set_pd_power_limit(gpc, opp_table[i].power),
				opp_table[i].power);
		KUNIT_EXPECT_EQ(test, gpc_devfreq_set_pd_power_limit(gpc, opp_table[i].power + 1),
				opp_table[i].power);
	}

	// When devfreq is not available.
	devfreq = gpc_devfreq->cdev.devfreq;
	gpc_devfreq->cdev.devfreq = NULL;
	KUNIT_EXPECT_EQ(test, gpc_devfreq_set_pd_power_limit(gpc, 0), 0);
	gpc_devfreq_pd_release(&gpc_devfreq->gpowercap);
}

static void powercap_cpu_setup_test(struct kunit *test)
{
	struct powercap_test_data *data = test->priv;
	int i = 0;

	// No CPUfreq policy.
	cpu_policy_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT),
			-ENODEV);
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));

	// gpowercap_register error.
	gpc_test_init_data(data);
	gpc_register_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT),
			gpc_register_ret);
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));

	// get opp ct error.
	gpc_test_init_data(data);
	cpu_get_opp_ct_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT),
			cpu_get_opp_ct_ret);
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));

	// opp count is zero.
	gpc_test_init_data(data);
	cpu_get_opp_ct_ret = 0;
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT),
			-ENODEV);
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));

	// get OPP table error.
	gpc_test_init_data(data);
	cpu_get_opp_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT),
			cpu_get_opp_ret);
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));

	// Qos req error.
	gpc_test_init_data(data);
	qos_add_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT),
			qos_add_ret);
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));

	// Success case.
	gpc_test_init_data(data);
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);
	KUNIT_EXPECT_TRUE(test, gpc_register_called);
	KUNIT_EXPECT_TRUE(test, list_is_singular(&gpowercap_cpu_list));

	// Registering for sibling CPUs.
	KUNIT_EXPECT_TRUE(test, list_is_singular(&gpowercap_cpu_list));
	for (; i < GPC_TEST_CPU_MAX; i++) {
		gpc_test_init_data(data);
		KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(i, NULL, HW_CDEV_LIT), 0);
		KUNIT_EXPECT_FALSE(test, gpc_register_called);
		KUNIT_EXPECT_TRUE(test, list_is_singular(&gpowercap_cpu_list));
	}

}

static void powercap_cpu_release_test(struct kunit *test)
{
	struct powercap_test_data *data = test->priv;
	struct gpowercap_cpu *pos, *n;

	// Inactive qos req.
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);
	list_for_each_entry_safe(pos, n, &gpowercap_cpu_list, node) {
		__gpc_cpu_pd_release(&pos->gpowercap);
	}
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));
	KUNIT_EXPECT_FALSE(test, remove_qos_called);

	// Active QOS req.
	gpc_test_init_data(data);
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);

	list_for_each_entry_safe(pos, n, &gpowercap_cpu_list, node) {
		pos->qos_req.qos = kunit_kzalloc(test, sizeof(*pos->qos_req.qos),
						 GFP_KERNEL);
		__gpc_cpu_pd_release(&pos->gpowercap);
	}
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));
	KUNIT_EXPECT_TRUE(test, remove_qos_called);

	// Active QOS req & no policy.
	gpc_test_init_data(data);
	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);
	cpu_policy_ret = -EINVAL;
	list_for_each_entry_safe(pos, n, &gpowercap_cpu_list, node) {
		pos->qos_req.qos = kunit_kzalloc(test, sizeof(*pos->qos_req.qos),
						 GFP_KERNEL);
		__gpc_cpu_pd_release(&pos->gpowercap);
	}
	KUNIT_EXPECT_TRUE(test, list_empty(&gpowercap_cpu_list));
	KUNIT_EXPECT_FALSE(test, remove_qos_called);
}

static void powercap_cpu_update_power_test(struct kunit *test)
{
	int i = 0;
	struct gpowercap_cpu *pos;

	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);
	list_for_each_entry(pos, &gpowercap_cpu_list, node) {
		struct gpowercap *gpc = &pos->gpowercap;

		KUNIT_EXPECT_EQ(test, gpc->num_opps, 0);
		__gpc_cpu_update_cluster_power_uw(gpc);
		KUNIT_EXPECT_EQ(test, gpc->num_opps, GPC_TEST_OPP_CT);

		for (i = 0; i < GPC_TEST_OPP_CT; i++) {
			KUNIT_EXPECT_EQ(test, gpc->opp_table[i].power,
					opp_table[i].power);
			KUNIT_EXPECT_EQ(test, gpc->opp_table[i].freq,
					opp_table[i].freq);
		}
		KUNIT_EXPECT_EQ(test, gpc->power_limit,
				opp_table[GPC_TEST_OPP_CT - 1].power);
		KUNIT_EXPECT_EQ(test, gpc->power_max,
				opp_table[GPC_TEST_OPP_CT - 1].power);
		KUNIT_EXPECT_EQ(test, gpc->power_min, opp_table[0].power);
	}
}

static void powercap_cpu_get_power_test(struct kunit *test)
{
	int i = 0;
	struct gpowercap_cpu *pos;

	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);
	list_for_each_entry(pos, &gpowercap_cpu_list, node) {
		struct gpowercap *gpc = &pos->gpowercap;

		for (i = 0; i < GPC_TEST_OPP_CT; i++) {
			gpc_cpufreq = opp_table[i].freq;
			KUNIT_EXPECT_EQ(test, __gpc_cpu_get_cluster_power_uw(gpc),
					opp_table[i].power);
		}
	}
}

static void powercap_cpu_set_power_test(struct kunit *test)
{
	int i = 0;
	struct gpowercap_cpu *pos;

	KUNIT_EXPECT_EQ(test, __gpc_cpu_setup(GPC_TEST_CPU, NULL, HW_CDEV_LIT), 0);
	list_for_each_entry(pos, &gpowercap_cpu_list, node) {
		struct gpowercap *gpc = &pos->gpowercap;

		for (i = 0; i < GPC_TEST_OPP_CT; i++) {
			struct gpowercap_cpu *gpowercap_cpu = to_gpowercap_cpu(gpc);

			apply_tp_type = THERMAL_PRESSURE_TYPE_MAX;
			cpumask_clear(&apply_tp_cpus);
			KUNIT_EXPECT_EQ(test,
					__gpc_cpu_set_cluster_power_limit(gpc, opp_table[i].power),
					opp_table[i].power);
#if KERNEL_VERSION(6, 12, 0) > LINUX_VERSION_CODE
			KUNIT_EXPECT_EQ(test, apply_tp_type, THERMAL_PRESSURE_TYPE_TSKIN);
			KUNIT_EXPECT_TRUE(test, cpumask_equal(&apply_tp_cpus,
							      &gpowercap_cpu->related_cpus));
#endif
			apply_tp_type = THERMAL_PRESSURE_TYPE_MAX;
			cpumask_clear(&apply_tp_cpus);
			KUNIT_EXPECT_EQ(test,
					__gpc_cpu_set_cluster_power_limit(gpc,
									  opp_table[i].power + 1),
					opp_table[i].power);
#if KERNEL_VERSION(6, 12, 0) > LINUX_VERSION_CODE
			KUNIT_EXPECT_EQ(test, apply_tp_type, THERMAL_PRESSURE_TYPE_TSKIN);
			KUNIT_EXPECT_TRUE(test, cpumask_equal(&apply_tp_cpus,
							      &gpowercap_cpu->related_cpus));
#endif
		}
	}
}
static struct kunit_case powercap_helper_test[] = {
	KUNIT_CASE(powercap_init_test),
	KUNIT_CASE(powercap_create_hierarchy_test),
	KUNIT_CASE(powercap_destroy_hierarchy_test),
	KUNIT_CASE(powercap_setup_virtual_test),
	KUNIT_CASE(powercap_gpc_register_and_ops_test),
	KUNIT_CASE(powercap_power_limit_bypass),
	KUNIT_CASE(powercap_devfreq_setup_test),
	KUNIT_CASE(powercap_devfreq_update_power_test),
	KUNIT_CASE(powercap_devfreq_get_power_test),
	KUNIT_CASE(powercap_devfreq_set_power_test),
	KUNIT_CASE(powercap_cpu_setup_test),
	KUNIT_CASE(powercap_cpu_release_test),
	KUNIT_CASE(powercap_cpu_update_power_test),
	KUNIT_CASE(powercap_cpu_get_power_test),
	KUNIT_CASE(powercap_cpu_set_power_test),
	{},
};

static void powercap_test_suite_exit(struct kunit_suite *suite)
{
	if (!IS_ERR_OR_NULL(pct_test))
		powercap_unregister_control_type(pct_test);
	kfree(opp_table);
	kfree(test_data);
	kfree(test_policy);
}

static void powercap_test_init_cpu(void)
{
	int i = 0;

	test_policy = kzalloc(sizeof(*test_policy), GFP_KERNEL);
	test_policy->cpu = GPC_TEST_CPU;
	for (i = 0; i < GPC_TEST_CPU_MAX; i++) {
		cpumask_set_cpu(i, test_policy->related_cpus);
	}
}

static int powercap_test_suite_init(struct kunit_suite *suite)
{
	int i = 0;

	pct_test = powercap_register_control_type(NULL, "gpc_test", NULL);

	powercap_test_init_cpu();

	opp_table = kcalloc(GPC_TEST_OPP_CT, sizeof(*opp_table), GFP_KERNEL);
	for (i = 0; i < GPC_TEST_OPP_CT; i++) {
		opp_table[i].power = (GPC_POWER_INIT + GPC_POWER_INCREMENT * i);
		opp_table[i].freq = (GPC_FREQ_INIT + GPC_FREQ_INCREMENT * i);
		opp_table[i].voltage = (GPC_VOLT_INIT + GPC_VOLT_INCREMENT * i);
	}
	test_data = kzalloc(sizeof(*test_data), GFP_KERNEL);
	return 0;
}

static int powercap_test_init(struct kunit *test)
{
	test->priv = test_data;
	gpc_test_init_data(test_data);

	return 0;
}

static void powercap_test_exit(struct kunit *test)
{
	struct gpowercap_cpu *pos, *n;

	list_for_each_entry_safe(pos, n, &gpowercap_cpu_list, node) {
		__gpc_cpu_pd_release(&pos->gpowercap);
	}
}

static struct kunit_suite powercap_helper_test_suite = {
	.name = "powercap_helper_tests",
	.test_cases = powercap_helper_test,
	.init = powercap_test_init,
	.exit = powercap_test_exit,
	.suite_init = powercap_test_suite_init,
	.suite_exit = powercap_test_suite_exit
};
kunit_test_suite(powercap_helper_test_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ram Chandrasekar <rchandrasekar@google.com>");

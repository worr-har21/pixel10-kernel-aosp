// SPDX-License-Identifier: GPL-2.0
/*
 * cdev_helper_tests.c Test suite to test all the cdev helper functions.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */

#include <linux/delay.h>
#include <kunit/test.h>
#include <kunit/test-bug.h>

#include "cdev_cpufreq_helper.h"
#include "cdev_devfreq_helper.h"
#include "cdev_helper_mock.h"

#define CDEV_TEST_OPP_CT	3
#define CDEV_FREQ_INIT		300000
#define CDEV_FREQ_INCREMENT	500000
#define CDEV_POWER_INIT		500000
#define CDEV_POWER_INCREMENT	1000000
#define CDEV_VOLT_INIT		100
#define CDEV_VOLT_INCREMENT	200
#define CDEV_TEST_CPU		0

static struct devfreq *test_devfreq;
static struct cdev_opp_table *opp_table;
static int get_devfreq_ret,
	qos_add_ret,
	fw_opp_ct_ret,
	fw_get_freq_ret,
	fw_get_power_ret,
	fw_max_state_ret,
	get_cpu_dev_ret;
static bool remove_qos_called, cdev_failure, cdev_success;
static struct em_perf_domain *test_pd;
static struct em_perf_state *test_pd_table;
static struct device *fake_dev;

struct devfreq *mock_cdev_get_devfreq_by_node(struct device_node *node)
{
	return get_devfreq_ret ? ERR_PTR(get_devfreq_ret) : test_devfreq;
}

int mock_pm_qos_add_and_register_devfreq_request(struct devfreq *devfreq,
						 struct dev_pm_qos_request *req,
						 enum dev_pm_qos_req_type type,
						 s32 value)
{
	return qos_add_ret;
}

struct em_perf_state *mock_em_perf_state_from_pd(struct em_perf_domain *pd)
{
	return test_pd_table;
}

int mock_pm_qos_remove_devfreq_request(struct devfreq *devfreq,
				       struct dev_pm_qos_request *req)
{
	remove_qos_called = true;
	return 0;
}

int mock_dev_pm_opp_get_opp_count(struct device *dev)
{
	return fw_opp_ct_ret ? : CDEV_TEST_OPP_CT;
}

struct dev_pm_opp *mock_dev_pm_opp_find_freq_ceil(struct device *dev,
						  unsigned long *freq)
{
	int i = 0;

	for (i = CDEV_TEST_OPP_CT - 2; i >= 0; i--) {
		if (*freq > opp_table[i].freq)
			break;
	}
	*freq = opp_table[i+1].freq;

	return fw_get_freq_ret ? ERR_PTR(fw_get_freq_ret) : (struct dev_pm_opp *) 0x12345678;
}

int mock_msg_tmu_get_power_table(enum hw_dev_type cdev_id, u8 idx,
				 int *val, int *max_state_idx)
{
	idx = CDEV_TEST_OPP_CT - 1 - idx;
	*val = opp_table[idx].power / MICROWATT_PER_MILLIWATT;
	if (max_state_idx)
		*max_state_idx = fw_max_state_ret;
	return fw_get_power_ret;
}

struct device *mock_get_cpu_device(unsigned int cpu)
{
	return get_cpu_dev_ret ? NULL : fake_dev;
}

unsigned long mock_dev_pm_opp_get_voltage(struct dev_pm_opp *opp)
{
	return CDEV_VOLT_INIT;
}

void cdev_test_init_data(void)
{
	get_devfreq_ret = 0;
	qos_add_ret = 0;
	remove_qos_called = false;
	cdev_success = false;
	cdev_failure = false;
	fw_max_state_ret = CDEV_TEST_OPP_CT - 1;
	fw_opp_ct_ret = 0;
	fw_get_freq_ret = 0;
	fw_get_power_ret = 0;
	get_cpu_dev_ret = 0;
}

static void cdev_devfreq_register_test(struct kunit *test)
{
	struct cdev_devfreq_data *cdev = test->priv;
	int i = 0;

	INIT_DEFERRABLE_WORK(&cdev->work, __cdev_devfreq_fetch_work);
	// get devfreq error.
	cdev_test_init_data();
	get_devfreq_ret = -ENODATA;
	KUNIT_EXPECT_EQ(test, __cdev_devfreq_fetch_register(cdev), -EAGAIN);

	/* Test getting power from EM profile. */
	// No EM profile.
	cdev_test_init_data();
	test_devfreq->dev.parent->em_pd = NULL;
	KUNIT_EXPECT_EQ(test,
			__cdev_devfreq_update_opp_from_pd(cdev, test_devfreq->dev.parent),
			-EINVAL);
	test_devfreq->dev.parent->em_pd = test_pd;

	// No OPP in EM profile.
	test_pd->nr_perf_states = 0;
	KUNIT_EXPECT_EQ(test,
			__cdev_devfreq_update_opp_from_pd(cdev, test_devfreq->dev.parent),
			-ENODATA);
	test_pd->nr_perf_states = CDEV_TEST_OPP_CT;

	// Success case.
	cdev_test_init_data();
	fw_opp_ct_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __cdev_devfreq_fetch_register(cdev), 0);
	KUNIT_EXPECT_EQ(test, cdev->num_opps, test_pd->nr_perf_states);
	for (i = 0; i < test_pd->nr_perf_states; i++) {
		KUNIT_EXPECT_EQ(test, cdev->opp_table[i].power,
				opp_table[i].power);
		KUNIT_EXPECT_EQ(test, cdev->opp_table[i].freq,
				opp_table[i].freq);
	}
	KUNIT_EXPECT_PTR_EQ(test, cdev->devfreq, test_devfreq);
	kfree(cdev->opp_table);
	cdev->devfreq = NULL;

	/* Test getting power from Firmware. */
	// No OPP count.
	cdev_test_init_data();
	fw_opp_ct_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test,
			__cdev_devfreq_update_opp_from_firmware(cdev,
								test_devfreq->dev.parent),
		       -EINVAL);

	// Get FREQ Ceil error.
	cdev_test_init_data();
	fw_get_freq_ret = -ENODATA;
	KUNIT_EXPECT_EQ(test,
			__cdev_devfreq_update_opp_from_firmware(cdev,
								test_devfreq->dev.parent),
		       fw_get_freq_ret);

	// Get FW power error.
	cdev_test_init_data();
	fw_get_power_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test,
			__cdev_devfreq_update_opp_from_firmware(cdev,
								test_devfreq->dev.parent),
		       fw_get_power_ret);

	// Invalid OPP CT.
	cdev_test_init_data();
	fw_max_state_ret = 0;
	KUNIT_EXPECT_EQ(test,
			__cdev_devfreq_update_opp_from_firmware(cdev,
								test_devfreq->dev.parent),
		       -EINVAL);

	// Success.
	cdev_test_init_data();
	// Invalid EM profile. But should get the power from firmware.
	test_devfreq->dev.parent->em_pd = NULL;
	/* Even if firmware reads > opp_ct power values, we only get the first
	 * opp_ct values from firmware.
	 */
	fw_max_state_ret = CDEV_TEST_OPP_CT + 1;
	KUNIT_EXPECT_EQ(test, __cdev_devfreq_fetch_register(cdev), 0);
	KUNIT_EXPECT_EQ(test, cdev->num_opps, CDEV_TEST_OPP_CT);
	for (i = 0; i < CDEV_TEST_OPP_CT; i++) {
		KUNIT_EXPECT_EQ(test, cdev->opp_table[i].power,
				opp_table[i].power);
		KUNIT_EXPECT_EQ(test, cdev->opp_table[i].freq,
				opp_table[i].freq);
	}
	KUNIT_EXPECT_PTR_EQ(test, cdev->devfreq, test_devfreq);
	kfree(cdev->opp_table);
	cdev->devfreq = NULL;
	test_devfreq->dev.parent->em_pd = test_pd;

	// Add QOS req error.
	cdev_test_init_data();
	qos_add_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, __cdev_devfreq_fetch_register(cdev), qos_add_ret);
	KUNIT_EXPECT_NULL(test, cdev->devfreq);
}

static void cdev_devfreq_exit_test(struct kunit *test)
{
	struct cdev_devfreq_data *cdev = test->priv;

	// Inactive qos request
	cdev_test_init_data();
	KUNIT_EXPECT_EQ(test, __cdev_devfreq_fetch_register(cdev), 0);
	cdev_devfreq_exit(cdev);
	KUNIT_EXPECT_FALSE(test, remove_qos_called);

	// Active qos request.
	cdev_test_init_data();
	KUNIT_EXPECT_EQ(test, __cdev_devfreq_fetch_register(cdev), 0);
	cdev->qos_req.dev = test_devfreq->dev.parent;
	cdev_devfreq_exit(cdev);
	KUNIT_EXPECT_TRUE(test, remove_qos_called);
}

static void __cdev_failure_cb(struct cdev_devfreq_data *cdev)
{
	cdev_failure = true;
}

static void __cdev_success_cb(struct cdev_devfreq_data *cdev)
{
	cdev_success = true;
}

static void cdev_devfreq_init_test(struct kunit *test)
{
	struct cdev_devfreq_data *cdev = test->priv;
	struct device_node np;

	// Failure cb.
	cdev_test_init_data();
	fw_opp_ct_ret = -EINVAL;
	test_pd->nr_perf_states = 0;
	KUNIT_EXPECT_EQ(test, cdev_devfreq_init(cdev, &np, HW_CDEV_GPU,
						__cdev_success_cb,
						__cdev_failure_cb),
			0);
	msleep(20);
	KUNIT_EXPECT_TRUE(test, cdev_failure);
	test_pd->nr_perf_states = CDEV_TEST_OPP_CT;

	// success cb.
	cdev_test_init_data();
	KUNIT_EXPECT_EQ(test, cdev_devfreq_init(cdev, &np, HW_CDEV_GPU,
						__cdev_success_cb,
						__cdev_failure_cb),
			0);
	msleep(20);
	KUNIT_EXPECT_TRUE(test, cdev_success);
	cdev_devfreq_exit(cdev);
}

static void cdev_cpufreq_get_opp_ct_test(struct kunit *test)
{
	// Success
	KUNIT_EXPECT_EQ(test, cdev_cpufreq_get_opp_count(CDEV_TEST_CPU), CDEV_TEST_OPP_CT);

	// Invalid CPU
	KUNIT_EXPECT_EQ(test, cdev_cpufreq_get_opp_count(NR_CPUS + 1), -EINVAL);

	// NO CPU dev
	get_cpu_dev_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test, cdev_cpufreq_get_opp_count(CDEV_TEST_CPU), -ENODEV);
}

static void cdev_cpufreq_get_opp_test(struct kunit *test)
{
	struct cdev_opp_table *cdev = kunit_kcalloc(test, CDEV_TEST_OPP_CT,
						    sizeof(*cdev), GFP_KERNEL);
	int i = 0;

	// Invalid arg.
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG,
						      NULL, CDEV_TEST_OPP_CT),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_MAX,
						      cdev, CDEV_TEST_OPP_CT),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG,
						      cdev, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(NR_CPUS + 1, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT+1),
			-EINVAL);

	// NO CPU dev
	cdev_test_init_data();
	get_cpu_dev_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT),
			-ENODEV);

	// FW get power ret
	cdev_test_init_data();
	fw_get_power_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT),
			-EINVAL);

	// FW power tabel mismatch.
	cdev_test_init_data();
	fw_max_state_ret = CDEV_TEST_OPP_CT - 2;
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT),
			-EINVAL);

	// freq ceil return error
	cdev_test_init_data();
	fw_get_freq_ret = -EINVAL;
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT),
			-EINVAL);

	// success
	cdev_test_init_data();
	KUNIT_EXPECT_EQ(test,
			cdev_cpufreq_update_opp_table(CDEV_TEST_CPU, HW_CDEV_BIG, cdev,
						      CDEV_TEST_OPP_CT),
			0);
	for (i = 0; i < CDEV_TEST_OPP_CT; i++) {
		KUNIT_EXPECT_EQ_MSG(test, cdev[i].power,
				    opp_table[i].power, "idx:%d Power\n", i);
		KUNIT_EXPECT_EQ_MSG(test, cdev[i].freq,
				    opp_table[i].freq / HZ_PER_KHZ, "idx:%d Freq\n", i);
		/* Voltage is fetched from the opp datastructure. so if we get the
		 * right entry for freq, then the corresponding volt entry should be
		 * ok.
		 */
		KUNIT_EXPECT_EQ(test, cdev[i].voltage, CDEV_VOLT_INIT);
	}

}

static struct kunit_case cdev_helper_test[] = {
	KUNIT_CASE(cdev_devfreq_register_test),
	KUNIT_CASE(cdev_devfreq_exit_test),
	KUNIT_CASE(cdev_devfreq_init_test),
	KUNIT_CASE(cdev_cpufreq_get_opp_ct_test),
	KUNIT_CASE(cdev_cpufreq_get_opp_test),
	{},
};

static int cdev_test_init(struct kunit *test)
{
	struct cdev_devfreq_data *cdev = kunit_kzalloc(test, sizeof(*cdev), GFP_KERNEL);

	cdev_test_init_data();
	test->priv = cdev;

	return 0;
}

static void cdev_test_suite_exit(struct kunit_suite *suite)
{
	kfree(opp_table);
	root_device_unregister(test_devfreq->dev.parent);
	kfree(test_devfreq);
	kfree(test_pd);
	kfree(test_pd_table);
}

static int cdev_test_suite_init(struct kunit_suite *suite)
{
	int i = 0;

	test_pd_table = kcalloc(CDEV_TEST_OPP_CT, sizeof(*test_pd_table), GFP_KERNEL);
	opp_table = kcalloc(CDEV_TEST_OPP_CT, sizeof(*opp_table), GFP_KERNEL);
	for (i = 0; i < CDEV_TEST_OPP_CT; i++) {
		opp_table[i].power = (CDEV_POWER_INIT + CDEV_POWER_INCREMENT * i);
		opp_table[i].freq = (CDEV_FREQ_INIT + CDEV_FREQ_INCREMENT * i);
		opp_table[i].voltage = (CDEV_VOLT_INIT + CDEV_VOLT_INCREMENT * i);

		test_pd_table[i].power = opp_table[i].power;
		test_pd_table[i].frequency = opp_table[i].freq;
	}
	test_pd = kzalloc(sizeof(*test_pd), GFP_KERNEL);
	test_pd->nr_perf_states = CDEV_TEST_OPP_CT;
	test_devfreq = kzalloc(sizeof(*test_devfreq), GFP_KERNEL);
	fake_dev = test_devfreq->dev.parent = root_device_register("mock_thermal-device");
	test_devfreq->dev.parent->em_pd = test_pd;

	return 0;
}

static struct kunit_suite cdev_helper_test_suite = {
	.name = "cdev_helper_tests",
	.test_cases = cdev_helper_test,
	.init = cdev_test_init,
	.suite_init = cdev_test_suite_init,
	.suite_exit = cdev_test_suite_exit
};
kunit_test_suite(cdev_helper_test_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ram Chandrasekar <rchandrasekar@google.com>");

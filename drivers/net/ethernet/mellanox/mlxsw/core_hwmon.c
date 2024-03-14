// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2015-2018 Mellanox Technologies. All rights reserved */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/sfp.h>

#include "core.h"
#include "core_env.h"

#define MLXSW_HWMON_SENSORS_MAX_COUNT 64
#define MLXSW_HWMON_MODULES_MAX_COUNT 64
#define MLXSW_HWMON_GEARBOXES_MAX_COUNT 32

#define MLXSW_HWMON_ATTR_PER_SENSOR 3
#define MLXSW_HWMON_ATTR_PER_MODULE 7
#define MLXSW_HWMON_ATTR_PER_GEARBOX 4
#define MLXSW_HWMON_DEV_NAME_LEN_MAX 16

#define MLXSW_HWMON_ATTR_COUNT (MLXSW_HWMON_SENSORS_MAX_COUNT * MLXSW_HWMON_ATTR_PER_SENSOR + \
				MLXSW_HWMON_MODULES_MAX_COUNT * MLXSW_HWMON_ATTR_PER_MODULE + \
				MLXSW_HWMON_GEARBOXES_MAX_COUNT * MLXSW_HWMON_ATTR_PER_GEARBOX + \
				MLXSW_MFCR_TACHOS_MAX + MLXSW_MFCR_PWMS_MAX)

struct mlxsw_hwmon_attr {
	struct device_attribute dev_attr;
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev;
	unsigned int type_index;
	char name[32];
};

static int mlxsw_hwmon_get_attr_index(int index, int count)
{
	if (index >= count)
		return index % count + MLXSW_REG_MTMP_GBOX_INDEX_MIN;

	return index;
}

struct mlxsw_hwmon_dev {
	char name[MLXSW_HWMON_DEV_NAME_LEN_MAX];
	struct mlxsw_hwmon *hwmon;
	struct device *hwmon_dev;
	struct attribute_group group;
	const struct attribute_group *groups[2];
	struct attribute *attrs[MLXSW_HWMON_ATTR_COUNT + 1];
	struct mlxsw_hwmon_attr hwmon_attrs[MLXSW_HWMON_ATTR_COUNT];
	unsigned int attrs_count;
	u8 sensor_count;
	u8 module_sensor_max;
	u8 slot_index;
	bool active;
};

struct mlxsw_hwmon {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	struct mlxsw_hwmon_dev line_cards[];
};

static ssize_t mlxsw_hwmon_temp_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int temp, index;
	int err;

	index = mlxsw_hwmon_get_attr_index(mlxsw_hwmon_attr->type_index,
					   mlxsw_hwmon_dev->module_sensor_max);
	mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index, index, false,
			    false);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query temp sensor\n");
		return err;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, &temp, NULL, NULL, NULL, NULL);
	return sprintf(buf, "%d\n", temp);
}

static ssize_t mlxsw_hwmon_temp_max_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int temp_max, index;
	int err;

	index = mlxsw_hwmon_get_attr_index(mlxsw_hwmon_attr->type_index,
					   mlxsw_hwmon_dev->module_sensor_max);
	mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index, index, false,
			    false);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query temp sensor\n");
		return err;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, NULL, &temp_max, NULL, NULL, NULL);
	return sprintf(buf, "%d\n", temp_max);
}

static ssize_t mlxsw_hwmon_temp_rst_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	unsigned long val;
	int index;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;
	if (val != 1)
		return -EINVAL;

	index = mlxsw_hwmon_get_attr_index(mlxsw_hwmon_attr->type_index,
					   mlxsw_hwmon_dev->module_sensor_max);

	mlxsw_reg_mtmp_slot_index_set(mtmp_pl, mlxsw_hwmon_dev->slot_index);
	mlxsw_reg_mtmp_sensor_index_set(mtmp_pl, index);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err)
		return err;
	mlxsw_reg_mtmp_mte_set(mtmp_pl, true);
	mlxsw_reg_mtmp_mtr_set(mtmp_pl, true);
	err = mlxsw_reg_write(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to reset temp sensor history\n");
		return err;
	}
	return len;
}

static ssize_t mlxsw_hwmon_fan_rpm_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfsm_pl[MLXSW_REG_MFSM_LEN];
	int err;

	mlxsw_reg_mfsm_pack(mfsm_pl, mlxsw_hwmon_attr->type_index);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mfsm), mfsm_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query fan\n");
		return err;
	}
	return sprintf(buf, "%u\n", mlxsw_reg_mfsm_rpm_get(mfsm_pl));
}

static ssize_t mlxsw_hwmon_fan_fault_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char fore_pl[MLXSW_REG_FORE_LEN];
	bool fault;
	int err;

	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(fore), fore_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query fan\n");
		return err;
	}
	mlxsw_reg_fore_unpack(fore_pl, mlxsw_hwmon_attr->type_index, &fault);

	return sprintf(buf, "%u\n", fault);
}

static ssize_t mlxsw_hwmon_pwm_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfsc_pl[MLXSW_REG_MFSC_LEN];
	int err;

	mlxsw_reg_mfsc_pack(mfsc_pl, mlxsw_hwmon_attr->type_index, 0);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mfsc), mfsc_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query PWM\n");
		return err;
	}
	return sprintf(buf, "%u\n",
		       mlxsw_reg_mfsc_pwm_duty_cycle_get(mfsc_pl));
}

static ssize_t mlxsw_hwmon_pwm_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfsc_pl[MLXSW_REG_MFSC_LEN];
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;
	if (val > 255)
		return -EINVAL;

	mlxsw_reg_mfsc_pack(mfsc_pl, mlxsw_hwmon_attr->type_index, val);
	err = mlxsw_reg_write(mlxsw_hwmon->core, MLXSW_REG(mfsc), mfsc_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to write PWM\n");
		return err;
	}
	return len;
}

static int mlxsw_hwmon_module_temp_get(struct device *dev,
				       struct device_attribute *attr,
				       int *p_temp)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	u8 module;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index,
			    MLXSW_REG_MTMP_MODULE_INDEX_MIN + module, false,
			    false);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(dev, "Failed to query module temperature\n");
		return err;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, p_temp, NULL, NULL, NULL, NULL);

	return 0;
}

static ssize_t mlxsw_hwmon_module_temp_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	int err, temp;

	err = mlxsw_hwmon_module_temp_get(dev, attr, &temp);
	if (err)
		return err;

	return sprintf(buf, "%d\n", temp);
}

static ssize_t mlxsw_hwmon_module_temp_fault_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtbr_pl[MLXSW_REG_MTBR_LEN] = {0};
	u8 module, fault;
	u16 temp;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	mlxsw_reg_mtbr_pack(mtbr_pl, mlxsw_hwmon_dev->slot_index,
			    MLXSW_REG_MTBR_BASE_MODULE_INDEX + module, 1);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtbr), mtbr_pl);
	if (err) {
		dev_err(dev, "Failed to query module temperature sensor\n");
		return err;
	}

	mlxsw_reg_mtbr_temp_unpack(mtbr_pl, 0, &temp, NULL);

	/* Update status and temperature cache. */
	switch (temp) {
	case MLXSW_REG_MTBR_BAD_SENS_INFO:
		/* Untrusted cable is connected. Reading temperature from its
		 * sensor is faulty.
		 */
		fault = 1;
		break;
	case MLXSW_REG_MTBR_NO_CONN:
	case MLXSW_REG_MTBR_NO_TEMP_SENS:
	case MLXSW_REG_MTBR_INDEX_NA:
	default:
		fault = 0;
		break;
	}

	return sprintf(buf, "%u\n", fault);
}

static int mlxsw_hwmon_module_temp_critical_get(struct device *dev,
						struct device_attribute *attr,
						int *p_temp)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	u8 module;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	err = mlxsw_env_module_temp_thresholds_get(mlxsw_hwmon->core,
						   mlxsw_hwmon_dev->slot_index,
						   module, SFP_TEMP_HIGH_WARN,
						   p_temp);
	if (err) {
		dev_err(dev, "Failed to query module temperature thresholds\n");
		return err;
	}

	return 0;
}

static ssize_t
mlxsw_hwmon_module_temp_critical_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	int err, temp;

	err = mlxsw_hwmon_module_temp_critical_get(dev, attr, &temp);
	if (err)
		return err;

	return sprintf(buf, "%u\n", temp);
}

static int mlxsw_hwmon_module_temp_emergency_get(struct device *dev,
						 struct device_attribute *attr,
						 int *p_temp)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	u8 module;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	err = mlxsw_env_module_temp_thresholds_get(mlxsw_hwmon->core,
						   mlxsw_hwmon_dev->slot_index,
						   module, SFP_TEMP_HIGH_ALARM,
						   p_temp);
	if (err) {
		dev_err(dev, "Failed to query module temperature thresholds\n");
		return err;
	}

	return 0;
}

static ssize_t
mlxsw_hwmon_module_temp_emergency_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	int err, temp;

	err = mlxsw_hwmon_module_temp_emergency_get(dev, attr, &temp);
	if (err)
		return err;

	return sprintf(buf, "%u\n", temp);
}

static ssize_t
mlxsw_hwmon_module_temp_label_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);

	return sprintf(buf, "front panel %03u\n",
		       mlxsw_hwmon_attr->type_index + 1 -
		       mlxsw_hwmon_attr->mlxsw_hwmon_dev->sensor_count);
}

static ssize_t
mlxsw_hwmon_gbox_temp_label_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	int index = mlxsw_hwmon_attr->type_index -
		    mlxsw_hwmon_dev->module_sensor_max + 1;

	return sprintf(buf, "gearbox %03u\n", index);
}

static ssize_t mlxsw_hwmon_temp_critical_alarm_show(struct device *dev,
						    struct device_attribute *attr,
						    char *buf)
{
	int err, temp, emergency_temp, critic_temp;

	err = mlxsw_hwmon_module_temp_get(dev, attr, &temp);
	if (err)
		return err;

	if (temp <= 0)
		return sprintf(buf, "%d\n", false);

	err = mlxsw_hwmon_module_temp_emergency_get(dev, attr, &emergency_temp);
	if (err)
		return err;

	if (temp >= emergency_temp)
		return sprintf(buf, "%d\n", false);

	err = mlxsw_hwmon_module_temp_critical_get(dev, attr, &critic_temp);
	if (err)
		return err;

	return sprintf(buf, "%d\n", temp >= critic_temp);
}

static ssize_t mlxsw_hwmon_temp_emergency_alarm_show(struct device *dev,
						     struct device_attribute *attr,
						     char *buf)
{
	int err, temp, emergency_temp;

	err = mlxsw_hwmon_module_temp_get(dev, attr, &temp);
	if (err)
		return err;

	if (temp <= 0)
		return sprintf(buf, "%d\n", false);

	err = mlxsw_hwmon_module_temp_emergency_get(dev, attr, &emergency_temp);
	if (err)
		return err;

	return sprintf(buf, "%d\n", temp >= emergency_temp);
}

enum mlxsw_hwmon_attr_type {
	MLXSW_HWMON_ATTR_TYPE_TEMP,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MAX,
	MLXSW_HWMON_ATTR_TYPE_TEMP_RST,
	MLXSW_HWMON_ATTR_TYPE_FAN_RPM,
	MLXSW_HWMON_ATTR_TYPE_FAN_FAULT,
	MLXSW_HWMON_ATTR_TYPE_PWM,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_FAULT,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_CRIT,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_EMERG,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_LABEL,
	MLXSW_HWMON_ATTR_TYPE_TEMP_GBOX_LABEL,
	MLXSW_HWMON_ATTR_TYPE_TEMP_CRIT_ALARM,
	MLXSW_HWMON_ATTR_TYPE_TEMP_EMERGENCY_ALARM,
};

static void mlxsw_hwmon_attr_add(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev,
				 enum mlxsw_hwmon_attr_type attr_type,
				 unsigned int type_index, unsigned int num)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr;
	unsigned int attr_index;

	attr_index = mlxsw_hwmon_dev->attrs_count;
	mlxsw_hwmon_attr = &mlxsw_hwmon_dev->hwmon_attrs[attr_index];

	switch (attr_type) {
	case MLXSW_HWMON_ATTR_TYPE_TEMP:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_temp_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_input", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MAX:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_temp_max_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_highest", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_RST:
		mlxsw_hwmon_attr->dev_attr.store = mlxsw_hwmon_temp_rst_store;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0200;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_reset_history", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_FAN_RPM:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_fan_rpm_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "fan%u_input", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_FAN_FAULT:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_fan_fault_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "fan%u_fault", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_PWM:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_pwm_show;
		mlxsw_hwmon_attr->dev_attr.store = mlxsw_hwmon_pwm_store;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0644;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "pwm%u", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_module_temp_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_input", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_FAULT:
		mlxsw_hwmon_attr->dev_attr.show =
					mlxsw_hwmon_module_temp_fault_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_fault", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_CRIT:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_module_temp_critical_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_crit", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_EMERG:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_module_temp_emergency_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_emergency", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_LABEL:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_module_temp_label_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_label", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_GBOX_LABEL:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_gbox_temp_label_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_label", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_CRIT_ALARM:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_temp_critical_alarm_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_crit_alarm", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_EMERGENCY_ALARM:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_temp_emergency_alarm_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_emergency_alarm", num + 1);
		break;
	default:
		WARN_ON(1);
	}

	mlxsw_hwmon_attr->type_index = type_index;
	mlxsw_hwmon_attr->mlxsw_hwmon_dev = mlxsw_hwmon_dev;
	mlxsw_hwmon_attr->dev_attr.attr.name = mlxsw_hwmon_attr->name;
	sysfs_attr_init(&mlxsw_hwmon_attr->dev_attr.attr);

	mlxsw_hwmon_dev->attrs[attr_index] = &mlxsw_hwmon_attr->dev_attr.attr;
	mlxsw_hwmon_dev->attrs_count++;
}

static int mlxsw_hwmon_temp_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtcap_pl[MLXSW_REG_MTCAP_LEN] = {0};
	int i;
	int err;

	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtcap), mtcap_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to get number of temp sensors\n");
		return err;
	}
	mlxsw_hwmon_dev->sensor_count = mlxsw_reg_mtcap_sensor_count_get(mtcap_pl);
	for (i = 0; i < mlxsw_hwmon_dev->sensor_count; i++) {
		char mtmp_pl[MLXSW_REG_MTMP_LEN] = {0};

		mlxsw_reg_mtmp_slot_index_set(mtmp_pl,
					      mlxsw_hwmon_dev->slot_index);
		mlxsw_reg_mtmp_sensor_index_set(mtmp_pl, i);
		err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp),
				      mtmp_pl);
		if (err)
			return err;
		mlxsw_reg_mtmp_mte_set(mtmp_pl, true);
		mlxsw_reg_mtmp_mtr_set(mtmp_pl, true);
		err = mlxsw_reg_write(mlxsw_hwmon->core,
				      MLXSW_REG(mtmp), mtmp_pl);
		if (err) {
			dev_err(mlxsw_hwmon->bus_info->dev, "Failed to setup temp sensor number %d\n",
				i);
			return err;
		}
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP, i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MAX, i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_RST, i, i);
	}
	return 0;
}

static int mlxsw_hwmon_fans_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfcr_pl[MLXSW_REG_MFCR_LEN] = {0};
	enum mlxsw_reg_mfcr_pwm_frequency freq;
	unsigned int type_index;
	unsigned int num;
	u16 tacho_active;
	u8 pwm_active;
	int err;

	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mfcr), mfcr_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to get to probe PWMs and Tachometers\n");
		return err;
	}
	mlxsw_reg_mfcr_unpack(mfcr_pl, &freq, &tacho_active, &pwm_active);
	num = 0;
	for (type_index = 0; type_index < MLXSW_MFCR_TACHOS_MAX; type_index++) {
		if (tacho_active & BIT(type_index)) {
			mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
					     MLXSW_HWMON_ATTR_TYPE_FAN_RPM,
					     type_index, num);
			mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
					     MLXSW_HWMON_ATTR_TYPE_FAN_FAULT,
					     type_index, num++);
		}
	}
	num = 0;
	for (type_index = 0; type_index < MLXSW_MFCR_PWMS_MAX; type_index++) {
		if (pwm_active & BIT(type_index))
			mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
					     MLXSW_HWMON_ATTR_TYPE_PWM,
					     type_index, num++);
	}
	return 0;
}

static int mlxsw_hwmon_module_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	u8 module_sensor_max;
	int i, err;

	mlxsw_reg_mgpir_pack(mgpir_pl, mlxsw_hwmon_dev->slot_index);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
			       &module_sensor_max, NULL);

	/* Add extra attributes for module temperature. Sensor index is
	 * assigned to sensor_count value, while all indexed before
	 * sensor_count are already utilized by the sensors connected through
	 * mtmp register by mlxsw_hwmon_temp_init().
	 */
	mlxsw_hwmon_dev->module_sensor_max = mlxsw_hwmon_dev->sensor_count +
					     module_sensor_max;
	for (i = mlxsw_hwmon_dev->sensor_count;
	     i < mlxsw_hwmon_dev->module_sensor_max; i++) {
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE, i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_FAULT,
				     i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_CRIT, i,
				     i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_EMERG,
				     i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_LABEL,
				     i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_CRIT_ALARM,
				     i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_EMERGENCY_ALARM,
				     i, i);
	}

	return 0;
}

static int mlxsw_hwmon_gearbox_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	enum mlxsw_reg_mgpir_device_type device_type;
	int index, max_index, sensor_index;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	u8 gbox_num;
	int err;

	mlxsw_reg_mgpir_pack(mgpir_pl, mlxsw_hwmon_dev->slot_index);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, &gbox_num, &device_type, NULL, NULL,
			       NULL);
	if (device_type != MLXSW_REG_MGPIR_DEVICE_TYPE_GEARBOX_DIE ||
	    !gbox_num)
		return 0;

	index = mlxsw_hwmon_dev->module_sensor_max;
	max_index = mlxsw_hwmon_dev->module_sensor_max + gbox_num;
	while (index < max_index) {
		sensor_index = index % mlxsw_hwmon_dev->module_sensor_max +
			       MLXSW_REG_MTMP_GBOX_INDEX_MIN;
		mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index,
				    sensor_index, true, true);
		err = mlxsw_reg_write(mlxsw_hwmon->core,
				      MLXSW_REG(mtmp), mtmp_pl);
		if (err) {
			dev_err(mlxsw_hwmon->bus_info->dev, "Failed to setup temp sensor number %d\n",
				sensor_index);
			return err;
		}
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP, index, index);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MAX, index,
				     index);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_RST, index,
				     index);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_GBOX_LABEL,
				     index, index);
		index++;
	}

	return 0;
}

static void
mlxsw_hwmon_got_active(struct mlxsw_core *mlxsw_core, u8 slot_index,
		       void *priv)
{
	struct mlxsw_hwmon *hwmon = priv;
	struct mlxsw_hwmon_dev *linecard;
	struct device *dev;
	int err;

	dev = hwmon->bus_info->dev;
	linecard = &hwmon->line_cards[slot_index];
	if (linecard->active)
		return;
	/* For the main board, module sensor indexes start from 1, sensor index
	 * 0 is used for the ASIC. Use the same numbering for line cards.
	 */
	linecard->sensor_count = 1;
	linecard->slot_index = slot_index;
	linecard->hwmon = hwmon;
	err = mlxsw_hwmon_module_init(linecard);
	if (err) {
		dev_err(dev, "Failed to configure hwmon objects for line card modules in slot %d\n",
			slot_index);
		return;
	}

	err = mlxsw_hwmon_gearbox_init(linecard);
	if (err) {
		dev_err(dev, "Failed to configure hwmon objects for line card gearboxes in slot %d\n",
			slot_index);
		return;
	}

	linecard->groups[0] = &linecard->group;
	linecard->group.attrs = linecard->attrs;
	sprintf(linecard->name, "%s#%02u", "linecard", slot_index);
	linecard->hwmon_dev =
		hwmon_device_register_with_groups(dev, linecard->name,
						  linecard, linecard->groups);
	if (IS_ERR(linecard->hwmon_dev)) {
		dev_err(dev, "Failed to register hwmon objects for line card in slot %d\n",
			slot_index);
		return;
	}

	linecard->active = true;
}

static void
mlxsw_hwmon_got_inactive(struct mlxsw_core *mlxsw_core, u8 slot_index,
			 void *priv)
{
	struct mlxsw_hwmon *hwmon = priv;
	struct mlxsw_hwmon_dev *linecard;

	linecard = &hwmon->line_cards[slot_index];
	if (!linecard->active)
		return;
	linecard->active = false;
	hwmon_device_unregister(linecard->hwmon_dev);
	/* Reset attributes counter */
	linecard->attrs_count = 0;
}

static struct mlxsw_linecards_event_ops mlxsw_hwmon_event_ops = {
	.got_active = mlxsw_hwmon_got_active,
	.got_inactive = mlxsw_hwmon_got_inactive,
};

int mlxsw_hwmon_init(struct mlxsw_core *mlxsw_core,
		     const struct mlxsw_bus_info *mlxsw_bus_info,
		     struct mlxsw_hwmon **p_hwmon)
{
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	struct mlxsw_hwmon *mlxsw_hwmon;
	struct device *hwmon_dev;
	u8 num_of_slots;
	int err;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(mlxsw_core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL, NULL,
			       &num_of_slots);

	mlxsw_hwmon = kzalloc(struct_size(mlxsw_hwmon, line_cards,
					  num_of_slots + 1), GFP_KERNEL);
	if (!mlxsw_hwmon)
		return -ENOMEM;

	mlxsw_hwmon->core = mlxsw_core;
	mlxsw_hwmon->bus_info = mlxsw_bus_info;
	mlxsw_hwmon->line_cards[0].hwmon = mlxsw_hwmon;
	mlxsw_hwmon->line_cards[0].slot_index = 0;

	err = mlxsw_hwmon_temp_init(&mlxsw_hwmon->line_cards[0]);
	if (err)
		goto err_temp_init;

	err = mlxsw_hwmon_fans_init(&mlxsw_hwmon->line_cards[0]);
	if (err)
		goto err_fans_init;

	err = mlxsw_hwmon_module_init(&mlxsw_hwmon->line_cards[0]);
	if (err)
		goto err_temp_module_init;

	err = mlxsw_hwmon_gearbox_init(&mlxsw_hwmon->line_cards[0]);
	if (err)
		goto err_temp_gearbox_init;

	mlxsw_hwmon->line_cards[0].groups[0] = &mlxsw_hwmon->line_cards[0].group;
	mlxsw_hwmon->line_cards[0].group.attrs = mlxsw_hwmon->line_cards[0].attrs;

	hwmon_dev = hwmon_device_register_with_groups(mlxsw_bus_info->dev,
						      "mlxsw",
						      &mlxsw_hwmon->line_cards[0],
						      mlxsw_hwmon->line_cards[0].groups);
	if (IS_ERR(hwmon_dev)) {
		err = PTR_ERR(hwmon_dev);
		goto err_hwmon_register;
	}

	err = mlxsw_linecards_event_ops_register(mlxsw_hwmon->core,
						 &mlxsw_hwmon_event_ops,
						 mlxsw_hwmon);
	if (err)
		goto err_linecards_event_ops_register;

	mlxsw_hwmon->line_cards[0].hwmon_dev = hwmon_dev;
	mlxsw_hwmon->line_cards[0].active = true;
	*p_hwmon = mlxsw_hwmon;
	return 0;

err_linecards_event_ops_register:
	hwmon_device_unregister(mlxsw_hwmon->line_cards[0].hwmon_dev);
err_hwmon_register:
err_temp_gearbox_init:
err_temp_module_init:
err_fans_init:
err_temp_init:
	kfree(mlxsw_hwmon);
	return err;
}

void mlxsw_hwmon_fini(struct mlxsw_hwmon *mlxsw_hwmon)
{
	mlxsw_hwmon->line_cards[0].active = false;
	mlxsw_linecards_event_ops_unregister(mlxsw_hwmon->core,
					     &mlxsw_hwmon_event_ops, mlxsw_hwmon);
	hwmon_device_unregister(mlxsw_hwmon->line_cards[0].hwmon_dev);
	kfree(mlxsw_hwmon);
}

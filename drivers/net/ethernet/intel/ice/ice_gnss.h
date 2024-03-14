/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021-2022, Intel Corporation. */

#ifndef _ICE_GNSS_H_
#define _ICE_GNSS_H_

#define ICE_E810T_GNSS_I2C_BUS		0x2
#define ICE_GNSS_POLL_DATA_DELAY_TIME	(HZ / 50) /* poll every 20 ms */
#define ICE_GNSS_TIMER_DELAY_TIME	(HZ / 10) /* 0.1 second per message */
#define ICE_GNSS_TTY_WRITE_BUF		250
#define ICE_MAX_I2C_DATA_SIZE		FIELD_MAX(ICE_AQC_I2C_DATA_SIZE_M)
#define ICE_MAX_I2C_WRITE_BYTES		4

/* u-blox ZED-F9T specific definitions */
#define ICE_GNSS_UBX_I2C_BUS_ADDR	0x42
/* Data length register is big endian */
#define ICE_GNSS_UBX_DATA_LEN_H		0xFD
#define ICE_GNSS_UBX_DATA_LEN_WIDTH	2
#define ICE_GNSS_UBX_EMPTY_DATA		0xFF
/* For u-blox writes are performed without address so the first byte to write is
 * passed as I2C addr parameter.
 */
#define ICE_GNSS_UBX_WRITE_BYTES	(ICE_MAX_I2C_WRITE_BYTES + 1)

/**
 * struct gnss_serial - data used to initialize GNSS TTY port
 * @back: back pointer to PF
 * @kworker: kwork thread for handling periodic work
 * @read_work: read_work function for handling GNSS reads
 */
struct gnss_serial {
	struct ice_pf *back;
	struct kthread_worker *kworker;
	struct kthread_delayed_work read_work;
};

#if IS_ENABLED(CONFIG_GNSS)
void ice_gnss_init(struct ice_pf *pf);
void ice_gnss_exit(struct ice_pf *pf);
bool ice_gnss_is_gps_present(struct ice_hw *hw);
#else
static inline void ice_gnss_init(struct ice_pf *pf) { }
static inline void ice_gnss_exit(struct ice_pf *pf) { }
static inline bool ice_gnss_is_gps_present(struct ice_hw *hw)
{
	return false;
}
#endif /* IS_ENABLED(CONFIG_GNSS) */
#endif /* _ICE_GNSS_H_ */

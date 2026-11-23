/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file imu.h
 * @brief The QMI8658 (0x6B), accelerometer only: steps and raising the wrist.
 */
#ifndef IMU_H_
#define IMU_H_

#include <stdbool.h>
#include <stdint.h>

void imu_init(void);
bool imu_ok(void);
/** @brief Steps counted since boot (the OS keeps the day's total). */
uint32_t imu_steps(void);
/** @brief true once after the wrist was raised to look at the watch. */
bool imu_raised(void);
/** @brief Mean movement (milli-g away from the slow average) since the last call; -1 when unknown. */
int imu_motion_take(void);

#endif /* IMU_H_ */

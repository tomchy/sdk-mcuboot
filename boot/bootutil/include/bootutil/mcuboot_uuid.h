/*
 *  Copyright (c) 2025 Nordic Semiconductor ASA
 *
 *  SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MCUBOOT_UUID_H__
#define __MCUBOOT_UUID_H__

/**
 * @file uuid.h
 *
 * @note A vendor ID as well as class ID values may be statically generated
 *       using CMake, based on the vendor domain name as well as product name.
 *       It is advised to use vendor ID as an input while generating device
 *       class ID to avoid collisions between UUIDs from two different vendors.
 */

#include <stdint.h>
#include "bootutil/fault_injection_hardening.h"

#ifdef __cplusplus
extern "C" {
#endif


/** The 128-bit UUID, used for identifying vendors as well as classes. */
struct image_uuid {
	uint8_t raw[16];
};

/**
 * @brief Initialises the UUID module.
 *
 * @return FIH_SUCCESS on success
 */
fih_ret boot_uuid_init(void);

/**
 * @brief Checks if the specified image should have a vendor ID present.
 *
 * @param[in] image_index  Index of the image to check (from 0).
 *
 * @return FIH_SUCCESS if vendor ID should be present; FIH_FAILURE if otherwise
 */
fih_ret boot_image_should_have_uuid_vid(uint32_t image_index);

/**
 * @brief Reads the stored value of a given image's expected vendor ID.
 *
 * @param[in]  image_id  Index of the image (from 0).
 * @param[out] uuid_vid  Pointer to store the reference to the vendor ID value.
 *
 * @return FIH_SUCCESS on success
 */
fih_ret boot_uuid_vid_get(uint32_t image_id, const struct image_uuid **uuid_vid);

/**
 * @brief Checks if the specified image should have a class ID present.
 *
 * @param[in] image_index  Index of the image to check (from 0).
 *
 * @return FIH_SUCCESS if class ID should be present; FIH_FAILURE if otherwise
 */
fih_ret boot_image_should_have_uuid_cid(uint32_t image_index);

/**
 * @brief Reads the stored value of a given image's expected class ID.
 *
 * @param[in]  image_id  Index of the image (from 0).
 * @param[out] uuid_cid  Pointer to store the reference to the class ID value.
 *
 * @return FIH_SUCCESS on success
 */
fih_ret boot_uuid_cid_get(uint32_t image_id, const struct image_uuid **uuid_cid);

/**
 * @brief Checks if two image_uuid structures hold the same UUID value.
 *
 * @param[in] uuid1  UUID to compare.
 * @param[in] uuid2  UUID to compare with.
 *
 * @return FIH_SUCCESS on success
 */
fih_ret boot_uuid_compare(const struct image_uuid *uuid1, const struct image_uuid *uuid2);

//fih_ret_encode_zero_equality(memcmp(uuid1->raw, uuid2->raw, ARRAY_SIZE(uuid1->raw)));
#ifdef __cplusplus
}
#endif

#endif /* __MCUBOOT_UUID_H__ */

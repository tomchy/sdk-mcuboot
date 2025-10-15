/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2016-2020 Linaro LTD
 * Copyright (c) 2016-2019 JUUL Labs
 * Copyright (c) 2019-2023 Arm Limited
 * Copyright (c) 2024-2025 Nordic Semiconductor ASA
 *
 * Original license:
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * This file provides an interface to the manifest-based boot loader.
 * Functions defined in this file should only be called while the boot loader is
 * running.
 */

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "flash_map_backend/flash_map_backend.h"
#include "mcuboot_config/mcuboot_config.h"
#include "bootutil/bootutil.h"
#include "bootutil/bootutil_public.h"
#include "bootutil/image.h"
#include "bootutil_priv.h"
#include "bootutil/bootutil_log.h"
#include "bootutil/security_cnt.h"
#include "bootutil/boot_record.h"
#include "bootutil/fault_injection_hardening.h"
#include "bootutil/ramload.h"
#include "bootutil/boot_hooks.h"
#include "bootutil/mcuboot_status.h"

#if defined(MCUBOOT_MANIFEST_UPDATES) && (defined(MCUBOOT_DIRECT_XIP) || defined(MCUBOOT_RAM_LOAD))
#include "bootutil/mcuboot_manifest.h"

#if defined(MCUBOOT_DECOMPRESS_IMAGES)
#include <nrf_compress/implementation.h>
#include <compression/decompression.h>
#endif

#if defined(MCUBOOT_DIRECT_XIP) && defined(MCUBOOT_DECOMPRESS_IMAGES)
#error "Image decompression (MCUBOOT_DECOMPRESS_IMAGES) is not supported when MCUBOOT_DIRECT_XIP is selected."
#endif /* MCUBOOT_DIRECT_XIP && MCUBOOT_DECOMPRESS_IMAGES */

#ifdef __ZEPHYR__
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_WAIT_MS)
#include <zephyr/kernel.h>
#endif /* CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_WAIT_MS */
#endif

#ifdef MCUBOOT_ENC_IMAGES
#include "bootutil/enc_key.h"
#endif

#ifdef CONFIG_SOC_EARLY_RESET_HOOK
void s2ram_designate_slot(uint8_t slot);
#endif

BOOT_LOG_MODULE_DECLARE(mcuboot);

static struct boot_loader_state boot_data;

#if defined(MCUBOOT_SERIAL_IMG_GRP_SLOT_INFO) || defined(MCUBOOT_DATA_SHARING)
static struct image_max_size image_max_sizes[BOOT_IMAGE_NUMBER] = {0};
#endif

/*
 * This macro allows some control on the allocation of local variables.
 * When running natively on a target, we don't want to allocated huge
 * variables on the stack, so make them global instead. For the simulator
 * we want to run as many threads as there are tests, and it's safer
 * to just make those variables stack allocated.
 */
#if !defined(__BOOTSIM__)
#define TARGET_STATIC static
#else
#define TARGET_STATIC
#endif

#if BOOT_MAX_ALIGN > 1024
#define BUF_SZ BOOT_MAX_ALIGN
#else
#define BUF_SZ 1024
#endif

struct boot_loader_state *boot_get_loader_state(void)
{
    return &boot_data;
}

#if defined(MCUBOOT_SERIAL_IMG_GRP_SLOT_INFO)
struct image_max_size *boot_get_image_max_sizes(void)
{
    return image_max_sizes;
}
#endif

static int
boot_read_image_headers(struct boot_loader_state *state, bool require_all,
        struct boot_status *bs)
{
    int rc;
    int i;

    for (i = 0; i < BOOT_NUM_SLOTS; i++) {
        rc = BOOT_HOOK_CALL(boot_read_image_header_hook, BOOT_HOOK_REGULAR,
                            BOOT_CURR_IMG(state), i, boot_img_hdr(state, i));
        if (rc == BOOT_HOOK_REGULAR)
        {
            rc = boot_read_image_header(state, i, boot_img_hdr(state, i), bs);
        }
        if (rc != 0) {
            /* If `require_all` is set, fail on any single fail, otherwise
             * if at least the first slot's header was read successfully,
             * then the boot loader can attempt a boot.
             *
             * Failure to read any headers is a fatal error.
             */
            if (i > 0 && !require_all) {
                return 0;
            } else {
                return rc;
            }
        }
    }

    return 0;
}

/**
 * Saves boot status and shared data for current image.
 *
 * @param  state        Boot loader status information.
 * @param  active_slot  Index of the slot will be loaded for current image.
 *
 * @return              0 on success; nonzero on failure.
 */
static int
boot_add_shared_data(struct boot_loader_state *state,
                     uint8_t active_slot)
{
#if defined(MCUBOOT_MEASURED_BOOT) || defined(MCUBOOT_DATA_SHARING)
    int rc;

#ifdef MCUBOOT_MEASURED_BOOT
    rc = boot_save_boot_status(BOOT_CURR_IMG(state),
                                boot_img_hdr(state, active_slot),
                                BOOT_IMG_AREA(state, active_slot));
    if (rc != 0) {
        BOOT_LOG_ERR("Failed to add image data to shared area");
        return rc;
    }
#endif /* MCUBOOT_MEASURED_BOOT */

#ifdef MCUBOOT_DATA_SHARING
    rc = boot_save_shared_data(boot_img_hdr(state, active_slot),
                                BOOT_IMG_AREA(state, active_slot),
                                active_slot, image_max_sizes);
    if (rc != 0) {
        BOOT_LOG_ERR("Failed to add data to shared memory area.");
        return rc;
    }
#endif /* MCUBOOT_DATA_SHARING */

    return 0;

#else /* MCUBOOT_MEASURED_BOOT || MCUBOOT_DATA_SHARING */
    (void) (state);
    (void) (active_slot);

    return 0;
#endif
}

/**
 * Fills rsp to indicate how booting should occur.
 *
 * @param  state        Boot loader status information.
 * @param  rsp          boot_rsp struct to fill.
 */
static void
fill_rsp(struct boot_loader_state *state, struct boot_rsp *rsp)
{
    uint32_t active_slot;

    /* Always boot from the first image. */
    BOOT_CURR_IMG(state) = 0;
    active_slot = state->slot_usage[BOOT_CURR_IMG(state)].active_slot;

    rsp->br_flash_dev_id = flash_area_get_device_id(BOOT_IMG_AREA(state, active_slot));
    rsp->br_image_off = boot_img_slot_off(state, active_slot);
    rsp->br_hdr = boot_img_hdr(state, active_slot);
}

/**
 * Compare image version numbers
 *
 * By default, the comparison does not take build number into account.
 * Enable MCUBOOT_VERSION_CMP_USE_BUILD_NUMBER to take the build number into account.
 *
 * @param ver1           Pointer to the first image version to compare.
 * @param ver2           Pointer to the second image version to compare.
 *
 * @retval -1           If ver1 is less than ver2.
 * @retval 0            If the image version numbers are equal.
 * @retval 1            If ver1 is greater than ver2.
 */
static int
boot_version_cmp(const struct image_version *ver1,
                 const struct image_version *ver2)
{
#if !defined(MCUBOOT_VERSION_CMP_USE_BUILD_NUMBER)
    BOOT_LOG_DBG("boot_version_cmp: ver1 %u.%u.%u vs ver2 %u.%u.%u",
                 (unsigned)ver1->iv_major, (unsigned)ver1->iv_minor,
                 (unsigned)ver1->iv_revision, (unsigned)ver2->iv_major,
                 (unsigned)ver2->iv_minor, (unsigned)ver2->iv_revision);
#else
    BOOT_LOG_DBG("boot_version_cmp: ver1 %u.%u.%u.%u vs ver2 %u.%u.%u.%u",
                 (unsigned)ver1->iv_major, (unsigned)ver1->iv_minor,
                 (unsigned)ver1->iv_revision, (unsigned)ver1->iv_build_num,
                 (unsigned)ver2->iv_major, (unsigned)ver2->iv_minor,
                 (unsigned)ver2->iv_revision, (unsigned)ver2->iv_build_num);
#endif

    if (ver1->iv_major > ver2->iv_major) {
        return 1;
    }
    if (ver1->iv_major < ver2->iv_major) {
        return -1;
    }
    /* The major version numbers are equal, continue comparison. */
    if (ver1->iv_minor > ver2->iv_minor) {
        return 1;
    }
    if (ver1->iv_minor < ver2->iv_minor) {
        return -1;
    }
    /* The minor version numbers are equal, continue comparison. */
    if (ver1->iv_revision > ver2->iv_revision) {
        return 1;
    }
    if (ver1->iv_revision < ver2->iv_revision) {
        return -1;
    }

#if defined(MCUBOOT_VERSION_CMP_USE_BUILD_NUMBER)
    /* The revisions are equal, continue comparison. */
    if (ver1->iv_build_num > ver2->iv_build_num) {
        return 1;
    }
    if (ver1->iv_build_num < ver2->iv_build_num) {
        return -1;
    }
#endif

    return 0;
}

/*
 * Validate image hash/signature and optionally the security counter in a slot.
 */
static fih_ret
boot_image_check(struct boot_loader_state *state, struct image_header *hdr,
                 const struct flash_area *fap, struct boot_status *bs)
{
    TARGET_STATIC uint8_t tmpbuf[BOOT_TMPBUF_SZ];
    FIH_DECLARE(fih_rc, FIH_FAILURE);

    (void)bs;

    for (int i = 1; i <= CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT; i++ ) {
#if CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1
        BOOT_LOG_DBG("Image validation attempt %d/%d", i,
                     CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT);
#endif /* CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1 */

        FIH_CALL(bootutil_img_validate, fih_rc, state, hdr, fap, tmpbuf, BOOT_TMPBUF_SZ,
                 NULL, 0, NULL);

        if (FIH_EQ(fih_rc, FIH_SUCCESS)) {
#if CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1
            BOOT_LOG_DBG("Image validation attempt %d/%d success", i,
                         CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT);
#endif /* CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1 */
            break;
        } else {
#if CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1
            BOOT_LOG_WRN("Image validation attempt %d/%d failure: %d", i,
                         CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT, fih_rc);
#endif /* CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1 */

            if (i < CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT) {
#if defined(CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_WAIT_MS)
#if CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1
                BOOT_LOG_DBG("Waiting %d ms before next attempt",
                             CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_WAIT_MS);
#endif /* CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_COUNT > 1 */
                k_busy_wait(CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_WAIT_MS * 1000);
#endif /* CONFIG_NCS_MCUBOOT_IMG_VALIDATE_ATTEMPT_WAIT_MS */
            }
        }
    }
    FIH_RET(fih_rc);
}

/*
 * Check that this is a valid header.  Valid means that the magic is
 * correct, and that the sizes/offsets are "sane".  Sane means that
 * there is no overflow on the arithmetic, and that the result fits
 * within the flash area we are in. Also check the flags in the image
 * and class the image as invalid if flags for encryption/compression
 * are present but these features are not enabled.
 */
static bool
boot_is_header_valid(const struct image_header *hdr, const struct flash_area *fap,
                     struct boot_loader_state *state)
{
    uint32_t size;

    (void)state;

    if (hdr->ih_magic != IMAGE_MAGIC) {
        return false;
    }

    if (!boot_u32_safe_add(&size, hdr->ih_img_size, hdr->ih_hdr_size)) {
        return false;
    }

#ifdef MCUBOOT_DECOMPRESS_IMAGES
    if (!MUST_DECOMPRESS(fap, BOOT_CURR_IMG(state), hdr)) {
#else
    if (1) {
#endif
        if (!boot_u32_safe_add(&size, size, hdr->ih_protect_tlv_size)) {
            return false;
        }
    }

    if (size >= flash_area_get_size(fap)) {
        return false;
    }

#if !defined(MCUBOOT_ENC_IMAGES)
    if (IS_ENCRYPTED(hdr)) {
        return false;
    }
#else
    if ((hdr->ih_flags & IMAGE_F_ENCRYPTED_AES128) &&
        (hdr->ih_flags & IMAGE_F_ENCRYPTED_AES256))
    {
        return false;
    }
#endif

#if !defined(MCUBOOT_DECOMPRESS_IMAGES)
    if (IS_COMPRESSED(hdr)) {
        return false;
    }
#else
    if (MUST_DECOMPRESS(fap, BOOT_CURR_IMG(state), hdr)) {
        if (!boot_is_compressed_header_valid(hdr, fap, state)) {
            return false;
        }
    }
#endif

    return true;
}

/*
 * Check that a memory area consists of a given value.
 */
static inline bool
boot_data_is_set_to(uint8_t val, void *data, size_t len)
{
    uint8_t i;
    uint8_t *p = (uint8_t *)data;

    for (i = 0; i < len; i++) {
        if (val != p[i]) {
            return false;
        }
    }
    return true;
}

static int
boot_check_header_erased(struct boot_loader_state *state, int slot)
{
    const struct flash_area *fap = NULL;
    struct image_header *hdr;
    uint8_t erased_val;

    fap = BOOT_IMG_AREA(state, slot);
    assert(fap != NULL);

    erased_val = flash_area_erased_val(fap);

    hdr = boot_img_hdr(state, slot);
    if (!boot_data_is_set_to(erased_val, &hdr->ih_magic, sizeof(hdr->ih_magic))) {
        return -1;
    }

    return 0;
}

#if defined(MCUBOOT_DIRECT_XIP)
/**
 * Check if image in slot has been set with specific ROM address to run from
 * and whether the slot starts at that address.
 *
 * @returns 0 if IMAGE_F_ROM_FIXED flag is not set;
 *          0 if IMAGE_F_ROM_FIXED flag is set and ROM address specified in
 *            header matches the slot address;
 *          1 if IMF_F_ROM_FIXED flag is set but ROM address specified in header
 *          does not match the slot address.
 */
static bool
boot_rom_address_check(struct boot_loader_state *state)
{
    uint32_t active_slot;
    const struct image_header *hdr;
    uint32_t f_off;

    active_slot = state->slot_usage[BOOT_CURR_IMG(state)].active_slot;
    hdr = boot_img_hdr(state, active_slot);
    f_off = boot_img_slot_off(state, active_slot);

    if (hdr->ih_flags & IMAGE_F_ROM_FIXED && hdr->ih_load_addr != f_off) {
        BOOT_LOG_WRN("Image in %s slot at 0x%x has been built for offset 0x%x"\
                     ", skipping",
                     active_slot == 0 ? "primary" : "secondary", f_off,
                     hdr->ih_load_addr);

        /* The image is not bootable from this slot. */
        return 1;
    }

    return 0;
}
#endif

/*
 * Check that there is a valid image in a slot
 *
 * @returns
 *         FIH_SUCCESS                      if image was successfully validated
 *         FIH_NO_BOOTABLE_IMAGE            if no bootloable image was found
 *         FIH_FAILURE                      on any errors
 */
static fih_ret
boot_validate_slot(struct boot_loader_state *state, int slot,
                   struct boot_status *bs, int expected_swap_type)
{
    const struct flash_area *fap;
    struct image_header *hdr;
    FIH_DECLARE(fih_rc, FIH_FAILURE);

    BOOT_LOG_DBG("boot_validate_slot: slot %d, expected_swap_type %d",
                 slot, expected_swap_type);
    (void)expected_swap_type;

    fap = BOOT_IMG_AREA(state, slot);
    assert(fap != NULL);

    hdr = boot_img_hdr(state, slot);
    if (boot_check_header_erased(state, slot) == 0 ||
        (hdr->ih_flags & IMAGE_F_NON_BOOTABLE)) {
        /* No bootable image in slot; continue booting from the primary slot. */
        fih_rc = FIH_NO_BOOTABLE_IMAGE;
        goto out;
    }

    if (!boot_is_header_valid(hdr, fap, state)) {
        fih_rc = FIH_FAILURE;
    } else {
        BOOT_HOOK_CALL_FIH(boot_image_check_hook, FIH_BOOT_HOOK_REGULAR,
                           fih_rc, BOOT_CURR_IMG(state), slot);
        if (FIH_EQ(fih_rc, FIH_BOOT_HOOK_REGULAR)) {
            FIH_CALL(boot_image_check, fih_rc, state, hdr, fap, bs);
        }
    }

    if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
        if ((slot != BOOT_SLOT_PRIMARY) || ARE_SLOTS_EQUIVALENT()) {
            boot_scramble_slot(fap, slot);
            /* Image is invalid, erase it to prevent further unnecessary
             * attempts to validate and boot it.
             */
        }

#if !defined(__BOOTSIM__)
        BOOT_LOG_ERR("Image in the %s slot is not valid!",
                     (slot == BOOT_SLOT_PRIMARY) ? "primary" : "secondary");
#endif
        fih_rc = FIH_NO_BOOTABLE_IMAGE;
        goto out;
    }

out:
    FIH_RET(fih_rc);
}

#ifdef MCUBOOT_HW_ROLLBACK_PROT
/**
 * Updates the stored security counter value with the image's security counter
 * value which resides in the given slot, only if it's greater than the stored
 * value.
 *
 * @param state         Boot state where the current image's security counter will
 *                      be updated.
 * @param slot          Slot number of the image.
 * @param hdr_slot_idx  Index of the header in the state current image variable
 *                      containing the pointer to the image header structure of the
 *                      image that is currently stored in the given slot.
 *
 * @return              0 on success; nonzero on failure.
 */
static int
boot_update_security_counter(struct boot_loader_state *state, int slot, int hdr_slot_idx)
{
    const struct flash_area *fap = NULL;
    uint32_t img_security_cnt;
    int rc;

    fap = BOOT_IMG_AREA(state, slot);
    assert(fap != NULL);

    rc = bootutil_get_img_security_cnt(state, hdr_slot_idx, fap, &img_security_cnt);
    if (rc != 0) {
        goto done;
    }

    rc = boot_nv_security_counter_update(BOOT_CURR_IMG(state), img_security_cnt);
    if (rc != 0) {
        goto done;
    }

done:
    return rc;
}
#endif /* MCUBOOT_HW_ROLLBACK_PROT */


/**
 * Removes data from specified region either by writing erase value in place of
 * data or by doing erase, if device has such hardware requirement.
 * Note that function will fail if off or size are not aligned to device
 * write block size or erase block size.
 *
 * @param fa                    The flash_area containing the region to erase.
 * @param off                   The offset within the flash area to start the
 *                              erase.
 * @param size                  The number of bytes to erase.
 * @param backwards             If set to true will erase from end to start
 *                              addresses, otherwise erases from start to end
 *                              addresses.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
boot_scramble_region(const struct flash_area *fa, uint32_t off, uint32_t size, bool backwards)
{
    int rc = 0;

    BOOT_LOG_DBG("boot_scramble_region: %p %d %d %d", fa, off, size, (int)backwards);

    if (size == 0) {
        goto done;
    }

    if (device_requires_erase(fa)) {
        rc = boot_erase_region(fa, off, size, backwards);
    } else if (off >= flash_area_get_size(fa) || (flash_area_get_size(fa) - off) < size) {
        rc = -1;
        goto done;
    } else {
        uint8_t buf[BOOT_MAX_ALIGN];
        const size_t write_block = flash_area_align(fa);
        uint32_t end_offset;

        BOOT_LOG_DBG("boot_scramble_region: device without erase, overwriting");
        memset(buf, flash_area_erased_val(fa), sizeof(buf));

        if (backwards) {
            end_offset = ALIGN_DOWN(off, write_block);
            /* Starting at the last write block in range */
            off += size - write_block;
        } else {
            end_offset = ALIGN_DOWN((off + size), write_block);
        }
        BOOT_LOG_DBG("boot_scramble_region: start offset %u, end offset %u", off, end_offset);

        while (off != end_offset) {
            /* Write over the area to scramble data that is there */
            rc = flash_area_write(fa, off, buf, write_block);
            if (rc != 0) {
                BOOT_LOG_DBG("boot_scramble_region: error %d for %p %d %u",
                             rc, fa, off, (unsigned int)write_block);
                break;
            }

            MCUBOOT_WATCHDOG_FEED();

            if (backwards) {
                if (end_offset >= off) {
                    /* Reached the first offset in range and already scrambled it */
                    break;
                }

                off -= write_block;
            } else {
                off += write_block;

                if (end_offset <= off) {
                    /* Reached the end offset in range and already scrambled it */
                    break;
                }
            }
        }
    }

done:
    return rc;
}

/**
 * Removes data from specified region backwards either by writing erase_value
 * in place of data or by doing erase, if device has such hardware requirement.
 * Note that function will fail if off or size are not aligned to device
 * write block size or erase block size.
 *
 * @param fa                    The flash_area containing the region to erase.
 * @param off                   The offset within the flash area to start the
 *                              erase.
 * @param size                  The number of bytes to erase.
 *
 * @return                      0 on success; nonzero on failure.
 */

/**
 * Remove enough data from slot to mark is as unused
 * Assumption: header and trailer are not overlapping on write block or
 * erase block, if device has erase requirement.
 * Note that this function is intended for removing data not preparing device
 * for write.
 *
 * @param fa        Pointer to flash area object for slot
 * @param slot      Slot the @p fa represents
 *
 * @return          0 on success; nonzero on failure.
 */
int
boot_scramble_slot(const struct flash_area *fa, int slot)
{
    size_t size;
    int ret = 0;

    (void)slot;

    /* Without minimal entire area needs to be scrambled */
#if !defined(MCUBOOT_MINIMAL_SCRAMBLE)
    size = flash_area_get_size(fa);
    ret = boot_scramble_region(fa, 0, size, false);
#else
    size_t off = 0;

    ret = boot_header_scramble_off_sz(fa, slot, &off, &size);
    if (ret < 0) {
        return ret;
    }

    ret = boot_scramble_region(fa, off, size, false);
    if (ret < 0) {
        return ret;
    }

    ret = boot_trailer_scramble_offset(fa, 0, &off);
    if (ret < 0) {
        return ret;
    }

    ret = boot_scramble_region(fa, off, (flash_area_get_size(fa) - off), true);
#endif
    return ret;
}

/**
 * Opens all flash areas and checks which contain an image with a valid header.
 *
 * @param  state        Boot loader status information.
 *
 * @return              0 on success; nonzero on failure.
 */
static int
boot_get_slot_usage(struct boot_loader_state *state)
{
    uint32_t slot;
    int rc;
    struct image_header *hdr = NULL;

    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        /* Attempt to read an image header from each slot. */
        rc = boot_read_image_headers(state, false, NULL);
        if (rc != 0) {
            BOOT_LOG_WRN("Failed reading image headers.");
            return rc;
        }

        /* Check headers in all slots */
        for (slot = 0; slot < BOOT_NUM_SLOTS; slot++) {
            hdr = boot_img_hdr(state, slot);

            if (boot_is_header_valid(hdr, BOOT_IMG_AREA(state, slot), state)) {
                state->slot_usage[BOOT_CURR_IMG(state)].slot_available[slot] = true;
                BOOT_LOG_IMAGE_INFO(slot, hdr);
            } else {
                state->slot_usage[BOOT_CURR_IMG(state)].slot_available[slot] = false;
                BOOT_LOG_INF("Image %d %s slot: Image not found",
                             BOOT_CURR_IMG(state),
                             (slot == BOOT_SLOT_PRIMARY)
                             ? "Primary" : "Secondary");
            }
        }

        state->slot_usage[BOOT_CURR_IMG(state)].active_slot = BOOT_SLOT_NONE;
    }

    return 0;
}

/**
 * Finds the slot containing the image with the highest version number for the
 * current image.
 *
 * @param  state        Boot loader status information.
 *
 * @return              BOOT_SLOT_NONE if no available slot found, number of
 *                      the found slot otherwise.
 */
static uint32_t
find_slot_with_highest_version(struct boot_loader_state *state)
{
    uint32_t slot;
    uint32_t candidate_slot = BOOT_SLOT_NONE;
    int rc;

    for (slot = 0; slot < BOOT_NUM_SLOTS; slot++) {
        if (state->slot_usage[BOOT_CURR_IMG(state)].slot_available[slot]) {
            if (candidate_slot == BOOT_SLOT_NONE) {
                candidate_slot = slot;
            } else {
                rc = boot_version_cmp(
                            &boot_img_hdr(state, slot)->ih_ver,
                            &boot_img_hdr(state, candidate_slot)->ih_ver);
                if (rc == 1) {
                    /* The version of the image being examined is greater than
                     * the version of the current candidate.
                     */
                    candidate_slot = slot;
                }
            }
        }
    }

    return candidate_slot;
}

#ifdef MCUBOOT_HAVE_LOGGING
/**
 * Prints the state of the loaded images.
 *
 * @param  state        Boot loader status information.
 */
static void
print_loaded_images(struct boot_loader_state *state)
{
    uint32_t active_slot;

    (void)state;

    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        active_slot = state->slot_usage[BOOT_CURR_IMG(state)].active_slot;

        BOOT_LOG_INF("Image %d loaded from the %s slot",
                     BOOT_CURR_IMG(state),
                     (active_slot == BOOT_SLOT_PRIMARY) ?
                     "primary" : "secondary");
    }
}
#endif

#if (defined(MCUBOOT_DIRECT_XIP) && defined(MCUBOOT_DIRECT_XIP_REVERT)) || \
    (defined(MCUBOOT_RAM_LOAD) && defined(MCUBOOT_RAM_LOAD_REVERT))
/**
 * Checks whether the active slot of the current image was previously selected
 * to run. Erases the image if it was selected but its execution failed,
 * otherwise marks it as selected if it has not been before.
 *
 * @param  state        Boot loader status information.
 *
 * @return              0 on success; nonzero on failure.
 */
static int
boot_select_or_erase(struct boot_loader_state *state)
{
    const struct flash_area *fap = NULL;
    int rc;
    uint32_t active_slot;
    struct boot_swap_state* active_swap_state;

    active_slot = state->slot_usage[BOOT_CURR_IMG(state)].active_slot;

    fap = BOOT_IMG_AREA(state, active_slot);
    assert(fap != NULL);

    active_swap_state = &(state->slot_usage[BOOT_CURR_IMG(state)].swap_state);

    memset(active_swap_state, 0, sizeof(struct boot_swap_state));
    rc = boot_read_swap_state(fap, active_swap_state);
    assert(rc == 0);

    if (active_swap_state->magic != BOOT_MAGIC_GOOD ||
        (active_swap_state->copy_done == BOOT_FLAG_SET &&
         active_swap_state->image_ok  != BOOT_FLAG_SET)) {
        /*
         * A reboot happened without the image being confirmed at
         * runtime or its trailer is corrupted/invalid. Erase the image
         * to prevent it from being selected again on the next reboot.
         */
        BOOT_LOG_DBG("Erasing faulty image in the %s slot.",
                     (active_slot == BOOT_SLOT_PRIMARY) ? "primary" : "secondary");
        rc = boot_scramble_region(fap, 0, flash_area_get_size(fap), false);
        assert(rc == 0);
        rc = -1;
    } else {
        if (active_swap_state->copy_done != BOOT_FLAG_SET) {
            if (active_swap_state->copy_done == BOOT_FLAG_BAD) {
                BOOT_LOG_DBG("The copy_done flag had an unexpected value. Its "
                             "value was neither 'set' nor 'unset', but 'bad'.");
            }
            /*
             * Set the copy_done flag, indicating that the image has been
             * selected to boot. It can be set in advance, before even
             * validating the image, because in case the validation fails, the
             * entire image slot will be erased (including the trailer).
             */
            rc = boot_write_copy_done(fap);
            if (rc != 0) {
                BOOT_LOG_WRN("Failed to set copy_done flag of the image in "
                             "the %s slot.", (active_slot == BOOT_SLOT_PRIMARY) ?
                             "primary" : "secondary");
                rc = 0;
            }
        }
    }

    return rc;
}
#endif /* MCUBOOT_DIRECT_XIP && MCUBOOT_DIRECT_XIP_REVERT ||
          MCUBOOT_RAM_LOAD && MCUBOOT_RAM_LOAD_REVERT */

/**
 * Tries to load and validate a single slot.
 *
 * @param  state        Boot loader status information.
 *
 * @return              0 on success; nonzero on failure.
 */
static fih_ret
boot_load_and_validate_current_image(struct boot_loader_state *state)
{
    int rc;
    fih_ret fih_rc;
    uint32_t active_slot = state->slot_usage[BOOT_CURR_IMG(state)].active_slot;

    if (active_slot == BOOT_SLOT_NONE) {
        FIH_RET(FIH_FAILURE);
    }

    BOOT_LOG_INF("Loading image %d from slot %d", BOOT_CURR_IMG(state), active_slot);

#ifdef MCUBOOT_DIRECT_XIP
    rc = boot_rom_address_check(state);
    if (rc != 0) {
        FIH_RET(FIH_FAILURE);
    }
#endif /* MCUBOOT_DIRECT_XIP */

#if defined(MCUBOOT_DIRECT_XIP_REVERT) || defined(MCUBOOT_RAM_LOAD_REVERT)
    rc = boot_select_or_erase(state);
    if (rc != 0) {
        FIH_RET(FIH_FAILURE);
    }
#endif /* MCUBOOT_DIRECT_XIP_REVERT || MCUBOOT_RAM_LOAD_REVERT */

#ifdef MCUBOOT_RAM_LOAD
    /* Image is first loaded to RAM and authenticated there in order to
     * prevent TOCTOU attack during image copy. This could be applied
     * when loading images from external (untrusted) flash to internal
     * (trusted) RAM and image is authenticated before copying.
     */
    rc = boot_load_image_to_sram(state);
    if (rc != 0) {
        /* Image cannot be ramloaded. */
        boot_remove_image_from_flash(state, active_slot);
        FIH_RET(FIH_FAILURE);
    }
#endif /* MCUBOOT_RAM_LOAD */

    FIH_CALL(boot_validate_slot, fih_rc, state, active_slot, NULL, 0);
    if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
        /* Image is invalid. */
#ifdef MCUBOOT_RAM_LOAD
        boot_remove_image_from_sram(state);
#endif /* MCUBOOT_RAM_LOAD */
        FIH_RET(FIH_FAILURE);
    }

    FIH_RET(FIH_SUCCESS);
}

/**
 * Tries to load a slot for all the images with validation.
 *
 * @param  state        Boot loader status information.
 *
 * @return              0 on success; nonzero on failure.
 */
fih_ret
boot_load_and_validate_images(struct boot_loader_state *state)
{
    uint32_t active_slot;
    int rc;
    fih_ret fih_rc;

    while (true) {
#if (BOOT_IMAGE_NUMBER > 1)
        BOOT_CURR_IMG(state) = MCUBOOT_MANIFEST_IMAGE_NUMBER;
#endif
        rc = BOOT_HOOK_FIND_SLOT_CALL(boot_find_next_slot_hook, BOOT_HOOK_REGULAR,
                                      state, BOOT_CURR_IMG(state), &active_slot);
        if (rc == BOOT_HOOK_REGULAR) {
            active_slot = find_slot_with_highest_version(state);
        }
        if (active_slot == BOOT_SLOT_NONE) {
            BOOT_LOG_INF("No more manifest slots available");
            FIH_RET(FIH_FAILURE);
        }

        /* Save the number of the active manifest slot. */
        state->slot_usage[BOOT_CURR_IMG(state)].active_slot = active_slot;

        FIH_CALL(boot_load_and_validate_current_image, fih_rc, state);
        if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
            state->slot_usage[MCUBOOT_MANIFEST_IMAGE_NUMBER].slot_available[active_slot] = false;
            state->slot_usage[MCUBOOT_MANIFEST_IMAGE_NUMBER].active_slot = BOOT_SLOT_NONE;
            BOOT_LOG_INF("No valid manifest in slot %d", active_slot);
            continue;
        }

        BOOT_LOG_INF("Try to validate images using manifest in slot %d", active_slot);

#if BOOT_IMAGE_NUMBER > 1
        /* Go over all other images and try to load one */
        IMAGES_ITER(BOOT_CURR_IMG(state)) {
            /* Skip the image with manifest - it's been already verified. */
            if (BOOT_CURR_IMG(state) == MCUBOOT_MANIFEST_IMAGE_NUMBER) {
                continue;
            }

            /* Check if there is a matching slot available. */
            if (!state->slot_usage[BOOT_CURR_IMG(state)].slot_available[active_slot]) {
                /* Invalidate manifest */
                FIH_SET(fih_rc, FIH_FAILURE);
                break;
            }

            /* Save the number of the active slot. */
            state->slot_usage[BOOT_CURR_IMG(state)].active_slot = active_slot;

            FIH_CALL(boot_load_and_validate_current_image, fih_rc, state);
            if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
                state->slot_usage[BOOT_CURR_IMG(state)].slot_available[active_slot] = false;
                state->slot_usage[BOOT_CURR_IMG(state)].active_slot = BOOT_SLOT_NONE;
                /* Invalidate manifest */
                break;
            }
        }
#endif

        if (FIH_EQ(fih_rc, FIH_SUCCESS)) {
            /* All images have been loaded and validated successfully. */
            break;
        }

        BOOT_LOG_DBG("Manifest in slot %d is invalid", active_slot);

        /* Invalidate manifest */
        state->slot_usage[MCUBOOT_MANIFEST_IMAGE_NUMBER].slot_available[active_slot] = false;
        state->slot_usage[MCUBOOT_MANIFEST_IMAGE_NUMBER].active_slot = BOOT_SLOT_NONE;
#ifdef MCUBOOT_RAM_LOAD
        BOOT_CURR_IMG(state) = MCUBOOT_MANIFEST_IMAGE_NUMBER;
        boot_remove_image_from_sram(state);
#endif /* MCUBOOT_RAM_LOAD */
    }

    FIH_RET(FIH_SUCCESS);
}

/**
 * Updates the security counter for the current image.
 *
 * @param  state        Boot loader status information.
 *
 * @return              0 on success; nonzero on failure.
 */
static int
boot_update_hw_rollback_protection(struct boot_loader_state *state)
{
#ifdef MCUBOOT_HW_ROLLBACK_PROT
    int rc;

    /* Update the stored security counter with the newer (active) image's
     * security counter value.
     */
#if (defined(MCUBOOT_DIRECT_XIP) && defined(MCUBOOT_DIRECT_XIP_REVERT)) || \
    (defined(MCUBOOT_RAM_LOAD) && defined(MCUBOOT_RAM_LOAD_REVERT))
    /* When the 'revert' mechanism is enabled in direct-xip or RAM load mode,
     * the security counter can be increased only after reboot, if the image
     * has been confirmed at runtime (the image_ok flag has been set).
     * This way a 'revert' can be performed when it's necessary.
     */
    if (state->slot_usage[BOOT_CURR_IMG(state)].swap_state.image_ok == BOOT_FLAG_SET) {
#endif
        rc = boot_update_security_counter(state,
                                          state->slot_usage[BOOT_CURR_IMG(state)].active_slot,
                                          state->slot_usage[BOOT_CURR_IMG(state)].active_slot);
        if (rc != 0) {
            BOOT_LOG_ERR("Security counter update failed after image %d validation.", BOOT_CURR_IMG(state));
            return rc;
        }
#if (defined(MCUBOOT_DIRECT_XIP) && defined(MCUBOOT_DIRECT_XIP_REVERT)) || \
    (defined(MCUBOOT_RAM_LOAD) && defined(MCUBOOT_RAM_LOAD_REVERT))
    }
#endif

    return 0;

#else /* MCUBOOT_HW_ROLLBACK_PROT */
    (void) (state);
    return 0;
#endif
}

fih_ret
context_boot_go(struct boot_loader_state *state, struct boot_rsp *rsp)
{
    int rc;
    FIH_DECLARE(fih_rc, FIH_FAILURE);

    rc = boot_open_all_flash_areas(state);
    if (rc != 0) {
        goto out;
    }

    rc = boot_get_slot_usage(state);
    if (rc != 0) {
        goto close;
    }

    FIH_CALL(boot_load_and_validate_images, fih_rc, state);
    if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
        FIH_SET(fih_rc, FIH_FAILURE);
        goto close;
    }

    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        rc = boot_update_hw_rollback_protection(state);
        if (rc != 0) {
            FIH_SET(fih_rc, FIH_FAILURE);
            goto close;
        }

        rc = boot_add_shared_data(state, (uint8_t)state->slot_usage[BOOT_CURR_IMG(state)].active_slot);
        if (rc != 0) {
            FIH_SET(fih_rc, FIH_FAILURE);
            goto close;
        }
    }

#ifdef CONFIG_SOC_EARLY_RESET_HOOK
    /* Designate the slot to be used by the PM_S2RAM resume module */
    s2ram_designate_slot((uint8_t)state->slot_usage[0].active_slot);
#endif

    /* All image loaded successfully. */
#ifdef MCUBOOT_HAVE_LOGGING
    print_loaded_images(state);
#endif

    fill_rsp(state, rsp);

close:
    boot_close_all_flash_areas(state);

out:
    if (rc != 0) {
        FIH_SET(fih_rc, FIH_FAILURE);
    }

    FIH_RET(fih_rc);
}

/**
 * Prepares the booting process. This function moves images around in flash as
 * appropriate, and tells you what address to boot from.
 *
 * @param rsp                   On success, indicates how booting should occur.
 *
 * @return                      FIH_SUCCESS on success; nonzero on failure.
 */
fih_ret
boot_go(struct boot_rsp *rsp)
{
    FIH_DECLARE(fih_rc, FIH_FAILURE);

    boot_state_clear(NULL);

    FIH_CALL(context_boot_go, fih_rc, &boot_data, rsp);
    FIH_RET(fih_rc);
}

int
boot_open_all_flash_areas(struct boot_loader_state *state)
{
    size_t slot;
    int rc = 0;
    int fa_id;
    int image_index;

    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        image_index = BOOT_CURR_IMG(state);

        for (slot = 0; slot < BOOT_NUM_SLOTS; slot++) {
            fa_id = flash_area_id_from_multi_image_slot(image_index, slot);
            rc = flash_area_open(fa_id, &BOOT_IMG_AREA(state, slot));
            assert(rc == 0);

            if (rc != 0) {
                BOOT_LOG_ERR("Failed to open flash area ID %d (image %d slot %zu): %d",
                             fa_id, image_index, slot, rc);
                goto out;
            }
        }
    }

out:
    if (rc != 0) {
        boot_close_all_flash_areas(state);
    }

    return rc;
}

void
boot_close_all_flash_areas(struct boot_loader_state *state)
{
    uint32_t slot;

    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        for (slot = 0; slot < BOOT_NUM_SLOTS; slot++) {
            if (BOOT_IMG_AREA(state, BOOT_NUM_SLOTS - 1 - slot) != NULL) {
                flash_area_close(BOOT_IMG_AREA(state, BOOT_NUM_SLOTS - 1 - slot));
            }
        }
    }
}

#if defined(MCUBOOT_SERIAL_IMG_GRP_SLOT_INFO) || defined(MCUBOOT_DATA_SHARING)
/**
 * Fetches the maximum allowed size of the image
 */
const struct image_max_size *boot_get_max_app_size(void)
{
#if defined(MCUBOOT_SERIAL_IMG_GRP_SLOT_INFO)
    uint8_t i = 0;

    while (i < BOOT_IMAGE_NUMBER) {
        if (image_max_sizes[i].calculated == true) {
            break;
        }

        ++i;
    }

    if (i == BOOT_IMAGE_NUMBER) {
        /* Information not available, need to fetch it */
        boot_fetch_slot_state_sizes();
    }
#endif

    return image_max_sizes;
}
#endif

#endif /* MCUBOOT_MANIFEST_UPDATES */

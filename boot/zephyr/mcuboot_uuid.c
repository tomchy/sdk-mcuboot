/*
 *  Copyright (c) 2025 Nordic Semiconductor ASA
 *
 *  SPDX-License-Identifier: Apache-2.0
 */
#include <bootutil/mcuboot_uuid.h>

fih_ret boot_uuid_init(void)
{
        FIH_RET(FIH_SUCCESS);
}

#ifdef CONFIG_MCUBOOT_UUID_VID
static const struct image_uuid uuid_vid_c = {{
        MCUBOOT_UUIC_VID_VALUE
}};

fih_ret boot_image_should_have_uuid_vid(uint32_t image_index)
{
        FIH_RET(FIH_SUCCESS);
}

fih_ret boot_uuid_vid_get(uint32_t image_id, const struct image_uuid **uuid_vid)
{
        if (uuid_vid != NULL) {
                *uuid_vid = &uuid_vid_c;
                FIH_RET(FIH_SUCCESS);
        }

        FIH_RET(FIH_FAILURE);
}
#endif /* CONFIG_MCUBOOT_UUID_VID */

#ifdef CONFIG_MCUBOOT_UUID_CID
#ifdef MCUBOOT_UUIC_CID_IMAGE_0_VALUE
static const struct image_uuid uuid_image_0_cid_c = {{
        MCUBOOT_UUIC_CID_IMAGE_0_VALUE
}};
#endif /* MCUBOOT_UUIC_CID_IMAGE_0_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_1_VALUE
static const struct image_uuid uuid_image_1_cid_c = {{
        MCUBOOT_UUIC_CID_IMAGE_1_VALUE
}};
#endif /* MCUBOOT_UUIC_CID_IMAGE_1_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_2_VALUE
static const struct image_uuid uuid_image_2_cid_c = {{
        MCUBOOT_UUIC_CID_IMAGE_2_VALUE
}};
#endif /* MCUBOOT_UUIC_CID_IMAGE_2_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_3_VALUE
static const struct image_uuid uuid_image_3_cid_c = {{
        MCUBOOT_UUIC_CID_IMAGE_3_VALUE
}};
#endif /* MCUBOOT_UUIC_CID_IMAGE_3_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_4_VALUE
static const struct image_uuid uuid_image_4_cid_c = {{
        MCUBOOT_UUIC_CID_IMAGE_4_VALUE
}};
#endif /* MCUBOOT_UUIC_CID_IMAGE_4_VALUE */

fih_ret boot_image_should_have_uuid_cid(uint32_t image_index)
{
        FIH_RET(FIH_SUCCESS);
}

fih_ret boot_uuid_cid_get(uint32_t image_id, const struct image_uuid **uuid_cid)
{
        if (uuid_cid != NULL) {
#ifdef MCUBOOT_UUIC_CID_IMAGE_0_VALUE
                if (image_id == 0) {
                        *uuid_cid = &uuid_image_0_cid_c;
                        FIH_RET(FIH_SUCCESS);
                }
#endif /* MCUBOOT_UUIC_CID_IMAGE_0_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_1_VALUE
                if (image_id == 1) {
                        *uuid_cid = &uuid_image_1_cid_c;
                        FIH_RET(FIH_SUCCESS);
                }
#endif /* MCUBOOT_UUIC_CID_IMAGE_1_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_2_VALUE
                if (image_id == 2) {
                        *uuid_cid = &uuid_image_2_cid_c;
                        FIH_RET(FIH_SUCCESS);
                }
#endif /* MCUBOOT_UUIC_CID_IMAGE_2_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_3_VALUE
                if (image_id == 3) {
                        *uuid_cid = &uuid_image_3_cid_c;
                        FIH_RET(FIH_SUCCESS);
                }
#endif /* MCUBOOT_UUIC_CID_IMAGE_3_VALUE */
#ifdef MCUBOOT_UUIC_CID_IMAGE_4_VALUE
                if (image_id == 4) {
                        *uuid_cid = &uuid_image_4_cid_c;
                        FIH_RET(FIH_SUCCESS);
                }
#endif /* MCUBOOT_UUIC_CID_IMAGE_4_VALUE */
        }

        FIH_RET(FIH_FAILURE);
}
#endif /* CONFIG_MCUBOOT_UUID_CID */

fih_ret boot_uuid_compare(const struct image_uuid *uuid1, const struct image_uuid *uuid2)
{
        return fih_ret_encode_zero_equality(memcmp(uuid1->raw, uuid2->raw,
                                            ARRAY_SIZE(uuid1->raw)));
}

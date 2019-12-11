/*
 * Copyright (C) 2013-2019 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#ifndef MERCURY_PROC_STRING_H
#define MERCURY_PROC_STRING_H

#include "mercury_proc.h"
#include "mercury_string_object.h"

#include <string.h>

typedef const char *hg_const_string_t;
typedef char *hg_string_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inline prototypes (do not remove)
 */
static HG_INLINE hg_return_t hg_proc_hg_const_string_t(
        hg_proc_t proc, void *data);
static HG_INLINE hg_return_t hg_proc_hg_string_t(
        hg_proc_t proc, void *data);
static HG_INLINE hg_return_t hg_proc_hg_string_object_t(
        hg_proc_t proc, void *data);

/**
 * Generic processing routine.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param data [IN/OUT]         pointer to data
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
static HG_INLINE hg_return_t
hg_proc_hg_const_string_t(hg_proc_t proc, void *data)
{
    hg_string_object_t string;
    hg_const_string_t *strdata = (hg_const_string_t *) data;
    hg_return_t ret = HG_SUCCESS;

    switch (hg_proc_get_op(proc)) {
        case HG_ENCODE:
            hg_string_object_init_const_char(&string, *strdata, 0);
            ret = hg_proc_hg_string_object_t(proc, &string);
            if (ret != HG_SUCCESS)
                goto done;
            hg_string_object_free(&string);
            break;
        case HG_DECODE:
            ret = hg_proc_hg_string_object_t(proc, &string);
            if (ret != HG_SUCCESS)
                goto done;
            *strdata = hg_string_object_swap(&string, 0);
            hg_string_object_free(&string);
            break;
        case HG_FREE:
            hg_string_object_init_const_char(&string, *strdata, 1);
            ret = hg_proc_hg_string_object_t(proc, &string);
            if (ret != HG_SUCCESS)
                goto done;
            break;
        default:
            break;
    }

done:
    return ret;
}

/**
 * Generic processing routine.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param data [IN/OUT]         pointer to data
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
static HG_INLINE hg_return_t
hg_proc_hg_string_t(hg_proc_t proc, void *data)
{
    hg_string_object_t string;
    hg_string_t *strdata = (hg_string_t *) data;
    hg_return_t ret = HG_SUCCESS;

    switch (hg_proc_get_op(proc)) {
        case HG_ENCODE:
            hg_string_object_init_char(&string, *strdata, 0);
            ret = hg_proc_hg_string_object_t(proc, &string);
            if (ret != HG_SUCCESS)
                goto done;
            hg_string_object_free(&string);
            break;
        case HG_DECODE:
            ret = hg_proc_hg_string_object_t(proc, &string);
            if (ret != HG_SUCCESS)
                goto done;
            *strdata = hg_string_object_swap(&string, 0);
            hg_string_object_free(&string);
            break;
        case HG_FREE:
            hg_string_object_init_char(&string, *strdata, 1);
            ret = hg_proc_hg_string_object_t(proc, &string);
            if (ret != HG_SUCCESS)
                goto done;
            break;
        default:
            break;
    }

done:
    return ret;
}

/**
 * Generic processing routine.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param string [IN/OUT]       pointer to string
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
static HG_INLINE hg_return_t
hg_proc_hg_string_object_t(hg_proc_t proc, void *string)
{
    hg_uint64_t string_len = 0;
    hg_return_t ret = HG_SUCCESS;
    hg_string_object_t *strobj = (hg_string_object_t *) string;

    switch (hg_proc_get_op(proc)) {
        case HG_ENCODE:
            string_len = (strobj->data) ? strlen(strobj->data) + 1 : 0;
            ret = hg_proc_uint64_t(proc, &string_len);
            if (ret != HG_SUCCESS)
                goto done;
            if (string_len) {
                ret = hg_proc_raw(proc, strobj->data, string_len);
                if (ret != HG_SUCCESS)
                    goto done;
                ret = hg_proc_hg_uint8_t(proc, (hg_uint8_t *) &strobj->is_const);
                if (ret != HG_SUCCESS)
                    goto done;
                ret = hg_proc_hg_uint8_t(proc, (hg_uint8_t *) &strobj->is_owned);
                if (ret != HG_SUCCESS)
                    goto done;
            }
            break;
        case HG_DECODE:
            ret = hg_proc_uint64_t(proc, &string_len);
            if (ret != HG_SUCCESS)
                goto done;
            if (string_len) {
                strobj->data = (char *) malloc(string_len);
                if (strobj->data == NULL) {
                    ret = HG_NOMEM;
                    goto done;
                }
                ret = hg_proc_raw(proc, strobj->data, string_len);
                if (ret != HG_SUCCESS) {
                    free(strobj->data);
                    strobj->data = NULL;
                    goto done;
                }
                ret = hg_proc_hg_uint8_t(proc, (hg_uint8_t *) &strobj->is_const);
                if (ret != HG_SUCCESS) {
                    free(strobj->data);
                    strobj->data = NULL;
                    goto done;
                }
                ret = hg_proc_hg_uint8_t(proc, (hg_uint8_t *) &strobj->is_owned);
                if (ret != HG_SUCCESS) {
                    free(strobj->data);
                    strobj->data = NULL;
                    goto done;
                }
            } else
                strobj->data = NULL;
            break;
        case HG_FREE:
            ret = hg_string_object_free(strobj);
            if (ret != HG_SUCCESS)
                goto done;
            break;
        default:
            break;
    }

done:
    return ret;
}

#ifdef __cplusplus
}
#endif

#endif /* MERCURY_PROC_STRING_H */

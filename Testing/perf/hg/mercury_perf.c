/**
 * Copyright (c) 2013-2022 UChicago Argonne, LLC and The HDF Group.
 * Copyright (c) 2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mercury_perf.h"

#include "mercury_mem.h"
#include "mercury_proc.h"
#include "mercury_proc_bulk.h"

#ifndef _WIN32
#    include <sys/uio.h>
#endif

/****************/
/* Local Macros */
/****************/

/* Default buf size max if not specified */
#define HG_PERF_BUF_SIZE_MAX (1 << 24)

/* Default RMA count if not specified */
#define HG_PERF_BULK_COUNT (64)

/* Wait max 5s */
#define HG_PERF_TIMEOUT_MAX (5000)

#define STRING(s)  #s
#define XSTRING(s) STRING(s)
#define VERSION_NAME                                                           \
    XSTRING(HG_VERSION_MAJOR)                                                  \
    "." XSTRING(HG_VERSION_MINOR) "." XSTRING(HG_VERSION_PATCH)

#define NDIGITS 2
#define NWIDTH  27

/************************************/
/* Local Type and Struct Definition */
/************************************/

#ifdef _WIN32
struct iovec {
    void *iov_base; /* Pointer to data.  */
    size_t iov_len; /* Length of data.  */
};
#endif

/********************/
/* Local Prototypes */
/********************/

static int
hg_perf_request_progress(unsigned int timeout, void *arg);

static int
hg_perf_request_trigger(unsigned int timeout, unsigned int *flag, void *arg);

static hg_return_t
hg_perf_class_init(const struct hg_test_info *hg_test_info, int class_id,
    struct hg_perf_class_info *info, hg_class_t *hg_class, bool listen);

static void
hg_perf_class_cleanup(struct hg_perf_class_info *info);

static hg_return_t
hg_perf_bulk_buf_alloc(
    struct hg_perf_class_info *info, uint8_t bulk_flags, bool init_data);

static void
hg_perf_bulk_buf_free(struct hg_perf_class_info *info);

static void
hg_perf_init_data(void *buf, size_t buf_size);

static hg_return_t
hg_perf_handle_create_cb(hg_handle_t handle, void *arg);

static hg_return_t
hg_perf_proc_iovec(hg_proc_t proc, void *arg);

static hg_return_t
hg_perf_proc_bulk_init_info(hg_proc_t proc, void *arg);

static hg_return_t
hg_perf_proc_bulk_info(hg_proc_t proc, void *arg);

static hg_return_t
hg_perf_rpc_rate_init_cb(hg_handle_t handle);

static hg_return_t
hg_perf_rpc_rate_cb(hg_handle_t handle);

static hg_return_t
hg_perf_bulk_init_cb(hg_handle_t handle);

static hg_return_t
hg_perf_bulk_push_cb(hg_handle_t handle);

static hg_return_t
hg_perf_bulk_pull_cb(hg_handle_t handle);

static hg_return_t
hg_perf_bulk_common(hg_handle_t handle, hg_bulk_op_t op);

static hg_return_t
hg_perf_bulk_transfer_cb(const struct hg_cb_info *hg_cb_info);

static hg_return_t
hg_perf_done_cb(hg_handle_t handle);

/*******************/
/* Local Variables */
/*******************/

/*---------------------------------------------------------------------------*/
static int
hg_perf_request_progress(unsigned int timeout, void *arg)
{
    struct hg_perf_class_info *info = (struct hg_perf_class_info *) arg;

    if (HG_Progress(info->context, timeout) != HG_SUCCESS)
        return HG_UTIL_FAIL;

    return HG_UTIL_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static int
hg_perf_request_trigger(unsigned int timeout, unsigned int *flag, void *arg)
{
    struct hg_perf_class_info *info = (struct hg_perf_class_info *) arg;
    unsigned int count = 0;

    if (HG_Trigger(info->context, timeout, 1, &count) != HG_SUCCESS)
        return HG_UTIL_FAIL;

    if (flag)
        *flag = (count > 0) ? true : false;

    return HG_UTIL_SUCCESS;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_init(int argc, char *argv[], bool listen, struct hg_perf_info *info)
{
    hg_return_t ret;
    int i;

    /* Initialize the interface */
    memset(info, 0, sizeof(*info));
    if (listen)
        info->hg_test_info.na_test_info.listen = true;
    ret = HG_Test_init(argc, argv, &info->hg_test_info);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Test_init() failed (%s)", HG_Error_to_string(ret));

    info->class_max = info->hg_test_info.na_test_info.max_classes;
    info->class_info = (struct hg_perf_class_info *) calloc(
        info->class_max, sizeof(*info->class_info));
    HG_TEST_CHECK_ERROR(info->class_info == NULL, error, ret, HG_NOMEM,
        "Could not allocate class infos");

    for (i = 0; i < (int) info->class_max; i++) {
        ret = hg_perf_class_init(&info->hg_test_info, i, &info->class_info[i],
            info->hg_test_info.hg_classes[i], listen);
        HG_TEST_CHECK_HG_ERROR(error, ret, "Could not initialize class info");
    }

    return HG_SUCCESS;

error:
    hg_perf_cleanup(info);
    return ret;
}

/*---------------------------------------------------------------------------*/
void
hg_perf_cleanup(struct hg_perf_info *info)
{
    if (info->class_info) {
        size_t i;
        for (i = 0; i < info->class_max; i++)
            hg_perf_class_cleanup(&info->class_info[i]);
        free(info->class_info);
    }
    info->class_info = NULL;
    (void) HG_Test_finalize(&info->hg_test_info);
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_class_init(const struct hg_test_info *hg_test_info, int class_id,
    struct hg_perf_class_info *info, hg_class_t *hg_class, bool listen)
{
    hg_return_t ret;
    unsigned int i;

    info->class_id = class_id;
    info->hg_class = hg_class;
    info->verify = hg_test_info->na_test_info.verify;
    info->bidir = hg_test_info->bidirectional;

    /* Add extra info to handles created */
    ret = HG_Class_set_handle_create_callback(
        info->hg_class, hg_perf_handle_create_cb, NULL);
    HG_TEST_CHECK_HG_ERROR(error, ret,
        "HG_Class_set_handle_create_callback() failed (%s)",
        HG_Error_to_string(ret));

    info->context = HG_Context_create(info->hg_class);
    HG_TEST_CHECK_ERROR(info->context == NULL, error, ret, HG_NOMEM,
        "HG_Context_create() failed");
    (void) HG_Context_set_data(info->context, info, NULL);

    info->request_class = hg_request_init(
        hg_perf_request_progress, hg_perf_request_trigger, info);

    /* Check that sizes are power of 2 */
    info->buf_size_min = hg_test_info->na_test_info.buf_size_min;
    HG_TEST_CHECK_ERROR(!powerof2(info->buf_size_min), error, ret,
        HG_INVALID_ARG, "Max buffer size must be a power of 2 (%zu)",
        info->buf_size_min);

    info->buf_size_max = hg_test_info->na_test_info.buf_size_max;
    if (info->buf_size_max == 0)
        info->buf_size_max = HG_PERF_BUF_SIZE_MAX;
    HG_TEST_CHECK_ERROR(!powerof2(info->buf_size_max), error, ret,
        HG_INVALID_ARG, "Max buffer size must be a power of 2 (%zu)",
        info->buf_size_max);

    /* Register RPCs */
    ret = HG_Register(info->hg_class, HG_PERF_RATE_INIT, NULL, NULL,
        hg_perf_rpc_rate_init_cb);

    ret = HG_Register(info->hg_class, HG_PERF_RATE, hg_perf_proc_iovec,
        (hg_test_info->bidirectional) ? hg_perf_proc_iovec : NULL,
        hg_perf_rpc_rate_cb);

    ret = HG_Register(info->hg_class, HG_PERF_BW_INIT,
        hg_perf_proc_bulk_init_info, NULL, hg_perf_bulk_init_cb);

    ret = HG_Register(info->hg_class, HG_PERF_BW_READ, hg_perf_proc_bulk_info,
        NULL, hg_perf_bulk_push_cb);

    ret = HG_Register(info->hg_class, HG_PERF_BW_WRITE, hg_perf_proc_bulk_info,
        NULL, hg_perf_bulk_pull_cb);

    ret =
        HG_Register(info->hg_class, HG_PERF_DONE, NULL, NULL, hg_perf_done_cb);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Register() failed (%s)", HG_Error_to_string(ret));

    /* Lookup target addrs */
    if (!listen) {
        if (hg_test_info->na_test_info.self_send) {
            info->target_addr_max = 1;

            info->target_addrs =
                (hg_addr_t *) malloc(info->target_addr_max * sizeof(hg_addr_t));
            HG_TEST_CHECK_ERROR(info->target_addrs == NULL, error, ret,
                HG_NOMEM, "Could not allocate array of target addrs");

            ret = HG_Addr_self(info->hg_class, &info->target_addrs[0]);
            HG_TEST_CHECK_HG_ERROR(error, ret, "HG_Addr_self() failed (%s)",
                HG_Error_to_string(ret));
        } else {
            info->target_addr_max = hg_test_info->na_test_info.max_targets;

            info->target_addrs =
                (hg_addr_t *) malloc(info->target_addr_max * sizeof(hg_addr_t));
            HG_TEST_CHECK_ERROR(info->target_addrs == NULL, error, ret,
                HG_NOMEM, "Could not allocate array of target addrs");

            for (i = 0; i < info->target_addr_max; i++) {
                ret = HG_Addr_lookup2(info->hg_class,
                    hg_test_info->na_test_info.target_names[i],
                    &info->target_addrs[i]);
                HG_TEST_CHECK_HG_ERROR(error, ret,
                    "HG_Addr_lookup() failed (%s)", HG_Error_to_string(ret));
            }
        }

        /* Bulk count */
        info->bulk_count = hg_test_info->na_test_info.buf_count;
        if (info->bulk_count == 0)
            info->bulk_count = HG_PERF_BULK_COUNT / info->target_addr_max;

        /* Create handles */
        info->handle_max = hg_test_info->handle_max;
        if (info->handle_max == 0)
            info->handle_max = info->target_addr_max;

        info->handles = malloc(info->handle_max * sizeof(hg_handle_t));
        HG_TEST_CHECK_ERROR(info->handles == NULL, error, ret, HG_NOMEM,
            "Could not allocate array of %zu handles", info->handle_max);

        for (i = 0; i < info->handle_max; i++) {
            ret = HG_Create(info->context,
                info->target_addrs[i % info->target_addr_max], 0,
                &info->handles[i]);
            HG_TEST_CHECK_HG_ERROR(
                error, ret, "HG_Create() failed (%s)", HG_Error_to_string(ret));
        }
    }

    info->request = hg_request_create(info->request_class);
    HG_TEST_CHECK_ERROR(info->request == NULL, error, ret, HG_NOMEM,
        "hg_request_create() failed");

    return HG_SUCCESS;

error:
    hg_perf_class_cleanup(info);

    return ret;
}

/*---------------------------------------------------------------------------*/
static void
hg_perf_class_cleanup(struct hg_perf_class_info *info)
{
    size_t i;

    if (info->handles != NULL) {
        for (i = 0; i < info->handle_max; i++)
            HG_Destroy(info->handles[i]);

        free(info->handles);
    }

    if (info->remote_bulk_handles != NULL) {
        for (i = 0; i < info->handle_max; i++)
            HG_Bulk_free(info->remote_bulk_handles[i]);
        free(info->remote_bulk_handles);
    }

    hg_perf_bulk_buf_free(info);

    if (info->rpc_buf != NULL) {
        hg_mem_aligned_free(info->rpc_buf);
        info->rpc_buf = NULL;
    }
    if (info->rpc_verify_buf != NULL) {
        hg_mem_aligned_free(info->rpc_verify_buf);
        info->rpc_verify_buf = NULL;
    }

    if (info->target_addrs != NULL) {
        for (i = 0; i < info->target_addr_max; i++)
            HG_Addr_free(info->hg_class, info->target_addrs[i]);
        free(info->target_addrs);
    }

    if (info->request != NULL)
        hg_request_destroy(info->request);

    if (info->request_class)
        hg_request_finalize(info->request_class, NULL);

    if (info->context)
        HG_Context_destroy(info->context);
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_bulk_buf_alloc(
    struct hg_perf_class_info *info, uint8_t bulk_flags, bool init_data)
{
    size_t page_size = (size_t) hg_mem_get_page_size();
    hg_return_t ret;
    size_t i;

    info->bulk_bufs = (void **) calloc(info->handle_max, sizeof(void *));
    HG_TEST_CHECK_ERROR(info->bulk_bufs == NULL, error, ret, HG_NOMEM,
        "malloc(%zu) failed", info->handle_max * sizeof(void *));

    info->local_bulk_handles =
        (hg_bulk_t *) calloc(info->handle_max, sizeof(hg_bulk_t));
    HG_TEST_CHECK_ERROR(info->local_bulk_handles == NULL, error, ret, HG_NOMEM,
        "malloc(%zu) failed", info->handle_max * sizeof(hg_bulk_t));

    for (i = 0; i < info->handle_max; i++) {
        hg_size_t alloc_size = info->buf_size_max * info->bulk_count;

        /* Prepare buf */
        info->bulk_bufs[i] = hg_mem_aligned_alloc(page_size, alloc_size);
        HG_TEST_CHECK_ERROR(info->bulk_bufs[i] == NULL, error, ret, HG_NOMEM,
            "hg_mem_aligned_alloc(%zu, %zu) failed", page_size,
            info->buf_size_max);

        /* Initialize data */
        if (init_data)
            hg_perf_init_data(info->bulk_bufs[i], alloc_size);

        ret = HG_Bulk_create(info->hg_class, 1, &info->bulk_bufs[i],
            &alloc_size, bulk_flags, &info->local_bulk_handles[i]);
        HG_TEST_CHECK_HG_ERROR(error, ret, "HG_Bulk_create() failed (%s)",
            HG_Error_to_string(ret));
    }

    return HG_SUCCESS;

error:
    if (info->local_bulk_handles != NULL) {
        for (i = 0; i < info->handle_max; i++)
            (void) HG_Bulk_free(info->local_bulk_handles[i]);
        free(info->local_bulk_handles);
        info->local_bulk_handles = NULL;
    }

    if (info->bulk_bufs != NULL) {
        for (i = 0; i < info->handle_max; i++)
            hg_mem_aligned_free(info->bulk_bufs[i]);
        free(info->bulk_bufs);
        info->bulk_bufs = NULL;
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static void
hg_perf_bulk_buf_free(struct hg_perf_class_info *info)
{
    size_t i;

    if (info->local_bulk_handles != NULL) {
        for (i = 0; i < info->handle_max; i++)
            (void) HG_Bulk_free(info->local_bulk_handles[i]);
        free(info->local_bulk_handles);
        info->local_bulk_handles = NULL;
    }

    if (info->bulk_bufs != NULL) {
        for (i = 0; i < info->handle_max; i++)
            hg_mem_aligned_free(info->bulk_bufs[i]);
        free(info->bulk_bufs);
        info->bulk_bufs = NULL;
    }
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_handle_create_cb(hg_handle_t handle, void HG_ATTR_UNUSED *arg)
{
    struct hg_perf_request *request;
    hg_return_t ret;

    request = (struct hg_perf_request *) calloc(1, sizeof(*request));
    HG_TEST_CHECK_ERROR(request == NULL, error, ret, HG_NOMEM,
        "Could not allocate hg_test_handle_info");

    HG_Set_data(handle, request, free);

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_set_handles(struct hg_perf_class_info *info, enum hg_perf_rpc_id rpc_id)
{
    hg_return_t ret;
    size_t i;

    for (i = 0; i < info->handle_max; i++) {
        ret = HG_Reset(info->handles[i],
            info->target_addrs[i % info->target_addr_max], (hg_id_t) rpc_id);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Reset() failed (%s)", HG_Error_to_string(ret));
    }

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_rpc_buf_init(struct hg_perf_class_info *info)
{
    size_t page_size = (size_t) hg_mem_get_page_size();
    hg_return_t ret;
    size_t i;

    /* Prepare buf */
    info->rpc_buf = hg_mem_aligned_alloc(page_size, info->buf_size_max);
    HG_TEST_CHECK_ERROR(info->rpc_buf == NULL, error, ret, HG_NOMEM,
        "hg_mem_aligned_alloc(%zu, %zu) failed", page_size, info->buf_size_max);

    /* Init data */
    hg_perf_init_data(info->rpc_buf, info->buf_size_max);

    if (info->verify) {
        info->rpc_verify_buf =
            hg_mem_aligned_alloc(page_size, info->buf_size_max);
        HG_TEST_CHECK_ERROR(info->rpc_verify_buf == NULL, error, ret, HG_NOMEM,
            "hg_mem_aligned_alloc(%zu, %zu) failed", page_size,
            info->buf_size_max);
    }

    for (i = 0; i < info->target_addr_max; i++) {
        struct hg_perf_request args = {
            .expected_count = 1, .complete_count = 0, .request = info->request};
        unsigned int completed = 0;

        ret = HG_Reset(info->handles[0], info->target_addrs[i],
            (hg_id_t) HG_PERF_RATE_INIT);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Reset() failed (%s)", HG_Error_to_string(ret));

        hg_request_reset(info->request);

        /* Forward call to target addr */
        ret =
            HG_Forward(info->handles[0], hg_perf_request_complete, &args, NULL);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Forward() failed (%s)", HG_Error_to_string(ret));

        hg_request_wait(info->request, HG_PERF_TIMEOUT_MAX, &completed);
        if (!completed) {
            HG_TEST_LOG_WARNING("Canceling finalize, no response from server");

            ret = HG_Cancel(info->handles[0]);
            HG_TEST_CHECK_HG_ERROR(
                error, ret, "HG_Cancel() failed (%s)", HG_Error_to_string(ret));

            hg_request_wait(info->request, HG_PERF_TIMEOUT_MAX, &completed);
        }
    }

    return HG_SUCCESS;

error:
    if (info->rpc_buf != NULL) {
        hg_mem_aligned_free(info->rpc_buf);
        info->rpc_buf = NULL;
    }
    if (info->rpc_verify_buf != NULL) {
        hg_mem_aligned_free(info->rpc_verify_buf);
        info->rpc_verify_buf = NULL;
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_bulk_buf_init(const struct hg_test_info *hg_test_info,
    struct hg_perf_class_info *info, hg_bulk_op_t bulk_op)
{
    hg_uint8_t bulk_flags =
        (bulk_op == HG_BULK_PULL) ? HG_BULK_READ_ONLY : HG_BULK_WRITE_ONLY;
    hg_return_t ret;
    size_t i;

    ret = hg_perf_bulk_buf_alloc(info, bulk_flags, bulk_op == HG_BULK_PULL);
    HG_TEST_CHECK_HG_ERROR(error, ret, "hg_perf_bulk_buf_alloc() failed (%s)",
        HG_Error_to_string(ret));

    for (i = 0; i < info->handle_max; i++) {
        struct hg_perf_request args = {
            .expected_count = 1, .complete_count = 0, .request = info->request};
        unsigned int completed = 0;
        struct hg_perf_bulk_init_info bulk_info = {
            .bulk = info->local_bulk_handles[i],
            .bulk_op = bulk_op,
            .handle_id = (uint32_t) (i / info->target_addr_max),
            .bulk_count = (uint32_t) info->bulk_count,
            .size_max = (uint32_t) info->buf_size_max,
            .handle_max = (uint32_t) info->handle_max,
            .comm_rank = (uint32_t) hg_test_info->na_test_info.mpi_comm_rank,
            .comm_size = (uint32_t) hg_test_info->na_test_info.mpi_comm_size,
            .target_addr_max = (uint32_t) info->target_addr_max};

        ret = HG_Reset(info->handles[0],
            info->target_addrs[i % info->target_addr_max],
            (hg_id_t) HG_PERF_BW_INIT);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Reset() failed (%s)", HG_Error_to_string(ret));

        hg_request_reset(info->request);

        /* Forward call to target addr */
        ret = HG_Forward(
            info->handles[0], hg_perf_request_complete, &args, &bulk_info);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Forward() failed (%s)", HG_Error_to_string(ret));

        hg_request_wait(info->request, HG_PERF_TIMEOUT_MAX, &completed);
        if (!completed) {
            HG_TEST_LOG_WARNING("Canceling finalize, no response from server");

            ret = HG_Cancel(info->handles[0]);
            HG_TEST_CHECK_HG_ERROR(
                error, ret, "HG_Cancel() failed (%s)", HG_Error_to_string(ret));

            hg_request_wait(info->request, HG_PERF_TIMEOUT_MAX, &completed);
        }
    }

    return HG_SUCCESS;

error:
    hg_perf_bulk_buf_free(info);

    return ret;
}

/*---------------------------------------------------------------------------*/
static void
hg_perf_init_data(void *buf, size_t buf_size)
{
    char *buf_ptr = (char *) buf;
    size_t i;

    for (i = 0; i < buf_size; i++)
        buf_ptr[i] = (char) i;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_verify_data(const void *buf, size_t buf_size)
{
    const char *buf_ptr = (const char *) buf;
    hg_return_t ret;
    size_t i;

    for (i = 0; i < buf_size; i++) {
        HG_TEST_CHECK_ERROR(buf_ptr[i] != (char) i, error, ret, HG_FAULT,
            "Error detected in bulk transfer, buf[%zu] = %d, "
            "was expecting %d!",
            i, buf_ptr[i], (char) i);
    }

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
void
hg_perf_print_header_lat(const struct hg_test_info *hg_test_info,
    const struct hg_perf_class_info *info, const char *benchmark)
{
    printf("# %s v%s\n", benchmark, VERSION_NAME);
    printf("# Loop %d times from size %zu to %zu byte(s) with %zu handle(s) "
           "in-flight\n",
        hg_test_info->na_test_info.loop, info->buf_size_min, info->buf_size_max,
        info->handle_max);
    if (info->handle_max < info->target_addr_max)
        printf("# WARNING number of handles in flight less than number of "
               "targets\n");
    if (info->verify)
        printf("# WARNING verifying data, output will be slower\n");
    printf("%-*s%*s%*s\n", 10, "# Size", NWIDTH, "Avg time (us)", NWIDTH,
        "Avg rate (RPC/s)");
    fflush(stdout);
}

/*---------------------------------------------------------------------------*/
void
hg_perf_print_lat(const struct hg_test_info *hg_test_info,
    const struct hg_perf_class_info *info, size_t buf_size, hg_time_t t)
{
    double rpc_time;
    size_t loop = (size_t) hg_test_info->na_test_info.loop,
           handle_max = (size_t) info->handle_max,
           dir = (size_t) (hg_test_info->bidirectional ? 2 : 1),
           mpi_comm_size = (size_t) hg_test_info->na_test_info.mpi_comm_size;

    rpc_time = hg_time_to_double(t) * 1e6 /
               (double) (loop * handle_max * dir * mpi_comm_size);

    printf("%-*zu%*.*f%*lu\n", 10, buf_size, NWIDTH, NDIGITS, rpc_time, NWIDTH,
        (long unsigned int) (1e6 / rpc_time));
}

/*---------------------------------------------------------------------------*/
void
hg_perf_print_header_bw(const struct hg_test_info *hg_test_info,
    const struct hg_perf_class_info *info, const char *benchmark)
{
    printf("# %s v%s\n", benchmark, VERSION_NAME);
    printf("# Loop %d times from size %zu to %zu byte(s) with %zu handle(s) "
           "in-flight\n# - %zu bulk transfer(s) per handle\n",
        hg_test_info->na_test_info.loop, info->buf_size_min, info->buf_size_max,
        info->handle_max, (size_t) info->bulk_count);
    if (info->verify)
        printf("# WARNING verifying data, output will be slower\n");
    if (hg_test_info->na_test_info.mbps)
        printf("%-*s%*s%*s\n", 10, "# Size", NWIDTH, "Bandwidth (MB/s)", NWIDTH,
            "Time (us)");
    else
        printf("%-*s%*s%*s\n", 10, "# Size", NWIDTH, "Bandwidth (MiB/s)",
            NWIDTH, "Time (us)");
    fflush(stdout);
}

/*---------------------------------------------------------------------------*/
void
hg_perf_print_bw(const struct hg_test_info *hg_test_info,
    const struct hg_perf_class_info *info, size_t buf_size, hg_time_t t)
{
    size_t loop = (size_t) hg_test_info->na_test_info.loop,
           mpi_comm_size = (size_t) hg_test_info->na_test_info.mpi_comm_size,
           handle_max = (size_t) info->handle_max,
           buf_count = (size_t) info->bulk_count;
    double avg_time, avg_bw;

    avg_time = hg_time_to_double(t) * 1e6 /
               (double) (loop * handle_max * mpi_comm_size * buf_count);
    avg_bw =
        (double) (buf_size * loop * handle_max * mpi_comm_size * buf_count) /
        hg_time_to_double(t);

    if (hg_test_info->na_test_info.mbps)
        avg_bw /= 1e6; /* MB/s, matches OSU benchmarks */
    else
        avg_bw /= (1024 * 1024); /* MiB/s */

    printf("%-*zu%*.*f%*.*f\n", 10, buf_size, NWIDTH, NDIGITS, avg_bw, NWIDTH,
        NDIGITS, avg_time);
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_request_complete(const struct hg_cb_info *hg_cb_info)
{
    struct hg_perf_request *info = (struct hg_perf_request *) hg_cb_info->arg;

    if ((++info->complete_count) == info->expected_count)
        hg_request_complete(info->request);

    return HG_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_proc_iovec(hg_proc_t proc, void *arg)
{
    struct iovec *iov = (struct iovec *) arg;
    uint32_t len = (uint32_t) iov->iov_len;
    hg_return_t ret;

    if (hg_proc_get_op(proc) == HG_FREE)
        return HG_SUCCESS;

    ret = hg_proc_uint32_t(proc, &len);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));
    iov->iov_len = (size_t) len;

    if (len == 0)
        return HG_SUCCESS;

    ret = hg_proc_raw(proc, iov->iov_base, iov->iov_len);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_raw() failed (%s)", HG_Error_to_string(ret));

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_proc_bulk_init_info(hg_proc_t proc, void *arg)
{
    struct hg_perf_bulk_init_info *info = (struct hg_perf_bulk_init_info *) arg;
    hg_return_t ret;

    ret = hg_proc_hg_bulk_t(proc, &info->bulk);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_hg_bulk_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->bulk_op);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->handle_id);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->handle_max);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->bulk_count);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->size_max);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->comm_rank);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->comm_size);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->target_addr_max);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_proc_bulk_info(hg_proc_t proc, void *arg)
{
    struct hg_perf_bulk_info *info = (struct hg_perf_bulk_info *) arg;
    hg_return_t ret;

    ret = hg_proc_uint32_t(proc, &info->comm_rank);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->handle_id);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    ret = hg_proc_uint32_t(proc, &info->size);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_proc_uint32_t() failed (%s)", HG_Error_to_string(ret));

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_rpc_rate_init_cb(hg_handle_t handle)
{
    size_t page_size = (size_t) hg_mem_get_page_size();
    const struct hg_info *hg_info = HG_Get_info(handle);
    struct hg_perf_class_info *info = HG_Context_get_data(hg_info->context);
    hg_return_t ret;

    /* Prepare buf */
    info->rpc_buf = hg_mem_aligned_alloc(page_size, info->buf_size_max);
    HG_TEST_CHECK_ERROR(info->rpc_buf == NULL, error, ret, HG_NOMEM,
        "hg_mem_aligned_alloc(%zu, %zu) failed", page_size, info->buf_size_max);

    if (info->verify) {
        info->rpc_verify_buf =
            hg_mem_aligned_alloc(page_size, info->buf_size_max);
        HG_TEST_CHECK_ERROR(info->rpc_verify_buf == NULL, error, ret, HG_NOMEM,
            "hg_mem_aligned_alloc(%zu, %zu) failed", page_size,
            info->buf_size_max);
    }

    /* Send response back */
    ret = HG_Respond(handle, NULL, NULL, NULL);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Respond() failed (%s)", HG_Error_to_string(ret));

    (void) HG_Destroy(handle);

    return HG_SUCCESS;

error:
    if (info->rpc_buf != NULL) {
        hg_mem_aligned_free(info->rpc_buf);
        info->rpc_buf = NULL;
    }
    if (info->rpc_verify_buf != NULL) {
        hg_mem_aligned_free(info->rpc_verify_buf);
        info->rpc_verify_buf = NULL;
    }

    (void) HG_Destroy(handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_rpc_rate_cb(hg_handle_t handle)
{
    const struct hg_info *hg_info = HG_Get_info(handle);
    struct hg_perf_class_info *info = HG_Context_get_data(hg_info->context);
    struct iovec iov;
    hg_return_t ret;

    /* Get input struct */
    iov = (struct iovec){
        .iov_base = info->rpc_buf, .iov_len = info->buf_size_max};
    if (info->verify)
        memset(iov.iov_base, 0, iov.iov_len);

    ret = HG_Get_input(handle, &iov);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Get_input() failed (%s)", HG_Error_to_string(ret));

    if (info->verify) {
        ret = hg_perf_verify_data(iov.iov_base, iov.iov_len);
        HG_TEST_CHECK_HG_ERROR(error_free, ret,
            "hg_perf_verify_data() failed (%s)", HG_Error_to_string(ret));
    }

    /* Send response back */
    if (info->bidir) {
        ret = HG_Respond(handle, NULL, NULL, &iov);
    } else {
        ret = HG_Respond(handle, NULL, NULL, NULL);
    }
    HG_TEST_CHECK_HG_ERROR(
        error_free, ret, "HG_Respond() failed (%s)", HG_Error_to_string(ret));

    (void) HG_Free_input(handle, &iov);
    (void) HG_Destroy(handle);

    return HG_SUCCESS;

error_free:
    (void) HG_Free_input(handle, &iov);
error:
    (void) HG_Destroy(handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_bulk_init_cb(hg_handle_t handle)
{
    const struct hg_info *hg_info = HG_Get_info(handle);
    struct hg_perf_class_info *info = HG_Context_get_data(hg_info->context);
    struct hg_perf_bulk_init_info bulk_info;
    size_t bulk_index;
    hg_return_t ret;

    ret = HG_Get_input(handle, &bulk_info);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Get_input() failed (%s)", HG_Error_to_string(ret));

    if (info->bulk_bufs == NULL) {
        hg_uint8_t bulk_flags = (bulk_info.bulk_op == HG_BULK_PULL)
                                    ? HG_BULK_WRITE_ONLY
                                    : HG_BULK_READ_ONLY;
        info->handle_per_rank =
            (bulk_info.handle_max / bulk_info.target_addr_max);

        info->handle_max = info->handle_per_rank * bulk_info.comm_size;
        info->bulk_count = bulk_info.bulk_count;
        info->buf_size_max = bulk_info.size_max;

        ret = hg_perf_bulk_buf_alloc(
            info, bulk_flags, bulk_info.bulk_op == HG_BULK_PUSH);
        HG_TEST_CHECK_HG_ERROR(error_free, ret,
            "hg_perf_bulk_buf_alloc() failed (%s)", HG_Error_to_string(ret));

        info->remote_bulk_handles =
            (hg_bulk_t *) calloc(info->handle_max, sizeof(hg_bulk_t));
        HG_TEST_CHECK_ERROR(info->remote_bulk_handles == NULL, error_free, ret,
            HG_NOMEM, "malloc(%zu) failed",
            info->handle_max * sizeof(hg_bulk_t));
    }

    bulk_index = bulk_info.handle_id % info->handle_per_rank +
                 info->handle_per_rank * bulk_info.comm_rank;

    info->remote_bulk_handles[bulk_index] = bulk_info.bulk;
    HG_Bulk_ref_incr(bulk_info.bulk);

    /* Send response back */
    ret = HG_Respond(handle, NULL, NULL, NULL);
    HG_TEST_CHECK_HG_ERROR(
        error_free, ret, "HG_Respond() failed (%s)", HG_Error_to_string(ret));

    (void) HG_Free_input(handle, &bulk_info);
    (void) HG_Destroy(handle);

    return HG_SUCCESS;

error_free:
    (void) HG_Free_input(handle, &bulk_info);
error:
    (void) HG_Destroy(handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_bulk_push_cb(hg_handle_t handle)
{
    return hg_perf_bulk_common(handle, HG_BULK_PUSH);
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_bulk_pull_cb(hg_handle_t handle)
{
    return hg_perf_bulk_common(handle, HG_BULK_PULL);
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_bulk_common(hg_handle_t handle, hg_bulk_op_t op)
{
    const struct hg_info *hg_info = HG_Get_info(handle);
    struct hg_perf_class_info *info = HG_Context_get_data(hg_info->context);
    struct hg_perf_request *request =
        (struct hg_perf_request *) HG_Get_data(handle);
    struct hg_perf_bulk_info bulk_info;
    hg_return_t ret;
    size_t i, bulk_index;

    /* Get input struct */
    ret = HG_Get_input(handle, &bulk_info);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Get_input() failed (%s)", HG_Error_to_string(ret));

    bulk_index = bulk_info.handle_id % info->handle_per_rank +
                 info->handle_per_rank * bulk_info.comm_rank;

    /* Initialize request */
    *request = (struct hg_perf_request){.complete_count = 0,
        .expected_count = (int32_t) info->bulk_count,
        .request = NULL};

    /* Post bulk push */
    for (i = 0; i < info->bulk_count; i++) {
        ret = HG_Bulk_transfer(info->context, hg_perf_bulk_transfer_cb, handle,
            op, hg_info->addr, info->remote_bulk_handles[bulk_index],
            i * info->buf_size_max, info->local_bulk_handles[bulk_index],
            i * info->buf_size_max, bulk_info.size, HG_OP_ID_IGNORE);
        HG_TEST_CHECK_HG_ERROR(error_free, ret,
            "HG_Bulk_transfer() failed (%s)", HG_Error_to_string(ret));
    }

    (void) HG_Free_input(handle, &bulk_info);

    return HG_SUCCESS;

error_free:
    (void) HG_Free_input(handle, &bulk_info);
error:
    (void) HG_Destroy(handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_bulk_transfer_cb(const struct hg_cb_info *hg_cb_info)
{
    hg_handle_t handle = (hg_handle_t) hg_cb_info->arg;
    const struct hg_info *hg_info = HG_Get_info(handle);
    struct hg_perf_class_info *info = HG_Context_get_data(hg_info->context);
    struct hg_perf_request *request =
        (struct hg_perf_request *) HG_Get_data(handle);

    HG_TEST_CHECK_HG_ERROR(done, hg_cb_info->ret, "Bulk transfer failed (%s)",
        HG_Error_to_string(hg_cb_info->ret));

done:
    if ((++request->complete_count) == request->expected_count) {
        if (hg_cb_info->info.bulk.op == HG_BULK_PULL && info->verify) {
            void *buf;
            hg_size_t buf_size;
            hg_uint32_t actual_count;
            hg_return_t ret;
            size_t i;

            ret = HG_Bulk_access(hg_cb_info->info.bulk.local_handle, 0,
                info->buf_size_max * info->bulk_count, HG_BULK_READWRITE, 1,
                &buf, &buf_size, &actual_count);
            HG_TEST_CHECK_HG_ERROR(error, ret, "HG_Bulk_access() failed (%s)",
                HG_Error_to_string(ret));
            HG_TEST_CHECK_ERROR(actual_count != 1, error, ret, HG_FAULT,
                "actual_count=%d", (int) actual_count);
            HG_TEST_CHECK_ERROR(
                buf_size != info->buf_size_max * info->bulk_count, error, ret,
                HG_FAULT, "buf_size=%zu", (size_t) buf_size);

            for (i = 0; i < info->bulk_count; i++) {
                char *buf_p = (char *) buf + info->buf_size_max * i;
                ret = hg_perf_verify_data(buf_p, hg_cb_info->info.bulk.size);
                HG_TEST_CHECK_HG_ERROR(error, ret,
                    "hg_perf_verify_data() failed (%s, %p)",
                    HG_Error_to_string(ret), buf);
            }
        }

        HG_Respond(handle, NULL, NULL, NULL);
        (void) HG_Destroy(handle);
    }

    return HG_SUCCESS;

error:
    HG_Respond(handle, NULL, NULL, NULL);
    (void) HG_Destroy(handle);

    return HG_FAULT;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_perf_done_cb(hg_handle_t handle)
{
    const struct hg_info *hg_info = HG_Get_info(handle);
    struct hg_perf_class_info *info = HG_Context_get_data(hg_info->context);
    hg_return_t ret;

    /* Set done for context data */
    info->done = true;

    /* Send response back */
    ret = HG_Respond(handle, NULL, NULL, NULL);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Respond() failed (%s)", HG_Error_to_string(ret));

    (void) HG_Destroy(handle);

    return HG_SUCCESS;

error:
    (void) HG_Destroy(handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_perf_send_done(struct hg_perf_class_info *info)
{
    hg_return_t ret;
    size_t i;

    for (i = 0; i < info->target_addr_max; i++) {
        struct hg_perf_request args = {
            .expected_count = 1, .complete_count = 0, .request = info->request};
        unsigned int completed = 0;

        ret = HG_Reset(
            info->handles[0], info->target_addrs[i], (hg_id_t) HG_PERF_DONE);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Reset() failed (%s)", HG_Error_to_string(ret));

        hg_request_reset(info->request);

        /* Forward call to target addr */
        ret =
            HG_Forward(info->handles[0], hg_perf_request_complete, &args, NULL);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Forward() failed (%s)", HG_Error_to_string(ret));

        hg_request_wait(info->request, HG_PERF_TIMEOUT_MAX, &completed);
        if (!completed) {
            HG_TEST_LOG_WARNING("Canceling finalize, no response from server");

            ret = HG_Cancel(info->handles[0]);
            HG_TEST_CHECK_HG_ERROR(
                error, ret, "HG_Cancel() failed (%s)", HG_Error_to_string(ret));

            hg_request_wait(info->request, HG_PERF_TIMEOUT_MAX, &completed);
        }
    }

    return HG_SUCCESS;

error:
    return ret;
}

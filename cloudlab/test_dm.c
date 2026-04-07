#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>

int main() {
    int num;
    struct ibv_device **devs = ibv_get_device_list(&num);
    printf("Found %d devices\n", num);

    /* Open mlx5_0 specifically (same as DEFT rnic_id=0) */
    struct ibv_context *ctx = NULL;
    int i;
    for (i = 0; i < num; i++) {
        const char *name = ibv_get_device_name(devs[i]);
        printf("  Device %d: %s\n", i, name);
        if (name[5] == '0') {
            ctx = ibv_open_device(devs[i]);
            printf("  -> Opened %s\n", name);
            break;
        }
    }
    if (ctx == NULL) { printf("No device found\n"); return 1; }
    ibv_free_device_list(devs);

    /* Create PD (same as DEFT) */
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    printf("PD: %p\n", pd);

    /* Query GID 3 (same as DEFT default) */
    union ibv_gid gid;
    ibv_query_gid(ctx, 1, 3, &gid);
    printf("GID[3]: %02x%02x:%02x%02x:...\n", gid.raw[0], gid.raw[1], gid.raw[2], gid.raw[3]);

    /* Test DM allocation at this point */
    printf("\n--- Test 1: DM right after context ---\n");
    {
        struct ibv_exp_alloc_dm_attr dm_attr;
        memset(&dm_attr, 0, sizeof(dm_attr));
        dm_attr.length = 131072;
        errno = 0;
        struct ibv_exp_dm *dm = ibv_exp_alloc_dm(ctx, &dm_attr);
        printf("Result: %s (errno=%d: %s)\n", dm ? "OK" : "FAILED", errno, strerror(errno));
        if (dm) ibv_exp_free_dm(dm);
    }

    /* Create CQ (same as DEFT) */
    struct ibv_cq *cq = ibv_create_cq(ctx, 128, NULL, NULL, 0);
    printf("\nCQ: %p\n", cq);

    /* Create QP (UD, same as DEFT message connection) */
    struct ibv_exp_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_type = IBV_QPT_UD;
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.pd = pd;
    qp_attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD;
    qp_attr.cap.max_send_wr = 128;
    qp_attr.cap.max_recv_wr = 128;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    struct ibv_qp *qp = ibv_exp_create_qp(ctx, &qp_attr);
    printf("QP: %p\n", qp);

    /* Allocate hugepage memory and register MR (like DEFT 62GB DSM) */
    size_t dsm_size = (size_t)62 * 1024 * 1024 * 1024ULL;
    void *dsm = mmap(NULL, dsm_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    printf("\nDSM mmap (%lu GB): %p\n", dsm_size / (1024*1024*1024ULL), dsm);
    if (dsm != MAP_FAILED) {
        struct ibv_mr *mr = ibv_reg_mr(pd, dsm, dsm_size,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
            IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
        printf("DSM MR: %p\n", mr);
    }

    /* Test DM allocation AFTER all the above */
    printf("\n--- Test 2: DM after CQ+QP+62GB MR ---\n");
    {
        struct ibv_exp_alloc_dm_attr dm_attr;
        memset(&dm_attr, 0, sizeof(dm_attr));
        dm_attr.length = 131072;
        errno = 0;
        struct ibv_exp_dm *dm = ibv_exp_alloc_dm(ctx, &dm_attr);
        printf("Result: %s (errno=%d: %s)\n", dm ? "OK" : "FAILED", errno, strerror(errno));
        if (dm) ibv_exp_free_dm(dm);
    }

    ibv_close_device(ctx);
    return 0;
}


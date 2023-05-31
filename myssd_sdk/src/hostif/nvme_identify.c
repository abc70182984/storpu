#include <config.h>
#include "nvme.h"
#include "../proto.h"

#include <string.h>

#define NSID_ALL 0xffffffff

int nvme_identify_namespace(u32 nsid, u8* data)
{
    struct nvme_id_ns* id_ns = (struct nvme_id_ns*)data;
    struct nvme_lbaf* lbaf;
    struct namespace_info ns_info;
    int r;

    memset(id_ns, 0, sizeof(*id_ns));

    if (nsid != NSID_ALL) {
        r = ftl_get_namespace(nsid, &ns_info);
        if (r != 0) return NVME_SC_INVALID_NS;

        id_ns->nsze = ns_info.size_blocks;
        id_ns->ncap = ns_info.capacity_blocks;
        id_ns->nuse = ns_info.util_blocks;
    }

    lbaf = &id_ns->lbaf[0];
    lbaf->ds = SECTOR_SHIFT;
    lbaf->ms = 0;
    lbaf->rp = 2;

    return 0;
}

void nvme_identify_controller(u8* data)
{
    struct nvme_id_ctrl* id_ctrl = (struct nvme_id_ctrl*)data;
    struct nvme_id_power_state* ps;

    memset(id_ctrl, 0, sizeof(*id_ctrl));

    id_ctrl->vid = 0x9038;
    id_ctrl->ssvid = 0x0007;

    id_ctrl->rab = 0x0;
    id_ctrl->ieee[0] = 0xa1;
    id_ctrl->ieee[1] = 0xb2;
    id_ctrl->ieee[2] = 0xc3;
    id_ctrl->cmic = 0x0;
    id_ctrl->mdts = MAX_DATA_TRANSFER_SIZE;
    id_ctrl->cntlid = 0x9;

    id_ctrl->acl = 0x3;
    id_ctrl->aerl = 0x3;

    id_ctrl->frmw = 0x3; /* (NOFS = 1) | FFSRO */

    id_ctrl->elpe = 0x8;

    id_ctrl->sqes = (0x6 << 4) | 0x6;
    id_ctrl->cqes = (0x4 << 4) | 0x4;

    id_ctrl->nn = 1; /* One namespace. */

    id_ctrl->oacs =
        NVME_CTRL_OACS_NS_MNGT_SUPP; /* Supports NVMe namespace management. */

    id_ctrl->oncs =
        NVME_CTRL_ONCS_WRITE_ZEROES; /* Supports Write Zeroes command. */

    id_ctrl->vwc = 0x4 | NVME_CTRL_VWC_PRESENT; /* Has volatile write cache.
                           Does not support the NSID field set to FFFFFFFFh. */

    id_ctrl->sgls = 0x0; /* No SGL support. */

    ps = &id_ctrl->psd[0];
    ps->max_power = 0x09c4;
}

void nvme_identify_ns_active_list(u8* data)
{
    u32* list = (u32*)data;
    struct namespace_info ns_info;
    int count = 0;
    int nsid, r;

    memset(list, 0, 0x1000);

    for (nsid = 1; nsid <= NAMESPACE_MAX; nsid++) {
        r = ftl_get_namespace(nsid, &ns_info);
        if (r != 0) continue;
        if (!ns_info.active) continue;

        list[count++] = nsid;
    }
}

void nvme_identify_cs_controller(u8 csi, u8* data)
{
    struct nvme_id_ctrl_nvm* id_ctrl_nvm;

    switch (csi) {
    case NVME_CSI_NVM:
        id_ctrl_nvm = (struct nvme_id_ctrl_nvm*)data;
        memset(id_ctrl_nvm, 0, sizeof(*id_ctrl_nvm));
        break;
    }
}

/*
 * node_identity.c - FCFG factory id + NVS-persisted RF address (see node_identity.h).
 *
 * NVS uses the internal-flash region already reserved in the board file
 * (CC1310_LAUNCHXL.c: flashBuf @ 0x1A000, Board_NVSINTERNAL). The address is a
 * "set-once, rarely rewritten" value - exactly what the TI NVS driver is for
 * (do NOT hand-roll FlashProgram; the legacy TI compiler mis-aligns const arrays).
 * See [[cc1310-flash-persistence-nvs]].
 */
#include "node_identity.h"

#include <string.h>
#include <ti/drivers/NVS.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_fcfg1.h)

#include "Board.h"   /* Board_NVSINTERNAL */

uint8_t gNodeAddress = ADDR_UNPROVISIONED;
uint8_t gFactoryId[NODE_FACTORY_ID_LEN];

/* NVS record. magic guards against reading uninitialized/erased flash; padded to a
 * multiple of 4 bytes. */
#define IDENTITY_MAGIC  0x53434E31u   /* "SCN1" - Solar Controller Node v1 */

typedef struct {
    uint32_t magic;
    uint8_t  addr;
    uint8_t  _pad[3];
} IdentityRec;

static NVS_Handle sNvs = NULL;

/* Read the chip's IEEE 802.15.4 MAC from FCFG (8 bytes, little-endian in the two
 * 32-bit words) - the immutable per-chip factory id. */
static void read_factory_id(uint8_t out[NODE_FACTORY_ID_LEN])
{
    uint32_t lo = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0);
    uint32_t hi = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_1);
    out[0] = (uint8_t)(lo);
    out[1] = (uint8_t)(lo >> 8);
    out[2] = (uint8_t)(lo >> 16);
    out[3] = (uint8_t)(lo >> 24);
    out[4] = (uint8_t)(hi);
    out[5] = (uint8_t)(hi >> 8);
    out[6] = (uint8_t)(hi >> 16);
    out[7] = (uint8_t)(hi >> 24);
}

void identity_init(void)
{
    NVS_Params params;
    IdentityRec rec;

    read_factory_id(gFactoryId);
    gNodeAddress = ADDR_UNPROVISIONED;

    NVS_init();
    NVS_Params_init(&params);
    sNvs = NVS_open(Board_NVSINTERNAL, &params);
    if (sNvs == NULL) {
        return;   /* no NVS -> stay unprovisioned (JOIN can still re-provision) */
    }
    if (NVS_read(sNvs, 0, &rec, sizeof(rec)) == NVS_STATUS_SUCCESS) {
        if (rec.magic == IDENTITY_MAGIC &&
            (rec.addr == ADDR_UNPROVISIONED ||
             (rec.addr >= ADDR_POOL_FIRST && rec.addr <= ADDR_POOL_LAST))) {
            gNodeAddress = rec.addr;
        }
    }
}

bool identity_persist(void)
{
    IdentityRec rec;

    if (sNvs == NULL) {
        return false;
    }
    memset(&rec, 0, sizeof(rec));
    rec.magic = IDENTITY_MAGIC;
    rec.addr  = gNodeAddress;
    return (NVS_write(sNvs, 0, &rec, sizeof(rec),
                      NVS_WRITE_ERASE | NVS_WRITE_POST_VERIFY) == NVS_STATUS_SUCCESS);
}

bool factory_is_mine(const uint8_t *fid)
{
    return (fid != NULL) && (memcmp(fid, gFactoryId, NODE_FACTORY_ID_LEN) == 0);
}

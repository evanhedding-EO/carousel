/* bus.c - see bus.h.  Targets SOEM 2.0 (context-based ecx_* API). */
#include "bus.h"
#include "soem/soem.h"
#include <stdio.h>

static ecx_contextt ctx;   /* the one master context for this process */

int bus_open(const char *ifname)
{
    if (!ecx_init(&ctx, ifname)) {
        fprintf(stderr, "bus: ecx_init('%s') failed - check NIC name and permissions "
                        "(run as root or setcap cap_net_raw,cap_net_admin+ep)\n", ifname);
        return -1;
    }
    int n = ecx_config_init(&ctx);   /* leaves slaves in PRE-OP; no PDO map needed for SDO */
    if (n <= 0) {
        fprintf(stderr, "bus: no slaves found on '%s'\n", ifname);
        ecx_close(&ctx);
        return 0;
    }
    return n;
}

void bus_close(void) { ecx_close(&ctx); }

void bus_print_slaves(void)
{
    printf("%d slave(s) found:\n", ctx.slavecount);
    for (int i = 1; i <= ctx.slavecount; i++) {
        ec_slavet *s = &ctx.slavelist[i];
        printf("  [%d] %-20s vendor=0x%08X product=0x%08X rev=0x%08X  state=0x%02X\n",
               i, s->name, (unsigned)s->eep_man, (unsigned)s->eep_id,
               (unsigned)s->eep_rev, (unsigned)s->state);
    }
}

int sdo_read(uint16_t s, uint16_t idx, uint8_t sub, void *buf, int size)
{
    int sz = size;
    int wkc = ecx_SDOread(&ctx, s, idx, sub, FALSE, &sz, buf, EC_TIMEOUTRXM);
    return wkc > 0;
}

int sdo_write(uint16_t s, uint16_t idx, uint8_t sub, const void *buf, int size)
{
    int wkc = ecx_SDOwrite(&ctx, s, idx, sub, FALSE, size, (void *)buf, EC_TIMEOUTRXM);
    return wkc > 0;
}

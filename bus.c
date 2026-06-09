/* bus.c - see bus.h */
#include "bus.h"
#include "ethercat.h"
#include <stdio.h>

int bus_open(const char *ifname)
{
    if (!ec_init(ifname)) {
        fprintf(stderr, "bus: ec_init('%s') failed - check NIC name and permissions "
                        "(run as root or setcap cap_net_raw,cap_net_admin+ep)\n", ifname);
        return -1;
    }
    int n = ec_config_init(FALSE);   /* leaves slaves in PRE-OP; no PDO map needed for SDO */
    if (n <= 0) {
        fprintf(stderr, "bus: no slaves found on '%s'\n", ifname);
        ec_close();
        return 0;
    }
    return n;
}

void bus_close(void) { ec_close(); }

void bus_print_slaves(void)
{
    printf("%d slave(s) found:\n", ec_slavecount);
    for (int i = 1; i <= ec_slavecount; i++) {
        ec_slavet *s = &ec_slave[i];
        printf("  [%d] %-20s vendor=0x%08X product=0x%08X rev=0x%08X  state=0x%02X\n",
               i, s->name, s->eep_man, s->eep_id, s->eep_rev, s->state);
    }
}

int sdo_read(uint16_t s, uint16_t idx, uint8_t sub, void *buf, int size)
{
    int sz = size;
    int wkc = ec_SDOread(s, idx, sub, FALSE, &sz, buf, EC_TIMEOUTRXM);
    return wkc > 0;
}

int sdo_write(uint16_t s, uint16_t idx, uint8_t sub, const void *buf, int size)
{
    int wkc = ec_SDOwrite(s, idx, sub, FALSE, size, (void *)buf, EC_TIMEOUTRXM);
    return wkc > 0;
}

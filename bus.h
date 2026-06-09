/* bus.h - thin SOEM wrapper: open the bus to PRE-OP and do typed SDO transfers.
 * No PDO / OP here - these milestones are mailbox (SDO) only, so no motion. */
#ifndef BUS_H
#define BUS_H

#include <stdint.h>

/* Open NIC, init SOEM, bring slaves to PRE-OP.
 * Returns slave count (>0) on success, <=0 on error (message printed). */
int  bus_open(const char *ifname);
void bus_close(void);

/* Print every enumerated slave: name, vendor/product/revision, AL state. */
void bus_print_slaves(void);

/* Typed SDO access to slave `s` (1-based). `size` is the object width in bytes.
 * Return 1 on success (working counter > 0), 0 on failure. */
int sdo_read (uint16_t s, uint16_t idx, uint8_t sub, void *buf, int size);
int sdo_write(uint16_t s, uint16_t idx, uint8_t sub, const void *buf, int size);

#endif /* BUS_H */

/* bus.h - thin SOEM wrapper: open the bus, do typed SDO transfers, and (for
 * motion milestones) reach OP and exchange the cyclic control/status words. */
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

/* --- cyclic process data / OP (milestone 2+) --- */
int      bus_enter_op(void);              /* config_map + SAFE-OP -> OP; 1 ok, 0 fail */
void     bus_cycle(void);                 /* exchange one PDO frame (send + receive) */
void     bus_set_controlword(uint16_t cw);/* RxPDO 0x1600: 6040h at offset 0 */
uint16_t bus_statusword(void);            /* TxPDO 0x1A00: 6041h at offset 2 */

#endif /* BUS_H */

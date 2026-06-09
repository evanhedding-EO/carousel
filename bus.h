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
/* cyclic field access at the drive's REAL PDO offsets (verified via 'pdomap'):
 *   out: 6040 ctrlword@0  607A targetpos@2  60FF targetvel@6  6060 mode@10
 *   in:  6041 status@0    6064 posactual@2  606C velactual@6  6061 modedisp@10 */
void     bus_set_controlword(uint16_t cw);
void     bus_set_mode(int8_t mode);
void     bus_set_target_velocity(int32_t v);
void     bus_set_target_position(int32_t p);
uint16_t bus_statusword(void);
int32_t  bus_velocity_actual(void);
int32_t  bus_position_actual(void);
void     bus_report_state(const char *what);/* print slave 1 EtherCAT state + AL code */
void     bus_debug(void);                 /* one-shot: WKC, mapped I/O sizes, raw input bytes */

#endif /* BUS_H */

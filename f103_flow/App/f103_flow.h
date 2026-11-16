#ifndef F103_FLOW_H
#define F103_FLOW_H
#include <stdint.h>
#define FLOW_OBSERVED 16u
typedef struct {
    uint16_t value[FLOW_OBSERVED];
    int rc[FLOW_OBSERVED], comm, hold;
} flow_snapshot_t;
typedef struct {
    uint32_t magic, mcu_reset_flags, done, checkpoint;
    uint16_t manufacturer_id, device_id;
    int init_rc, result;
    flow_snapshot_t startup;
    uint32_t new_pairs, valid_pairs, ch0, ch1;
    uint16_t status, data_errors;
    int poll_rc;
} flow_debug_t;
extern volatile flow_debug_t g_flow_debug;
int f103_flow_start(void);
void f103_flow_poll(void);
void f103_flow_monitor(uint32_t round);
void f103_flow_done_checkpoint(void);
#endif

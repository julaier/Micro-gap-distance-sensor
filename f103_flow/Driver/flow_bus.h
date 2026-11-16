#ifndef F103_FLOW_BUS_H
#define F103_FLOW_BUS_H
#include "wake_i2c.h"
#define FLOW_RECORDS 24u
typedef struct {
    uint8_t operation, reg, payload_index;
    uint16_t value;
    int rc;
    test_i2c_diag_t bus;
} flow_record_t;
typedef struct {
    int init_rc;
    unsigned count, overflow, recording, first_failed;
    flow_record_t records[FLOW_RECORDS], first_failure;
    ack_trace_t first_ack;
} flow_bus_debug_t;
extern volatile flow_bus_debug_t g_flow_bus;
void flow_bus_freeze(void);
#endif

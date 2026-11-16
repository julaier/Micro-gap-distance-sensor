#ifndef F103_FLOW_BOARD_H
#define F103_FLOW_BOARD_H
#include "test_board.h"
/* The original LDC driver only needs this board-level delay. */
#define board_delay_ms test_delay_ms
#endif

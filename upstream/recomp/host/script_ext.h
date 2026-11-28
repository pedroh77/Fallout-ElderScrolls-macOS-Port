#pragma once
#include <stdint.h>

extern uint32_t host_script_cmd_base;
extern uint32_t host_script_cmd_max_opcode;
extern uint32_t host_script_cmd_count;

void script_ext_init(void);

// Exposed for standalone unit tests on synthetic guest memory.
void script_ext_init_table(uint32_t vanilla_script_base,
                           uint32_t vanilla_console_base,
                           uint32_t console_count,
                           uint32_t parse_fn,
                           uint32_t extract_fn,
                           uint32_t console_print_fn,
                           uint32_t lookup_form_fn);

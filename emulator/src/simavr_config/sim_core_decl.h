// simavr normally generates this table of available cores. See sim_core_config.h.
#ifndef __SIM_CORE_DECL_H__
#define __SIM_CORE_DECL_H__

#include "sim_core_config.h"

extern avr_kind_t mega328;

extern avr_kind_t * avr_kind[];
#ifdef AVR_KIND_DECL
avr_kind_t * avr_kind[] = {
	&mega328,
	NULL
};
#endif
#endif

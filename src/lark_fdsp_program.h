/* See lark_fdsp_program.c. */
#ifndef HAVEN_LARK_FDSP_PROGRAM_H_
#define HAVEN_LARK_FDSP_PROGRAM_H_

#include <stdint.h>

#include "adau1860_regs.h"

#define LARK_FDSP_PROGRAM_WORDS 12
#define LARK_FDSP_NUM_SLOTS     12

/* Slot (instruction) indices in the upstream program. Slots 0-4 are the five
 * cascaded biquads Haven's bands land in; the rest are upstream's dynamics /
 * routing stages, kept as shipped.
 */
enum lark_fdsp_slot {
	LARK_FDSP_SLOT_BIQ_0 = 0,
	LARK_FDSP_SLOT_BIQ_1,
	LARK_FDSP_SLOT_BIQ_2,
	LARK_FDSP_SLOT_BIQ_3,
	LARK_FDSP_SLOT_BIQ_4,
	LARK_FDSP_SLOT_EXPANDER = 5,
	LARK_FDSP_SLOT_VOLUME = 6,
	LARK_FDSP_SLOT_MUTE = 7,
	LARK_FDSP_SLOT_MIXER = 8,
	LARK_FDSP_SLOT_LIMITER_MASTER = 9,
};
#define LARK_FDSP_NUM_BIQUADS 5

/* Upstream bank indices double as its "audio modes". */
#define LARK_FDSP_BANK_NORMAL       0
#define LARK_FDSP_BANK_TRANSPARENCY 1
#define LARK_FDSP_BANK_ANC          2

extern const uint32_t lark_fdsp_program[LARK_FDSP_PROGRAM_WORDS];
extern const uint32_t lark_fdsp_param_bank[ADAU1860_FDSP_NUM_BANKS][ADAU1860_FDSP_NUM_PARAMS]
					 [LARK_FDSP_NUM_SLOTS];

#endif /* HAVEN_LARK_FDSP_PROGRAM_H_ */

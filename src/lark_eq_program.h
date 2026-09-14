/* See lark_eq_program.c. */
#ifndef HAVEN_LARK_EQ_PROGRAM_H_
#define HAVEN_LARK_EQ_PROGRAM_H_

#include <stdint.h>

#define LARK_EQ_PROGRAM_WORDS      57
#define LARK_EQ_BANK_WORDS         35
#define LARK_EQ_NUM_BIQUADS        6   /* groups 0-5 of the bank */
#define LARK_EQ_PARAMS_PER_BIQUAD  5   /* [-a1, -a2, b0, b1, b2], 4.24 */
#define LARK_EQ_GAIN_WORDS         5   /* group 6: five unity gain words */

extern const uint32_t lark_eq_program[LARK_EQ_PROGRAM_WORDS];
extern const uint32_t lark_eq_param_bank0[LARK_EQ_BANK_WORDS];
extern const uint32_t lark_eq_param_bank1[LARK_EQ_BANK_WORDS];

#endif /* HAVEN_LARK_EQ_PROGRAM_H_ */

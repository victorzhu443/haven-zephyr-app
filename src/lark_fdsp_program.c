/* Upstream OpenEarable 2.0 FastDSP program for the ADAU1860 ("Lark"), as
 * Ported from OpenEarable/open-earable-2 {path} (OpenEarable project,
 * TECO / KIT). Original distributed under the license in
 * third_party/open-earable-2/LICENSE; modifications for Haven 2026.
 *
 * exported from ADI's Lark Studio ("Download to Target" writes exactly these
 * words to FDSP program/parameter memory -- EVAL-ADAU1860 User Guide UG-2017)
 * and checked in upstream as src/drivers/Lark-fdsp.c
 * (OpenEarable/open-earable-2, LicenseRef-PCFT / Nordic-5-Clause; used here
 * on an nRF5340 as that license requires). Values are verbatim; only the
 * array names and layout comments are Haven's.
 *
 * What this program is (decoded from the parameter banks, see
 * docs/fastdsp-program.md): the DMIC (in-ear microphone) stream, frame-clocked
 * at the DMIC rate, through five cascaded biquads (slots 0-4), then an
 * expander/noise gate (5), volume (6), mute (7), a mixer with the I2S/EQ music
 * path (8) and a master limiter (9), out on FDSP channel 0 -> DAC. Bank 0
 * (upstream "normal") zeroes the five biquads, i.e. mutes the mic path; bank 1
 * ("transparency") is the hear-through EQ; bank 2 ("ANC"). Haven runs bank 1
 * and overwrites slots 0-4 with its own notch/peaking-cut bands via safeload.
 *
 * Layout: lark_fdsp_param_bank[bank][param][slot] -- one 32-bit word per
 * slot, per parameter, exactly how the memory is organised (see
 * ADAU1860_FDSP_BANK() in adau1860_regs.h). Biquad slots use
 * param0..4 = b0, b1, b2, -a1, -a2 in Q5.27 (see adau1860_control.c).
 */
#include "lark_fdsp_program.h"

const uint32_t lark_fdsp_program[LARK_FDSP_PROGRAM_WORDS] = {
	0x00824800, 0x00B24800, 0x00B24800, 0x00B24800, 0x00B24800, 0x01005000, 0x01804800, 0x01805200, 0x01C04800, 0x04800000, 0x00000000, 0x00000000
};

const uint32_t lark_fdsp_param_bank[ADAU1860_FDSP_NUM_BANKS][ADAU1860_FDSP_NUM_PARAMS][LARK_FDSP_NUM_SLOTS] = {
	/* bank 0 -- upstream AUDIO_MODE_NORMAL: biquads zeroed (mic path silent) */
	{
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000C80,
		  0x17F00000, 0xC9F00000, 0xD1A40000, 0xC8000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000D00,
		  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000300,
		  0x02014700, 0x00014700, 0x08000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000300,
		  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000,
		  0x04000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
	},
	/* bank 1 -- upstream AUDIO_MODE_TRANSPARENCY: hear-through EQ */
	{
		{ 0x07FE81DE, 0x0790CF27, 0x006DC082, 0x04097BA8, 0x083C4385, 0x08000C80,
		  0x17F00000, 0xC9F00000, 0xD1A40000, 0xC8000000, 0x00000000, 0x00000000 },
		{ 0xF00A380A, 0xF1435005, 0x00DB8104, 0xF806CE83, 0xF03C9B90, 0x04000D00,
		  0x00000000, 0x00000000, 0xFD000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x07F74A0F, 0x07463618, 0x006DC082, 0x03EFFE9E, 0x078BFEC9, 0x00010000,
		  0x02014700, 0x00014700, 0x08000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x0FF5C5FD, 0x0EBCAFFB, 0x0B15CAC3, 0x0FE8B753, 0x0FC36470, 0x00004000,
		  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xF80A321A, 0xF928FAC0, 0xFB333334, 0xF816B773, 0xF837BDB2, 0x82180000,
		  0x04000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
	},
	/* bank 2 -- upstream AUDIO_MODE_ANC */
	{
		{ 0x08000000, 0x00006DB1, 0x07F32FFB, 0x08000000, 0x08000000, 0x08000C80,
		  0x17F00000, 0xC9F00000, 0xD1A40000, 0xC8000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x0000DB63, 0xF03A7E18, 0x00000000, 0x00000000, 0x04000D00,
		  0x00000000, 0x00000000, 0x01000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x00006DB1, 0x07D2780D, 0x00000000, 0x00000000, 0x00010000,
		  0x02014700, 0x00014700, 0x08000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x0FBBE982, 0x0FC558B0, 0x00000000, 0x00000000, 0x00004000,
		  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0xF841EE1B, 0xF83A2EBE, 0x00000000, 0x00000000, 0x82180000,
		  0x04000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
	},
};

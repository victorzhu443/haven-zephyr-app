/* Upstream OpenEarable 2.0 hardware-EQ program for the ADAU1860 ("Lark"), as
 * ported from OpenEarable/open-earable-2 src/drivers/Lark-eq.c (OpenEarable
 * project, TECO / KIT). Original distributed under the license in
 * third_party/open-earable-2/LICENSE; modifications for Haven 2026.
 *
 * Exported from ADI's Lark Studio (EQ designer -> "Download to Target") and
 * checked in upstream verbatim; values are unchanged here, only the names.
 *
 * What it is (decoded in tools/dsp/eq_bank_decode.py): a 57-word program for
 * the codec's dedicated EQ engine plus two 35-word parameter banks. Each bank
 * is six biquads of five words followed by five gain words, all in 28-bit
 * two's-complement 4.24 fixed point (1.0 = 0x01000000). Biquad layout is
 * [-a1, -a2, b0, b1, b2] -- feedback taps first and negated. Upstream's bank
 * 0 is a four-band music EQ (-10/-8/-2.5/-7 dB cuts) with two identity
 * stages; bank 1 is six identical stages. Haven loads only the PROGRAM from
 * here and writes its own banks (unity at boot, its bands at runtime); the
 * upstream banks are kept for tests and for the decode script.
 */
#include "lark_eq_program.h"

const uint32_t lark_eq_program[LARK_EQ_PROGRAM_WORDS] = {
	0x00010000, 0x00024F00, 0x0000C000, 0x00001018, 0x00014F98, 0x0000C000, 0x00001018,
	0x00015018, 0x0000C000, 0x00001018, 0x00014094, 0x0000C015, 0x0000C216, 0x0000C197,
	0x0000C118, 0x0000A018, 0x00001016, 0x00014310, 0x0000C291, 0x0000C492, 0x0000C413,
	0x0000C396, 0x0000A014, 0x00001012, 0x0001458C, 0x0000C50D, 0x0000C70E, 0x0000C68F,
	0x0000C612, 0x0000A010, 0x0000100E, 0x00014808, 0x0000C789, 0x0000C98A, 0x0000C90B,
	0x0000C88E, 0x0000A00C, 0x0000100A, 0x00014A84, 0x0000CA05, 0x0000CC06, 0x0000CB87,
	0x0000CB0A, 0x0000A008, 0x00001006, 0x00014D00, 0x0000CC81, 0x0000CE82, 0x0000CE03,
	0x0000CD86, 0x0000A004, 0x00001002, 0x00028000, 0x00018000, 0x0003C000, 0x00000000,
	0x00000000
};

const uint32_t lark_eq_param_bank0[LARK_EQ_BANK_WORDS] = {
	0x01E51476, 0x0F1A1CBD, 0x00C76C97, 0x0E7E7E85, 0x00BA5649,
	0x018A7D5F, 0x0F4540FB, 0x00EB2887, 0x0E7582A1, 0x00CF967E,
	0x012C8DDB, 0x0F8528EA, 0x00EF5915, 0x0ED37225, 0x008B7E01,
	0x017D88EB, 0x0F310B0E, 0x00F26E8A, 0x0E827715, 0x00DC8668,
	0x00000000, 0x00000000, 0x01000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x01000000, 0x00000000, 0x00000000,
	0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000
};

const uint32_t lark_eq_param_bank1[LARK_EQ_BANK_WORDS] = {
	0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF,
	0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF,
	0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF,
	0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF,
	0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF,
	0x01DE28C5, 0x0F1DB6F9, 0x0101D018, 0x0E21D73B, 0x00E078EF,
	0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000
};

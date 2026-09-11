/* ADAU1860 ("Lark") control-port register map.
 * Ported from OpenEarable/open-earable-2 {path} (OpenEarable project,
 * TECO / KIT). Original distributed under the license in
 * third_party/open-earable-2/LICENSE; modifications for Haven 2026.
 *
 *
 * Transcribed from the register list in OpenEarable 2.0's ADAU1860 driver
 * (OpenEarable/open-earable-2, src/drivers/ADAU1860.h -- LicenseRef-PCFT /
 * Nordic-5-Clause; use here is on an nRF5340, as that license requires),
 * which itself mirrors the ADAU1860 datasheet's register map. Names are kept
 * identical to upstream's so the two drivers can be diffed side by side.
 *
 * Addresses are 32-bit and are sent big-endian on the control port
 * (4 address bytes, then data) -- see adau1860_control.c. This is NOT the
 * 16-bit addressing of the older SigmaDSP family (ADAU14xx/17xx); the
 * earlier scaffold in this repo assumed that and would have addressed
 * nothing on this chip.
 */
#ifndef HAVEN_ADAU1860_REGS_H_
#define HAVEN_ADAU1860_REGS_H_

#include <stdint.h>

/* -- FastDSP (FDSP) memories ------------------------------------------------
 * The FDSP is a fixed-function per-sample engine: a short program of
 * instructions (one per "slot"), each reading 5 parameters from the active
 * parameter bank. Three banks (A/B/C) exist so a whole parameter set can be
 * swapped atomically (upstream uses them as audio modes: 0 normal,
 * 1 transparency, 2 ANC). Parameter memory is laid out per-parameter, not
 * per-slot: bank K, parameter N is a contiguous block of one 32-bit word per
 * slot at FDSP_BANK(K, N).
 */
#define ADAU1860_FDSP_PROG_MEM        0x40008000u
#define ADAU1860_FDSP_NUM_BANKS       3
#define ADAU1860_FDSP_NUM_PARAMS      5
#define ADAU1860_FDSP_PARAM_SIZE      0x100u
#define ADAU1860_FDSP_BANK(k, n) \
	(0x40008100u + (uint32_t)(k) * ADAU1860_FDSP_PARAM_SIZE * ADAU1860_FDSP_NUM_PARAMS + \
	 (uint32_t)(n) * ADAU1860_FDSP_PARAM_SIZE)
#define ADAU1860_FDSP_STATE           0x40009000u

/* Hardware EQ engine memories (separate from the FDSP; unused by Haven so
 * far -- kept for the EQ_ROUTE hear-through experiment described in
 * docs/fastdsp-program.md).
 */
#define ADAU1860_EQ_PROG_MEM          0x4000A000u
#define ADAU1860_EQ_BANK_0            0x4000A200u
#define ADAU1860_EQ_BANK_1            0x4000A400u

/* DAC_ROUTE0 source selectors observed in upstream. */
#define ADAU1860_DAC_ROUTE_I2S        0
#define ADAU1860_DAC_ROUTE_FDSP_CH(n) (32 + (n))
#define ADAU1860_DAC_ROUTE_EQ         75

/* -- Control registers ------------------------------------------------------ */

#define ADAU1860_REG_VENDOR_ID             0x4000C000u  /* ADI Vendor ID */
#define ADAU1860_REG_DEVICE_ID1            0x4000C001u  /* Device ID 1 */
#define ADAU1860_REG_DEVICE_ID2            0x4000C002u  /* Device ID 2 */
#define ADAU1860_REG_REVISION              0x4000C003u  /* Revision Code */
#define ADAU1860_REG_ADC_DAC_HP_PWR        0x4000C004u  /* ADC, DAC, Headphone Power Controls */
#define ADAU1860_REG_PLL_PGA_PWR           0x4000C005u  /* PLL, Mic Bias, and PGA Power Controls */
#define ADAU1860_REG_DMIC_PWR              0x4000C006u  /* Digital Mic Power Controls */
#define ADAU1860_REG_SAI_CLK_PWR           0x4000C007u  /* Serial Port, PDM Output, and DMIC CLK Power Controls */
#define ADAU1860_REG_DSP_PWR               0x4000C008u  /* DSP Power Controls */
#define ADAU1860_REG_ASRC_PWR              0x4000C009u  /* ASRC Power Controls */
#define ADAU1860_REG_FINT_PWR              0x4000C00Au  /* Interpolator Power Controls */
#define ADAU1860_REG_FDEC_PWR              0x4000C00Bu  /* Decimator Power Controls */
#define ADAU1860_REG_KEEP_CM               0x4000C00Cu  /* State Retention Controls */
#define ADAU1860_REG_MEM_RETAIN            0x4000C00Du  /* Retention Control for ADP and SOC Memories */
#define ADAU1860_REG_CHIP_PWR              0x4000C00Eu  /* Chip Power Control */
#define ADAU1860_REG_CLK_CTRL1             0x4000C010u  /* Clock Control */
#define ADAU1860_REG_CLK_CTRL2             0x4000C011u  /* PLL Input Divider */
#define ADAU1860_REG_CLK_CTRL3             0x4000C012u  /* PLL Feedback Integer Divider (LSBs) */
#define ADAU1860_REG_CLK_CTRL4             0x4000C013u  /* PLL Feedback Integer Divider (MSBs) */
#define ADAU1860_REG_CLK_CTRL5             0x4000C014u  /* PLL Fractional numerator value (LSBs) */
#define ADAU1860_REG_CLK_CTRL6             0x4000C015u  /* PLL Fractional numerator value (MSBs) */
#define ADAU1860_REG_CLK_CTRL7             0x4000C016u  /* PLL Fractional denominator (LSBs) */
#define ADAU1860_REG_CLK_CTRL8             0x4000C017u  /* PLL Fractional denominator (MSBs) */
#define ADAU1860_REG_CLK_CTRL9             0x4000C018u  /* PLL Update */
#define ADAU1860_REG_CLK_CTRL10            0x4000C019u  /* TDSP Clock Rate, BUS(AHB/APB) Clock Rate */
#define ADAU1860_REG_CLK_CTRL11            0x4000C01Au  /* AON and UART_CTRL Clock Rate */
#define ADAU1860_REG_CLK_CTRL12            0x4000C01Bu  /* Frequency Multiplier Enable, Ratio */
#define ADAU1860_REG_CLK_CTRL13            0x4000C01Cu  /* PLL Frequency Index and ADC Frequency Index */
#define ADAU1860_REG_CLK_CTRL14            0x4000C01Du  /* ADP_Clock_Enable, Low byte */
#define ADAU1860_REG_CLK_CTRL15            0x4000C01Eu  /* ADP_Clock_Enable, High byte */
#define ADAU1860_REG_ADC_CTRL1             0x4000C020u  /* ADC Sample Rate Control */
#define ADAU1860_REG_ADC_CTRL2             0x4000C021u  /* ADC IBias Controls */
#define ADAU1860_REG_ADC_CTRL3             0x4000C022u  /* ADC IBias Controls */
#define ADAU1860_REG_ADC_CTRL4             0x4000C023u  /* ADC HPF Control */
#define ADAU1860_REG_ADC_CTRL5             0x4000C024u  /* ADC Mute and Compensation Control */
#define ADAU1860_REG_ADC_CTRL6             0x4000C025u  /* Analog Input Pre-Charge Time */
#define ADAU1860_REG_ADC_CTRL7             0x4000C026u  /* Analog Input Pre-Charge Time */
#define ADAU1860_REG_ADC_MUTES             0x4000C027u  /* ADC Channel Mutes */
#define ADAU1860_REG_ADC0_VOL              0x4000C028u  /* ADC Channel 0 Volume Control */
#define ADAU1860_REG_ADC1_VOL              0x4000C029u  /* ADC Channel 1 Volume Control */
#define ADAU1860_REG_ADC2_VOL              0x4000C02Au  /* ADC Channel 2 Volume Control */
#define ADAU1860_REG_ADC_DITHER_LEV        0x4000C02Bu  /* ADC Dithering level */
#define ADAU1860_REG_PGA0_CTRL1            0x4000C030u  /* PGA Channel 0 Gain Control LSB's */
#define ADAU1860_REG_PGA0_CTRL2            0x4000C031u  /* PGA Channel 0 Gain Control MSB's */
#define ADAU1860_REG_PGA1_CTRL1            0x4000C032u  /* PGA Channel 1 Gain Control LSB's */
#define ADAU1860_REG_PGA1_CTRL2            0x4000C033u  /* PGA Channel 1 Gain Control MSB's */
#define ADAU1860_REG_PGA2_CTRL1            0x4000C034u  /* PGA Channel 2 Gain Control LSB's */
#define ADAU1860_REG_PGA2_CTRL2            0x4000C035u  /* PGA Channel 2 Gain Control MSB's */
#define ADAU1860_REG_PGA_CTRL1             0x4000C036u  /* PGA Slew Rate and Gain Link */
#define ADAU1860_REG_PGA_CTRL2             0x4000C037u  /* PGA RIN and Power Mode */
#define ADAU1860_REG_DMIC_CTRL1            0x4000C040u  /* DMIC clock rate control */
#define ADAU1860_REG_DMIC_CTRL2            0x4000C041u  /* DMIC Channels 0 and 1 rate, order, mapping, and edge control */
#define ADAU1860_REG_DMIC_CTRL3            0x4000C042u  /* DMIC Channels 2 and 3 rate, order, mapping, and edge control */
#define ADAU1860_REG_DMIC_CTRL4            0x4000C043u  /* DMIC Volume Options */
#define ADAU1860_REG_DMIC_MUTES            0x4000C044u  /* DMIC Channel Mute Controls */
#define ADAU1860_REG_DMIC_VOL0             0x4000C045u  /* DMIC Channel 0 Volume Control */
#define ADAU1860_REG_DMIC_VOL1             0x4000C046u  /* DMIC Channel 1 Volume Control */
#define ADAU1860_REG_DMIC_VOL2             0x4000C047u  /* DMIC Channel 2 Volume Control */
#define ADAU1860_REG_DMIC_VOL3             0x4000C048u  /* DMIC Channel 3 Volume Control */
#define ADAU1860_REG_DMIC_CTRL5            0x4000C049u  /* DMIC Channels 4 and 5 rate, order, mapping, and edge control */
#define ADAU1860_REG_DMIC_CTRL6            0x4000C04Au  /* DMIC Channels 6 and 7 rate, order, mapping, and edge control */
#define ADAU1860_REG_DMIC_CTRL7            0x4000C04Bu  /* DMIC Clock Map, DMIC Clock 1 Source Pin Select */
#define ADAU1860_REG_DMIC_VOL4             0x4000C04Cu  /* DMIC Channel 4 Volume Control */
#define ADAU1860_REG_DMIC_VOL5             0x4000C04Du  /* DMIC Channel 5 Volume Control */
#define ADAU1860_REG_DMIC_VOL6             0x4000C04Eu  /* DMIC Channel 6 Volume Control */
#define ADAU1860_REG_DMIC_VOL7             0x4000C04Fu  /* DMIC Channel 7 Volume Control */
#define ADAU1860_REG_DAC_CTRL1             0x4000C050u  /* DAC Sample Rate, Filtering, and Power Controls */
#define ADAU1860_REG_DAC_CTRL2             0x4000C051u  /* DAC Volume Link, HPF, and Mute Controls */
#define ADAU1860_REG_DAC_VOL0              0x4000C052u  /* DAC Channel 0 Volume */
#define ADAU1860_REG_DAC_ROUTE0            0x4000C053u  /* DAC Channel 0 Routing */
#define ADAU1860_REG_HP_CTRL               0x4000C060u  /* Headphone control */
#define ADAU1860_REG_HP_LVMODE_CTRL1       0x4000C061u  /* HP Low voltage Mode Enable, CM Enable */
#define ADAU1860_REG_HP_LVMODE_CTRL2       0x4000C062u  /* HP Low Voltage Auto Switch Mode, Delay, CM Delay */
#define ADAU1860_REG_HP_LVMODE_CTRL3       0x4000C063u  /* HP Low Voltage Auto Switch(Go) */
#define ADAU1860_REG_HPLDO_CTRL            0x4000C066u  /* HPLDO CTRL */
#define ADAU1860_REG_PB_CTRL               0x4000C06Cu  /* HP Low voltage mode control */
#define ADAU1860_REG_PMU_CTRL1             0x4000C070u  /* Memory Power Control */
#define ADAU1860_REG_PMU_CTRL2             0x4000C071u  /* CM Control */
#define ADAU1860_REG_PMU_CTRL3             0x4000C072u  /* IRQ Wakeup Control */
#define ADAU1860_REG_PMU_CTRL4             0x4000C073u  /* IRQ Wakeup Control */
#define ADAU1860_REG_PMU_CTRL5             0x4000C074u  /* DLDO Control */
#define ADAU1860_REG_FDEC_CTRL1            0x4000C080u  /* Fast to Slow Decimator Sample Rates Channels 0 and 1 */
#define ADAU1860_REG_FDEC_CTRL2            0x4000C081u  /* Fast to Slow Decimator Sample Rates Channels 2 and 3 */
#define ADAU1860_REG_FDEC_CTRL3            0x4000C082u  /* Fast to Slow Decimator Sample Rates Channels 4 and 5 */
#define ADAU1860_REG_FDEC_CTRL4            0x4000C083u  /* Fast to Slow Decimator Sample Rates Channels 6 and 7 */
#define ADAU1860_REG_FDEC_ROUTE0           0x4000C084u  /* Fast to Slow Decimator Channel 0 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE1           0x4000C085u  /* Fast to Slow Decimator Channel 1 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE2           0x4000C086u  /* Fast to Slow Decimator Channel 2 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE3           0x4000C087u  /* Fast to Slow Decimator Channel 3 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE4           0x4000C088u  /* Fast to Slow Decimator Channel 4 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE5           0x4000C089u  /* Fast to Slow Decimator Channel 5 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE6           0x4000C08Au  /* Fast to Slow Decimator Channel 6 Input Routing */
#define ADAU1860_REG_FDEC_ROUTE7           0x4000C08Bu  /* Fast to Slow Decimator Channel 7 Input Routing */
#define ADAU1860_REG_FINT_CTRL1            0x4000C090u  /* Slow to Fast Interpolator Sample Rates Channels 0/1 */
#define ADAU1860_REG_FINT_CTRL2            0x4000C091u  /* Slow to Fast Interpolator Sample Rates Channels 2/3 */
#define ADAU1860_REG_FINT_CTRL3            0x4000C092u  /* Slow to Fast Interpolator Sample Rates Channels 4/5 */
#define ADAU1860_REG_FINT_CTRL4            0x4000C093u  /* Slow to Fast Interpolator Sample Rates Channels 6/7 */
#define ADAU1860_REG_FINT_ROUTE0           0x4000C094u  /* Slow to Fast Interpolator Channel 0 Input Routing */
#define ADAU1860_REG_FINT_ROUTE1           0x4000C095u  /* Slow to Fast Interpolator Channel 1 Input Routing */
#define ADAU1860_REG_FINT_ROUTE2           0x4000C096u  /* Slow to Fast Interpolator Channel 2 Input Routing */
#define ADAU1860_REG_FINT_ROUTE3           0x4000C097u  /* Slow to Fast Interpolator Channel 3 Input Routing */
#define ADAU1860_REG_FINT_ROUTE4           0x4000C098u  /* Slow to Fast Interpolator Channel 4 Input Routing */
#define ADAU1860_REG_FINT_ROUTE5           0x4000C099u  /* Slow to Fast Interpolator Channel 5 Input Routing */
#define ADAU1860_REG_FINT_ROUTE6           0x4000C09Au  /* Slow to Fast Interpolator Channel 6 Input Routing */
#define ADAU1860_REG_FINT_ROUTE7           0x4000C09Bu  /* Slow to Fast Interpolator Channel 7 Input Routing */
#define ADAU1860_REG_ASRCI_CTRL            0x4000C0A0u  /* Input ASRC Control, Source, and Rate Selection */
#define ADAU1860_REG_ASRCI_ROUTE01         0x4000C0A1u  /* Input ASRC Channel 0 and 1 Input Routing */
#define ADAU1860_REG_ASRCI_ROUTE23         0x4000C0A2u  /* Input ASRC Channel 2 and 3 Input Routing */
#define ADAU1860_REG_ASRCO_CTRL            0x4000C0A3u  /* Output ASRC Control */
#define ADAU1860_REG_ASRCO_ROUTE0          0x4000C0A4u  /* Output ASRC Channel 0 Input Routing */
#define ADAU1860_REG_ASRCO_ROUTE1          0x4000C0A5u  /* Output ASRC Channel 1 Input Routing */
#define ADAU1860_REG_ASRCO_ROUTE2          0x4000C0A6u  /* Output ASRC Channel 2 Input Routing */
#define ADAU1860_REG_ASRCO_ROUTE3          0x4000C0A7u  /* Output ASRC Channel 3 Input Routing */
#define ADAU1860_REG_FDSP_RUN              0x4000C0B0u  /* FastDSP Run */
#define ADAU1860_REG_FDSP_CTRL1            0x4000C0B1u  /* FastDSP Current Bank and Bank Ramping Controls */
#define ADAU1860_REG_FDSP_CTRL2            0x4000C0B2u  /* FastDSP Bank Ramping Stop Point */
#define ADAU1860_REG_FDSP_CTRL3            0x4000C0B3u  /* FastDSP Bank Copying */
#define ADAU1860_REG_FDSP_CTRL4            0x4000C0B4u  /* FastDSP Frame Rate Source */
#define ADAU1860_REG_FDSP_CTRL5            0x4000C0B5u  /* FastDSP Fixed Rate Division MSBs */
#define ADAU1860_REG_FDSP_CTRL6            0x4000C0B6u  /* FastDSP Fixed Rate Division LSBs */
#define ADAU1860_REG_FDSP_CTRL7            0x4000C0B7u  /* FastDSP Modulo N Counter for Lower Rate Conditional Execution */
#define ADAU1860_REG_FDSP_CTRL8            0x4000C0B8u  /* FastDSP Generic Conditional Execution Registers */
#define ADAU1860_REG_FDSP_SL_ADDR          0x4000C0B9u  /* Fast DSP Safeload Address */
#define ADAU1860_REG_FDSP_SL_P0_0          0x4000C0BAu  /* FastDSP Safeload Parameter 0 Value */
#define ADAU1860_REG_FDSP_SL_P0_1          0x4000C0BBu  /* FastDSP Safeload Parameter 0 Value */
#define ADAU1860_REG_FDSP_SL_P0_2          0x4000C0BCu  /* FastDSP Safeload Parameter 0 Value */
#define ADAU1860_REG_FDSP_SL_P0_3          0x4000C0BDu  /* FastDSP Safeload Parameter 0 Value */
#define ADAU1860_REG_FDSP_SL_P1_0          0x4000C0BEu  /* FastDSP Safeload Parameter 1 Value */
#define ADAU1860_REG_FDSP_SL_P1_1          0x4000C0BFu  /* FastDSP Safeload Parameter 1 Value */
#define ADAU1860_REG_FDSP_SL_P1_2          0x4000C0C0u  /* FastDSP Safeload Parameter 1 Value */
#define ADAU1860_REG_FDSP_SL_P1_3          0x4000C0C1u  /* FastDSP Safeload Parameter 1 Value */
#define ADAU1860_REG_FDSP_SL_P2_0          0x4000C0C2u  /* FastDSP Safeload Parameter 2 Value */
#define ADAU1860_REG_FDSP_SL_P2_1          0x4000C0C3u  /* FastDSP Safeload Parameter 2 Value */
#define ADAU1860_REG_FDSP_SL_P2_2          0x4000C0C4u  /* FastDSP Safeload Parameter 2 Value */
#define ADAU1860_REG_FDSP_SL_P2_3          0x4000C0C5u  /* FastDSP Safeload Parameter 2 Value */
#define ADAU1860_REG_FDSP_SL_P3_0          0x4000C0C6u  /* FastDSP Safeload Parameter 3 Value */
#define ADAU1860_REG_FDSP_SL_P3_1          0x4000C0C7u  /* FastDSP Safeload Parameter 3 Value */
#define ADAU1860_REG_FDSP_SL_P3_2          0x4000C0C8u  /* FastDSP Safeload Parameter 3 Value */
#define ADAU1860_REG_FDSP_SL_P3_3          0x4000C0C9u  /* FastDSP Safeload Parameter 3 Value */
#define ADAU1860_REG_FDSP_SL_P4_0          0x4000C0CAu  /* FastDSP Safeload Parameter 4 Value */
#define ADAU1860_REG_FDSP_SL_P4_1          0x4000C0CBu  /* FastDSP Safeload Parameter 4 Value */
#define ADAU1860_REG_FDSP_SL_P4_2          0x4000C0CCu  /* FastDSP Safeload Parameter 4 Value */
#define ADAU1860_REG_FDSP_SL_P4_3          0x4000C0CDu  /* FastDSP Safeload Parameter 4 Value */
#define ADAU1860_REG_FDSP_SL_UPDATE        0x4000C0CEu  /* FastDSP Safeload Update */
#define ADAU1860_REG_FDSP_ONZ_MASK0        0x4000C0CFu  /* FastDSP ONZ Mask 0 */
#define ADAU1860_REG_FDSP_ONZ_MASK1        0x4000C0D0u  /* FastDSP ONZ Mask 1 */
#define ADAU1860_REG_FDSP_ONZ_MASK2        0x4000C0D1u  /* FastDSP ONZ Mask 2 */
#define ADAU1860_REG_EQ_CFG                0x4000C0D2u  /* EQ Configure */
#define ADAU1860_REG_EQ_ROUTE              0x4000C0D3u  /* EQ Routing */
#define ADAU1860_REG_TDSP_SOFT_RESET       0x4000C0D4u  /* Tensilica DSP Software Reset */
#define ADAU1860_REG_TDSP_ALTVEC_EN        0x4000C0D5u  /* Tensilica DSP Alternative Reset Vector Enable */
#define ADAU1860_REG_TDSP_ALTVEC_ADDR0     0x4000C0D8u  /* Tensilica DSP Alternative Reset Vector Address Byte 0 */
#define ADAU1860_REG_TDSP_ALTVEC_ADDR1     0x4000C0D9u  /* Tensilica DSP Alternative Reset Vector Address Byte 1 */
#define ADAU1860_REG_TDSP_ALTVEC_ADDR2     0x4000C0DAu  /* Tensilica DSP Alternative Reset Vector Address Byte 2 */
#define ADAU1860_REG_TDSP_ALTVEC_ADDR3     0x4000C0DBu  /* Tensilica DSP Alternative Reset Vector Address Byte 3 */
#define ADAU1860_REG_TDSP_RUN              0x4000C0DCu  /* Tensilica DSP Run */
#define ADAU1860_REG_SPT0_CTRL1            0x4000C0E0u  /* Serial Port 0 Control 1 */
#define ADAU1860_REG_SPT0_CTRL2            0x4000C0E1u  /* Serial Port 0 Control 2 */
#define ADAU1860_REG_SPT0_CTRL3            0x4000C0E2u  /* Serial Port 0 Control 3 */
#define ADAU1860_REG_SPT0_ROUTE0           0x4000C0E3u  /* Serial Port 0 Output Routing Slot 0 (Left) */
#define ADAU1860_REG_SPT0_ROUTE1           0x4000C0E4u  /* Serial Port 0 Output Routing Slot 1 (Right) */
#define ADAU1860_REG_SPT0_ROUTE2           0x4000C0E5u  /* Serial Port 0 Output Routing Slot 2 */
#define ADAU1860_REG_SPT0_ROUTE3           0x4000C0E6u  /* Serial Port 0 Output Routing Slot 3 */
#define ADAU1860_REG_SPT0_ROUTE4           0x4000C0E7u  /* Serial Port 0 Output Routing Slot 4 */
#define ADAU1860_REG_SPT0_ROUTE5           0x4000C0E8u  /* Serial Port 0 Output Routing Slot 5 */
#define ADAU1860_REG_SPT0_ROUTE6           0x4000C0E9u  /* Serial Port 0 Output Routing Slot 6 */
#define ADAU1860_REG_SPT0_ROUTE7           0x4000C0EAu  /* Serial Port 0 Output Routing Slot 7 */
#define ADAU1860_REG_SPT0_ROUTE8           0x4000C0EBu  /* Serial Port 0 Output Routing Slot 8 */
#define ADAU1860_REG_SPT0_ROUTE9           0x4000C0ECu  /* Serial Port 0 Output Routing Slot 9 */
#define ADAU1860_REG_SPT0_ROUTE10          0x4000C0EDu  /* Serial Port 0 Output Routing Slot 10 */
#define ADAU1860_REG_SPT0_ROUTE11          0x4000C0EEu  /* Serial Port 0 Output Routing Slot 11 */
#define ADAU1860_REG_SPT0_ROUTE12          0x4000C0EFu  /* Serial Port 0 Output Routing Slot 12 */
#define ADAU1860_REG_SPT0_ROUTE13          0x4000C0F0u  /* Serial Port 0 Output Routing Slot 13 */
#define ADAU1860_REG_SPT0_ROUTE14          0x4000C0F1u  /* Serial Port 0 Output Routing Slot 14 */
#define ADAU1860_REG_SPT0_ROUTE15          0x4000C0F2u  /* Serial Port 0 Output Routing Slot 15 */
#define ADAU1860_REG_SPT1_CTRL1            0x4000C0F3u  /* Serial Port 1 Control 1 */
#define ADAU1860_REG_SPT1_CTRL2            0x4000C0F4u  /* Serial Port 1 Control 2 */
#define ADAU1860_REG_SPT1_CTRL3            0x4000C0F5u  /* Serial Port 1 Control 3 */
#define ADAU1860_REG_SPT1_ROUTE0           0x4000C0F6u  /* Serial Port 1 Output Routing Slot 0 (Left) */
#define ADAU1860_REG_SPT1_ROUTE1           0x4000C0F7u  /* Serial Port 1 Output Routing Slot 1 (Right) */
#define ADAU1860_REG_SPT1_ROUTE2           0x4000C0F8u  /* Serial Port 1 Output Routing Slot 2 */
#define ADAU1860_REG_SPT1_ROUTE3           0x4000C0F9u  /* Serial Port 1 Output Routing Slot 3 */
#define ADAU1860_REG_SPT1_ROUTE4           0x4000C0FAu  /* Serial Port 1 Output Routing Slot 4 */
#define ADAU1860_REG_SPT1_ROUTE5           0x4000C0FBu  /* Serial Port 1 Output Routing Slot 5 */
#define ADAU1860_REG_SPT1_ROUTE6           0x4000C0FCu  /* Serial Port 1 Output Routing Slot 6 */
#define ADAU1860_REG_SPT1_ROUTE7           0x4000C0FDu  /* Serial Port 1 Output Routing Slot 7 */
#define ADAU1860_REG_SPT1_ROUTE8           0x4000C0FEu  /* Serial Port 1 Output Routing Slot 8 */
#define ADAU1860_REG_SPT1_ROUTE9           0x4000C0FFu  /* Serial Port 1 Output Routing Slot 9 */
#define ADAU1860_REG_SPT1_ROUTE10          0x4000C100u  /* Serial Port 1 Output Routing Slot 10 */
#define ADAU1860_REG_SPT1_ROUTE11          0x4000C101u  /* Serial Port 1 Output Routing Slot 11 */
#define ADAU1860_REG_SPT1_ROUTE12          0x4000C102u  /* Serial Port 1 Output Routing Slot 12 */
#define ADAU1860_REG_SPT1_ROUTE13          0x4000C103u  /* Serial Port 1 Output Routing Slot 13 */
#define ADAU1860_REG_SPT1_ROUTE14          0x4000C104u  /* Serial Port 1 Output Routing Slot 14 */
#define ADAU1860_REG_SPT1_ROUTE15          0x4000C105u  /* Serial Port 1 Output Routing Slot 15 */
#define ADAU1860_REG_PDM_CTRL1             0x4000C118u  /* PDM Sample Rate and Filtering Control */
#define ADAU1860_REG_PDM_CTRL2             0x4000C119u  /* PDM Muting, High-Pass, and Volume Options */
#define ADAU1860_REG_PDM_VOL0              0x4000C11Au  /* PDM Output Channel 0 Volume */
#define ADAU1860_REG_PDM_VOL1              0x4000C11Bu  /* PDM Output Channel 1 Volume */
#define ADAU1860_REG_PDM_ROUTE0            0x4000C11Cu  /* PDM Output Channel 0 Routing */
#define ADAU1860_REG_PDM_ROUTE1            0x4000C11Du  /* PDM Output Channel 1 Routing */
#define ADAU1860_REG_MP_CTRL1              0x4000C121u  /* MultiPurpose Pins 0/1 Mode Select */
#define ADAU1860_REG_MP_CTRL2              0x4000C122u  /* MultiPurpose Pins 2/3 Mode Select */
#define ADAU1860_REG_MP_CTRL3              0x4000C123u  /* MultiPurpose Pins 4/5 Mode Select */
#define ADAU1860_REG_MP_CTRL4              0x4000C124u  /* MultiPurpose Pins 6/7 Mode Select */
#define ADAU1860_REG_MP_CTRL5              0x4000C125u  /* MultiPurpose Pins 8/9 Mode Select */
#define ADAU1860_REG_MP_CTRL6              0x4000C126u  /* MultiPurpose Pin 10/11 Mode Select */
#define ADAU1860_REG_MP_CTRL7              0x4000C127u  /* MultiPurpose Pin 12/13 Mode Select */
#define ADAU1860_REG_MP_CTRL8              0x4000C128u  /* MultiPurpose Pin 14/15 Mode Select */
#define ADAU1860_REG_MP_CTRL9              0x4000C129u  /* MultiPurpose Pin 16/17 Mode Select */
#define ADAU1860_REG_MP_CTRL10             0x4000C12Au  /* MultiPurpose Pin 18/19 Mode Select */
#define ADAU1860_REG_MP_CTRL11             0x4000C12Bu  /* MultiPurpose Pin 20/21 Mode Select */
#define ADAU1860_REG_MP_CTRL12             0x4000C12Cu  /* MultiPurpose Pin 22/23 Mode Select */
#define ADAU1860_REG_MP_CTRL13             0x4000C12Du  /* MultiPurpose Pin 24/25 Mode Select */
#define ADAU1860_REG_MP_DB_CTRL            0x4000C12Eu  /* General Purpose Input Debounce Control and IRQ Input Debounce Control */
#define ADAU1860_REG_MP_MCLKO_RATE         0x4000C12Fu  /* MCLKO Rate Selection */
#define ADAU1860_REG_MP_GPIO_CTRL1         0x4000C130u  /* General Purpose Outputs Control Pins 0-7 */
#define ADAU1860_REG_MP_GPIO_CTRL2         0x4000C131u  /* General Purpose Outputs Control Pins 8-15 */
#define ADAU1860_REG_MP_GPIO_CTRL3         0x4000C132u  /* General Purpose Outputs Control Pins 16-23 */
#define ADAU1860_REG_MP_GPIO_CTRL4         0x4000C133u  /* General Purpose Outputs Control Pins 24-25 */
#define ADAU1860_REG_DMIC_CLK_CTRL         0x4000C134u  /* DMIC_CLK Pin Controls */
#define ADAU1860_REG_DMIC01_CTRL           0x4000C135u  /* DMIC01 Pin Controls */
#define ADAU1860_REG_DMIC23_CTRL           0x4000C136u  /* DMIC23 Pin Controls */
#define ADAU1860_REG_BCLK0_CTRL            0x4000C137u  /* BCLK_0 Pin Controls */
#define ADAU1860_REG_FSYNC0_CTRL           0x4000C138u  /* FSYNC_0 Pin Controls */
#define ADAU1860_REG_SDATAO0_CTRL          0x4000C139u  /* SDATAO_0 Pin Controls */
#define ADAU1860_REG_SDATAI0_CTRL          0x4000C13Au  /* SDATAI_0 Pin Controls */
#define ADAU1860_REG_BCLK1_CTRL            0x4000C13Bu  /* BCLK_1 Pin Controls */
#define ADAU1860_REG_FSYNC1_CTRL           0x4000C13Cu  /* FSYNC_1 Pin Controls */
#define ADAU1860_REG_SDATAO1_CTRL          0x4000C13Du  /* SDATAO_1 Pin Controls */
#define ADAU1860_REG_SDATAI1_CTRL          0x4000C13Eu  /* SDATAI_1 Pin Controls */
#define ADAU1860_REG_QSPIM_CLK_CTRL        0x4000C13Fu  /* QSPIM_CLK Pin Controls */
#define ADAU1860_REG_QSPIM_CS_CTRL         0x4000C140u  /* QSPIM_CS Pin Controls */
#define ADAU1860_REG_QSPIM_SDIO0_CTRL      0x4000C141u  /* QSPIM_SDIO0 Pin Controls */
#define ADAU1860_REG_QSPIM_SDIO1_CTRL      0x4000C142u  /* QSPIM_SDIO1 Pin Controls */
#define ADAU1860_REG_QSPIM_SDIO2_CTRL      0x4000C143u  /* QSPIM_SDIO2 Pin Controls */
#define ADAU1860_REG_QSPIM_SDIO3_CTRL      0x4000C144u  /* QSPIM_SDIO3 Pin Controls */
#define ADAU1860_REG_UART_COMM_TX_CTRL     0x4000C145u  /* UART_COMM_TX Pin Controls */
#define ADAU1860_REG_UART_COMM_RX_CTRL     0x4000C146u  /* UART_COMM_RX Pin Controls */
#define ADAU1860_REG_SELFBOOT_CTRL         0x4000C147u  /* SELFBOOT Pin Controls */
#define ADAU1860_REG_IRQ_CTRL              0x4000C148u  /* IRQ Pin Controls */
#define ADAU1860_REG_ROM_BOOT_MODE_CTRL    0x4000C149u  /* ROM Boot Mode Pin Controls */
#define ADAU1860_REG_TCK_CTRL              0x4000C14Au  /* TCK Pin Controls */
#define ADAU1860_REG_TMS_CTRL              0x4000C14Bu  /* TMS Pin Controls */
#define ADAU1860_REG_TDO_CTRL              0x4000C14Cu  /* TDO Pin Controls */
#define ADAU1860_REG_TDI_CTRL              0x4000C14Du  /* TDI Pin Controls */
#define ADAU1860_REG_I2C_SPI_CTRL          0x4000C14Eu  /* SDA/MISO Pin Controls */
#define ADAU1860_REG_IRQ_CTRL1             0x4000C150u  /* IRQ signaling and clearing */
#define ADAU1860_REG_IRQ1_MASK1            0x4000C151u  /* IRQ1 Masking */
#define ADAU1860_REG_IRQ1_MASK2            0x4000C152u  /* IRQ1 Masking */
#define ADAU1860_REG_IRQ1_MASK3            0x4000C153u  /* IRQ1 Masking */
#define ADAU1860_REG_IRQ1_MASK4            0x4000C154u  /* IRQ1 Masking */
#define ADAU1860_REG_IRQ1_MASK5            0x4000C155u  /* IRQ1 Masking */
#define ADAU1860_REG_IRQ2_MASK1            0x4000C156u  /* IRQ2 Masking */
#define ADAU1860_REG_IRQ2_MASK2            0x4000C157u  /* IRQ2 Masking */
#define ADAU1860_REG_IRQ2_MASK3            0x4000C158u  /* IRQ2 Masking */
#define ADAU1860_REG_IRQ2_MASK4            0x4000C159u  /* IRQ2 Masking */
#define ADAU1860_REG_IRQ2_MASK5            0x4000C15Au  /* IRQ2 Masking */
#define ADAU1860_REG_IRQ3_MASK1            0x4000C15Bu  /* IRQ3 Masking */
#define ADAU1860_REG_IRQ3_MASK2            0x4000C15Cu  /* IRQ3 Masking */
#define ADAU1860_REG_IRQ3_MASK3            0x4000C15Du  /* IRQ3 Masking */
#define ADAU1860_REG_IRQ3_MASK4            0x4000C15Eu  /* IRQ3 Masking */
#define ADAU1860_REG_IRQ3_MASK5            0x4000C15Fu  /* IRQ3 Masking */
#define ADAU1860_REG_IRQ4_MASK1            0x4000C160u  /* IRQ4 Masking */
#define ADAU1860_REG_IRQ4_MASK2            0x4000C161u  /* IRQ4 Masking */
#define ADAU1860_REG_IRQ4_MASK3            0x4000C162u  /* IRQ4 Masking */
#define ADAU1860_REG_IRQ4_MASK4            0x4000C163u  /* IRQ4 Masking */
#define ADAU1860_REG_IRQ4_MASK5            0x4000C164u  /* IRQ4 Masking */
#define ADAU1860_REG_SW_INT                0x4000C165u  /* Software Interrupts which can be set by external host or TDSP */
#define ADAU1860_REG_MP_INPUT_IRQ_CTRL1    0x4000C168u  /* MP IRQ clearing */
#define ADAU1860_REG_MP_INPUT_IRQ_CTRL2    0x4000C169u  /* MP IRQ clearing */
#define ADAU1860_REG_MP_INPUT_IRQ1_MASK1   0x4000C16Au  /* MP IRQ1 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ1_MASK2   0x4000C16Bu  /* MP IRQ1 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ1_MASK3   0x4000C16Cu  /* MP IRQ1 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ1_MASK4   0x4000C16Du  /* MP IRQ1 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ2_MASK1   0x4000C16Eu  /* MP IRQ2 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ2_MASK2   0x4000C16Fu  /* MP IRQ2 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ2_MASK3   0x4000C170u  /* MP IRQ2 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ2_MASK4   0x4000C171u  /* MP IRQ2 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ3_MASK1   0x4000C172u  /* MP IRQ3 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ3_MASK2   0x4000C173u  /* MP IRQ3 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ3_MASK3   0x4000C174u  /* MP IRQ3 Masking */
#define ADAU1860_REG_MP_INPUT_IRQ3_MASK4   0x4000C175u  /* MP IRQ3 Masking */
#define ADAU1860_REG_RESETS                0x4000C200u  /* Chip Resets */
#define ADAU1860_REG_READ_LAMBDA           0x4000C400u  /* FastDSP Current Lambda */
#define ADAU1860_REG_STATUS1               0x4000C401u  /* Chip Status 1 */
#define ADAU1860_REG_STATUS2               0x4000C402u  /* Chip Status 2 */
#define ADAU1860_REG_SELFBOOT_STATUS       0x4000C403u  /* Tensilica DSP Self-boot Indicator */
#define ADAU1860_REG_EQ_STATUS             0x4000C404u  /* EQ Status */
#define ADAU1860_REG_FDSP_STATUS           0x4000C405u  /* FastDSP ONZ Status */
#define ADAU1860_REG_PMU_STATUS1           0x4000C406u  /* Power Mode Status, DLDO Scale Busy, CM Delay Counter Done */
#define ADAU1860_REG_PMU_STATUS2           0x4000C407u  /* Memory Retention Status */
#define ADAU1860_REG_PMU_STATUS3           0x4000C408u  /* ADP Memory Shutdown Status */
#define ADAU1860_REG_PMU_STATUS4           0x4000C409u  /* SOC Memory Shutdown Status */
#define ADAU1860_REG_TDSP_MODE_STATUS      0x4000C40Au  /* TDSP Mode Status */
#define ADAU1860_REG_TDSP_ERROR_STATUS     0x4000C40Bu  /* TDSP Exception/Error Status */
#define ADAU1860_REG_TDSP_FAULT_INFO1      0x4000C40Cu  /* TDSP Fault Information */
#define ADAU1860_REG_TDSP_FAULT_INFO2      0x4000C40Du  /* TDSP Fault Information */
#define ADAU1860_REG_TDSP_FAULT_INFO3      0x4000C40Eu  /* TDSP Fault Information */
#define ADAU1860_REG_TDSP_FAULT_INFO4      0x4000C40Fu  /* TDSP Fault Information */
#define ADAU1860_REG_TDSP_GPO1             0x4000C410u  /* TDSP General Purpose Output Bit 0~7 */
#define ADAU1860_REG_TDSP_GPO2             0x4000C411u  /* TDSP General Purpose Output Bit 8~15 */
#define ADAU1860_REG_TDSP_GPO3             0x4000C412u  /* TDSP General Purpose Output Bit 16~23 */
#define ADAU1860_REG_TDSP_GPO4             0x4000C413u  /* TDSP General Purpose Output Bit 24~31 */
#define ADAU1860_REG_IRQ1_STATUS1          0x4000C414u  /* IRQ1 Status 1 */
#define ADAU1860_REG_IRQ1_STATUS2          0x4000C415u  /* IRQ1 Status 2 */
#define ADAU1860_REG_IRQ1_STATUS3          0x4000C416u  /* IRQ1 Status 3 */
#define ADAU1860_REG_IRQ1_STATUS4          0x4000C417u  /* IRQ1 Status 4 */
#define ADAU1860_REG_IRQ1_STATUS5          0x4000C418u  /* IRQ1 Status 5 */
#define ADAU1860_REG_IRQ2_STATUS1          0x4000C419u  /* IRQ2 Status 1 */
#define ADAU1860_REG_IRQ2_STATUS2          0x4000C41Au  /* IRQ2 Status 2 */
#define ADAU1860_REG_IRQ2_STATUS3          0x4000C41Bu  /* IRQ2 Status 3 */
#define ADAU1860_REG_IRQ2_STATUS4          0x4000C41Cu  /* IRQ2 Status 4 */
#define ADAU1860_REG_IRQ2_STATUS5          0x4000C41Du  /* IRQ2 Status 5 */
#define ADAU1860_REG_IRQ3_STATUS1          0x4000C41Eu  /* IRQ3 Status 1 */
#define ADAU1860_REG_IRQ3_STATUS2          0x4000C41Fu  /* IRQ3 Status 2 */
#define ADAU1860_REG_IRQ3_STATUS3          0x4000C420u  /* IRQ3 Status 3 */
#define ADAU1860_REG_IRQ3_STATUS4          0x4000C421u  /* IRQ3 Status 4 */
#define ADAU1860_REG_IRQ3_STATUS5          0x4000C422u  /* IRQ3 Status 5 */
#define ADAU1860_REG_IRQ4_STATUS1          0x4000C423u  /* IRQ4 Status 1 */
#define ADAU1860_REG_IRQ4_STATUS2          0x4000C424u  /* IRQ4 Status 2 */
#define ADAU1860_REG_IRQ4_STATUS3          0x4000C425u  /* IRQ4 Status 3 */
#define ADAU1860_REG_IRQ4_STATUS4          0x4000C426u  /* IRQ4 Status 4 */
#define ADAU1860_REG_IRQ4_STATUS5          0x4000C427u  /* IRQ4 Status 5 */
#define ADAU1860_REG_MP_INPUT_IRQ1_STATUS1 0x4000C428u  /* Multi Purpose IRQ1 Status 1 */
#define ADAU1860_REG_MP_INPUT_IRQ1_STATUS2 0x4000C429u  /* Multi Purpose IRQ1 Status 2 */
#define ADAU1860_REG_MP_INPUT_IRQ1_STATUS3 0x4000C42Au  /* Multi Purpose IRQ1 Status 3 */
#define ADAU1860_REG_MP_INPUT_IRQ1_STATUS4 0x4000C42Bu  /* Multi Purpose IRQ1 Status 4 */
#define ADAU1860_REG_MP_INPUT_IRQ2_STATUS1 0x4000C42Cu  /* Multi Purpose IRQ2 Status 1 */
#define ADAU1860_REG_MP_INPUT_IRQ2_STATUS2 0x4000C42Du  /* Multi Purpose IRQ2 Status 2 */
#define ADAU1860_REG_MP_INPUT_IRQ2_STATUS3 0x4000C42Eu  /* Multi Purpose IRQ2 Status 3 */
#define ADAU1860_REG_MP_INPUT_IRQ2_STATUS4 0x4000C42Fu  /* Multi Purpose IRQ2 Status 4 */
#define ADAU1860_REG_MP_INPUT_IRQ3_STATUS1 0x4000C430u  /* Multi Purpose IRQ3 Status 1 */
#define ADAU1860_REG_MP_INPUT_IRQ3_STATUS2 0x4000C431u  /* Multi Purpose IRQ3 Status 2 */
#define ADAU1860_REG_MP_INPUT_IRQ3_STATUS3 0x4000C432u  /* Multi Purpose IRQ3 Status 3 */
#define ADAU1860_REG_MP_INPUT_IRQ3_STATUS4 0x4000C433u  /* Multi Purpose IRQ3 Status 4 */
#define ADAU1860_REG_GPI1                  0x4000C434u  /* General Purpose Input Read 0-7 */
#define ADAU1860_REG_GPI2                  0x4000C435u  /* General Purpose Input Read 8-15 */
#define ADAU1860_REG_GPI3                  0x4000C436u  /* General Purpose Input Read 16-20 */
#define ADAU1860_REG_GPI4                  0x4000C437u  /* General Purpose Input Read 16-20 */
#define ADAU1860_REG_CTRL_PORT_MODE        0x4000C438u  /* Control Port Mode */
#define ADAU1860_REG_DAC_NOISE_CTRL2       0x4000CC04u  /* DAC/PLL Test Modes */
#define ADAU1860_REG_DAC_NOISE_CTRL1       0x4000CC12u  /* DAC Noise Control1 */

#endif /* HAVEN_ADAU1860_REGS_H_ */

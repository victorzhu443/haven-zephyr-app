# third_party/open-earable-2

License text for material ported from the OpenEarable 2.0 firmware
(https://github.com/OpenEarable/open-earable-2, OpenEarable project,
TECO / KIT). Its repository LICENSE is Nordic Semiconductor's 5-clause
nRF53 license (`LicenseRef-PCFT`): redistribution with or without
modification is permitted provided the notice, conditions and disclaimer
are retained, and the software is only used with a Nordic nRF53 chip —
which Haven's nRF5340 satisfies.

Files in this repo derived from it (each carries a header saying so):

- `src/adau1860_regs.h` ← `src/drivers/ADAU1860.h` (register map)
- `src/lark_fdsp_program.c` ← `src/drivers/Lark-fdsp.c` (FastDSP program,
  verbatim data)
- `src/adau1860_control.c` ← `src/drivers/ADAU1860.cpp` (bring-up sequence,
  safeload / volume / mute recipes; C port)

The board definition under `boards/teco/openearable_v2/` is from the same
repository but carries its own per-file header
(`Copyright (c) 2023 Raytac Corporation, SPDX-License-Identifier: Apache-2.0`);
the Apache-2.0 text sits next to it in `boards/teco/openearable_v2/LICENSE`.
`dts/bindings/load-switch.yaml` is Libre Solar Technologies, Apache-2.0
(header intact); the other copied bindings carry their upstream headers.

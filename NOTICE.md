# Licences of kode-mesh-sdk

This file says which licence applies to each part of this repository. Read it before you distribute the SDK, or a firmware that you built with it.

| Part | Owner | Licence |
|---|---|---|
| everything in this repository | Kode | GPL-3.0-or-later, with the additional permission below |

This repository holds no code of another owner. The end of this file lists what a build downloads.

The SDK links [`kode-sdk`](https://github.com/kodediy/kode-sdk), which is GPL-3.0-or-later, so a firmware built with both is a covered work of that licence. `kode-sdk`'s own `NOTICE.md` says what applies to its parts.

## The code of Kode

Copyright (C) 2026 Kode.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The file `LICENSE` holds the full text of the GNU General Public License, version 3.

### Additional permission under GNU GPL version 3 section 7

An app built with this SDK also links `kode-sdk`, and `kode-sdk` holds RFAL, the NFC library of STMicroelectronics. The licence of RFAL, SLA0051, is not an open source licence. This repository holds no code of STMicroelectronics. The permission below is the one that `kode-sdk` gives for its own code, so that an app can be built with both.

If you modify this Program, or any covered work, by linking or combining it with the RF Abstraction Layer (RFAL) and the ST25R3916 driver of STMicroelectronics (or a modified version of those libraries), containing parts covered by the terms of the STMicroelectronics SLA0051 licence, the licensors of this Program grant you additional permission to convey the resulting work. Corresponding Source for a non-source form of such a combination shall include the source code for the parts of RFAL and of the ST25R3916 driver used as well as that of the covered work.

If you change the code of Kode, you can remove this permission from your version. Section 7 of the GNU General Public License allows this.

### Trademarks, under GNU GPL version 3 section 7(e)

This licence gives no right to use the names Kode, Kode Dot, kodeOS and Kody, or the character Kody, as trademarks. `TRADEMARKS.md` in the repository `kodediy/.github` says how you can use them.

## The drivers of the module's chips

The drivers of the LR2021 radio and of the GNSS receiver are code of Kode. They were written from the datasheets of the two chips and from Kode's own firmware for the module. No code of a chip maker is copied or quoted in this repository. Semtech's driver of the LR2021 was read for one thing: the names of four error bits that the datasheet marks as reserved.

## The components that the build downloads

The ESP-IDF component manager downloads other components when you build an app. They are not in this repository. Each one has its own licence, in its folder under `managed_components/`.

| Component | Owner | Licence | Used for |
|---|---|---|---|
| `kode-sdk` (`kodediy/kode` and the layers below it) | Kode, Quantum Leaps and STMicroelectronics | its own `NOTICE.md` says which applies where | the Kode Dot under the module |

The host suites under `tests/` also clone `kode-sdk`, at the same release, for its headers and its copy of QP/C. ESP-IDF itself comes with the toolchain that you install.

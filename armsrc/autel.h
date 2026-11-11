//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Autel AT100 programmable transponder - device side
//-----------------------------------------------------------------------------

#ifndef AUTEL_ARM_H__
#define AUTEL_ARM_H__

#include "common.h"
#include "../include/autel.h"

void Autel_Read(const lf_autel_io_t *packet, bool ledcontrol);
void Autel_Write(const lf_autel_io_t *packet, bool ledcontrol);
void Autel_Dump(const lf_autel_io_t *packet, bool ledcontrol);
void Autel_Configure(const lf_autel_cfg_t *packet, uint16_t cmd, bool ledcontrol);

#endif // AUTEL_ARM_H__

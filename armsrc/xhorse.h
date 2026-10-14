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
// Xhorse super-chip (XT27A) firmware - channel-A management protocol
//-----------------------------------------------------------------------------

#ifndef XHORSE_FW_H
#define XHORSE_FW_H

#include "common.h"
#include "../include/xhorse.h"

// dispatch functions (appmain.c)
void Xhorse_Info(bool ledcontrol);
void Xhorse_Dump(bool ledcontrol);
void Xhorse_Configure(const lf_xhorse_data_t *packet, bool ledcontrol);
void Xhorse_Reset(bool ledcontrol);

#endif // XHORSE_FW_H

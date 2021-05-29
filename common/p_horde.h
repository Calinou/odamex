// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id$
//
// Copyright (C) 1998-2006 by Randy Heit (ZDoom).
// Copyright (C) 2006-2020 by The Odamex Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//  "Horde" game mode.
//
//-----------------------------------------------------------------------------

#pragma once

#include "actor.h"
#include "doomdata.h"
#include "p_hordedefine.h"

enum hordeState_e
{
	HS_STARTING,
	HS_PRESSURE,
	HS_RELAX,
	HS_BOSS,
};

struct hordeInfo_t
{
	hordeState_e state;
	int round;
	int alive;
	int killed;
	int goal;
};

hordeInfo_t P_HordeInfo();
void P_AddHealthPool(AActor* mo);
void P_RemoveHealthPool(AActor* mo);

void P_RunHordeTics();
bool P_IsHordeMode();
bool P_IsHordeThing(const int type);
const hordeDefine_t::weapons_t& P_HordeWeapons();
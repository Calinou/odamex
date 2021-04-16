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
//  "Death Wish" game mode.
//
//-----------------------------------------------------------------------------

#include "p_dwish.h"

#include <math.h>

#include "c_dispatch.h"
#include "d_player.h"
#include "doomstat.h"
#include "m_random.h"
#include "m_vectors.h"
#include "p_local.h"
#include "p_tick.h"
#include "s_sound.h"

bool P_LookForPlayers(AActor* actor, bool allaround);

struct roundDefine_t
{
	int spawns;          // Number of spawns for this round.
	int monsterHealth;   // Maximum health of a single monster spawn.
	int bossHealth;      // Maximum health of a single "boss" monster spawn.
	int minGroupHealth;  // Minimum health of a group of monsters to spawn.
	int maxGroupHealth;  // Maximum health of a group of monsters to spawn.
	int minGlobalHealth; // Lower bound on the amount of health in the map at once, aside
	                     // from round end.
	int maxGlobalHealth; // Upper bound on the amount of health in the map at once.
};

class HordeRoundState
{
	const roundDefine_t ROUND_DEFINES[3] = {
	    // Round 1
	    {
	        20,   // spawns
	        150,  // monsterHealth
	        150,  // bossHealth
	        150,  // minGroupHealth
	        300,  // maxGroupHealth
	        600,  // minGlobalHealth
	        1200, // maxGlobalHealth
	    },
	    // Round 2
	    {
	        30,   // spawns
	        600,  // monsterHealth
	        700,  // bossHealth
	        600,  // minGroupHealth
	        1200, // maxGroupHealth
	        2000, // minGlobalHealth
	        4800, // maxGlobalHealth
	    },
	    // Round 3
	    {
	        40,   // spawns
	        1000, // monsterHealth
	        4000, // bossHealth
	        1000, // minGroupHealth
	        2000, // maxGroupHealth
	        4000, // minGlobalHealth
	        8000, // maxGlobalHealth
	    }};

	int m_round; // 0-indexed

  public:
	int getRound() const
	{
		return m_round + 1;
	}

	const roundDefine_t& getDefine() const
	{
		return ROUND_DEFINES[m_round];
	}

	void setRound(const int round)
	{
		m_round = round - 1;
	}
};

namespace spawner
{
struct recipe_t
{
	mobjtype_t type;
	int count;
};

typedef std::vector<mobjtype_t> mobjTypes_t;

static bool CmpHealth(const mobjtype_t& a, const mobjtype_t& b)
{
	return ::mobjinfo[a].height < ::mobjinfo[b].height;
}

static const mobjTypes_t& GatherMonsters()
{
	// [AM] FIXME: This will break on WAD reload, this is just "for now".
	static mobjTypes_t all;

	if (all.empty())
	{
		for (size_t i = 0; i < ARRAY_LENGTH(::mobjinfo); i++)
		{
			// [AM] FIXME: Don't hardcode mobj exceptions, this basically makes
			//             that mobj unusable even when using DEH/BEX.
			if (i == MT_DOGS || i == MT_WOLFSS)
				continue;

			// Is this a monster?
			if (!(::mobjinfo[i].flags & MF_COUNTKILL))
				continue;

			// Does this monster have any attacks?  (Skips keen)
			if (::mobjinfo[i].missilestate == S_NULL &&
			    ::mobjinfo[i].meleestate == S_NULL)
				continue;

			// Is this monster a flying monster?
			if (::mobjinfo[i].flags & (MF_FLOAT | MF_NOGRAVITY))
			{
				// Monster is a flying monster.
				all.push_back(static_cast<mobjtype_t>(i));
			}
			else if (::mobjinfo[i].missilestate == S_NULL &&
			         ::mobjinfo[i].meleestate != S_NULL)
			{
				// Monster is melee-only monster.
				all.push_back(static_cast<mobjtype_t>(i));
			}
			else
			{
				// Monster is a ranged monster.
				all.push_back(static_cast<mobjtype_t>(i));
			}
		}

		// Sort monsters by health.
		std::sort(all.begin(), all.end(), CmpHealth);
	}

	return all;
}

static recipe_t GetSpawn(const HordeRoundState& roundState)
{
	recipe_t result;
	mobjTypes_t types;

	// Figure out which monster we want to spawn.
	const roundDefine_t& define = roundState.getDefine();
	const mobjTypes_t& all = GatherMonsters();
	for (mobjTypes_t::const_iterator it = all.begin(); it != all.end(); ++it)
	{
		const mobjinfo_t& info = ::mobjinfo[*it];
		if (info.spawnhealth <= 0 || info.spawnhealth > define.monsterHealth)
			continue;

		types.push_back(*it);
	}

	size_t resultIdx = P_Random() % types.size();
	result.type = types.at(resultIdx);

	// Figure out how many monsters we can spawn of our given type - at least one.
	const int upper = MAX(define.maxGroupHealth / ::mobjinfo[resultIdx].spawnstate, 1);
	const int lower = MAX(define.minGlobalHealth / ::mobjinfo[resultIdx].spawnstate, 1);
	if (upper == lower)
	{
		// Only one possibility.
		result.count = upper;
		return result;
	}

	result.count = (P_Random() % upper - lower) + lower;
	return result;
}
}; // namespace spawner

class HordeState
{
	enum state_e
	{
		DS_STARTING,
		DS_PRESSURE,
		DS_RELAX,
	};

	state_e m_state;
	int m_stateTime;
	HordeRoundState m_roundState;
	int m_spawnedHealth;
	int m_killedHealth;

	void setState(const state_e state)
	{
		m_state = state;
		m_stateTime = ::gametic;
	}

  public:
	void reset()
	{
		setState(DS_STARTING);
		m_roundState.setRound(1);
		m_spawnedHealth = 0;
		m_killedHealth = 0;
	}

	void addSpawnHealth(const int health)
	{
		m_spawnedHealth += health;
	}

	void addKilledHealth(const int health)
	{
		m_killedHealth += health;
	}

	HordeRoundState& getRoundState()
	{
		return m_roundState;
	}

	bool shouldSpawn()
	{
		int aliveHealth = m_spawnedHealth - m_killedHealth;

		switch (m_state)
		{
		case DS_STARTING:
			setState(DS_PRESSURE);
		case DS_PRESSURE: {
			Printf("PRESSURE | a:%d > max:%d?\n", aliveHealth,
			       m_roundState.getDefine().maxGlobalHealth);
			if (aliveHealth > m_roundState.getDefine().maxGlobalHealth)
			{
				setState(DS_RELAX);
				return false;
			}
			return true;
		}
		case DS_RELAX: {
			Printf("RELAX | a:%d < max:%d?\n", aliveHealth,
			       m_roundState.getDefine().minGlobalHealth);
			if (aliveHealth < m_roundState.getDefine().minGlobalHealth)
			{
				setState(DS_PRESSURE);
				return true;
			}
			return false;
		}
		default:
			return 0;
		}
	}
} gDirector;

struct SpawnPoint
{
	AActor::AActorPtr mo;
};
typedef std::vector<SpawnPoint> SpawnPoints;
typedef std::vector<SpawnPoint*> SpawnPointView;

static SpawnPoints gSpawns;
static bool DEBUG_enabled;

void P_AddHordeSpawnPoint(AActor* mo)
{
	SpawnPoint sp;
	sp.mo = mo->self;
	::gSpawns.push_back(sp);
}

void P_AddHealthPool(AActor* mo)
{
	// Already added?
	if (mo->oflags & MFO_HEALTHPOOL)
		return;

	// Counts as a monster?  (Lost souls don't)
	if (!(mo->flags & MF_COUNTKILL))
		return;

	// Mark as part of the health pool for cleanup later
	mo->oflags |= MFO_HEALTHPOOL;

	::gDirector.addSpawnHealth(::mobjinfo[mo->type].spawnhealth);
}

void P_RemoveHealthPool(AActor* mo)
{
	// Not a part of the pool
	if (!(mo->oflags & MFO_HEALTHPOOL))
		return;

	::gDirector.addKilledHealth(::mobjinfo[mo->type].spawnhealth);
}

/**
 * @brief Spawn a monster.
 *
 * @param point Spawn point of the monster.
 * @param type Thing type.
 * @param ambush True if the spawn should be an out-of-sight ambush without
 *               a teleport flash.
 */
static bool HordeSpawn(SpawnPoint& spawn, const mobjtype_t type, const bool ambush)
{
	// Spawn the monster - possibly.
	AActor* mo = new AActor(spawn.mo->x, spawn.mo->y, spawn.mo->z, type);
	if (mo)
	{
		if (P_TestMobjLocation(mo))
		{
			// Don't respawn
			mo->flags |= MF_DROPPED;

			P_AddHealthPool(mo);
			if (P_LookForPlayers(mo, true))
			{
				// Play the see sound if we have one and it's not an ambush.
				if (!ambush && mo->info->seesound)
				{
					char sound[MAX_SNDNAME];

					strcpy(sound, mo->info->seesound);

					if (sound[strlen(sound) - 1] == '1')
					{
						sound[strlen(sound) - 1] = P_Random(mo) % 3 + '1';
						if (S_FindSound(sound) == -1)
							sound[strlen(sound) - 1] = '1';
					}

					S_Sound(mo, CHAN_VOICE, sound, 1, ATTN_NORM);
				}

				// If monster has a target (which they should by now)
				// force them to start chasing them immediately.
				if (mo->target)
					P_SetMobjState(mo, mo->info->seestate, true);
			}

			if (!ambush)
			{
				// Spawn a teleport fog if it's not an ambush.
				AActor* tele = new AActor(spawn.mo->x, spawn.mo->y, spawn.mo->z, MT_TFOG);
				S_Sound(tele, CHAN_VOICE, "misc/teleport", 1, ATTN_NORM);
			}
			return true;
		}
		else
		{
			// Spawn blocked.
			mo->Destroy();
		}
	}
	return false;
}

void P_RunHordeTics()
{
	// Move this function to tick inside someplace that can be paused.
	if (::paused)
		return;

	if (::level.time == 0)
		::gDirector.reset();

	// Only run logic once a second.
	if (!P_AtInterval(TICRATE))
		return;

	// Should we spawn a monster?
	if (::gDirector.shouldSpawn())
	{
		PlayerResults pr = PlayerQuery().execute();
		PlayersView::const_iterator it;
		for (it = pr.players.begin(); it != pr.players.end(); ++it)
		{
			player_t* player = *it;

			// Do not consider dead players.
			if (player->health <= 0)
				continue;

			// Spawn monsters at visible spawn points.
			SpawnPointView view;
			for (SpawnPoints::iterator sit = ::gSpawns.begin(); sit != ::gSpawns.end();
			     ++sit)
			{
				if (P_CheckSight(sit->mo, player->mo))
				{
					view.push_back(&*sit);
				}
			}

			if (view.empty())
				continue;

			size_t idx = P_Random() % view.size();
			SpawnPoint& spawn = *view.at(idx);

			spawner::recipe_t recipe = spawner::GetSpawn(::gDirector.getRoundState());
			HordeSpawn(spawn, recipe.type, false);
		}
	}
}

bool P_IsHordeMode()
{
	return ::DEBUG_enabled;
}

BEGIN_COMMAND(horde_on)
{
	::DEBUG_enabled = true;
}
END_COMMAND(horde_on)

BEGIN_COMMAND(horde_round)
{
	if (argc < 2)
	{
		Printf("horde_round <ROUND_NUMBER>\n");
	}

	int round = atoi(argv[1]);
	if (round < 1 || round > 3)
	{
		Printf("Round must be between 1-3.\n");
	}

	::gDirector.getRoundState().setRound(round);
}
END_COMMAND(horde_round)
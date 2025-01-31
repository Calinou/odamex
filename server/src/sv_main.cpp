// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id$
//
// Copyright (C) 2000-2006 by Sergey Makovkin (CSDoom .62).
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
//	SV_MAIN
//
//-----------------------------------------------------------------------------

#include "win32inc.h"
#ifdef _WIN32
    #include <winsock.h>
    #include <time.h>
#endif

#ifdef UNIX
#include <unistd.h>
#include <sys/time.h>
#endif

#include "doomtype.h"
#include "doomstat.h"
#include "gstrings.h"
#include "d_player.h"
#include "s_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "g_level.h"
#include "p_tick.h"
#include "p_local.h"
#include "p_inter.h"
#include "sv_main.h"
#include "sv_sqp.h"
#include "sv_sqpold.h"
#include "sv_master.h"
#include "i_system.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "m_argv.h"
#include "m_random.h"
#include "p_ctf.h"
#include "w_wad.h"
#include "w_ident.h"
#include "md5.h"
#include "p_mobj.h"
#include "p_unlag.h"
#include "sv_vote.h"
#include "sv_maplist.h"
#include "g_levelstate.h"
#include "g_gametype.h"
#include "sv_banlist.h"
#include "d_main.h"
#include "m_fileio.h"
#include "v_textcolors.h"
#include "p_lnspec.h"
#include "m_wdlstats.h"
#include "svc_message.h"

#include <algorithm>
#include <sstream>
#include <vector>

extern void G_DeferedInitNew (char *mapname);
extern level_locals_t level;

// Unnatural Level Progression.  True if we've used 'map' or another command
// to switch to a specific map out of order, otherwise false.
bool unnatural_level_progression;

// denis - game manipulation, but no fancy gfx
bool clientside = false, serverside = true;
bool predicting = false;
baseapp_t baseapp = server;

// [SL] 2011-07-06 - not really connected (playing back a netdemo)
// really only used clientside
bool        simulated_connection = false;

extern bool HasBehavior;
extern int mapchange;

bool step_mode = false;

std::set<byte> free_player_ids;

bool keysfound[NUMCARDS];		// Ch0wW : Found keys

// General server settings
EXTERN_CVAR(sv_motd)
EXTERN_CVAR(sv_hostname)
EXTERN_CVAR(sv_email)
EXTERN_CVAR(sv_website)
EXTERN_CVAR(sv_waddownload)
EXTERN_CVAR(sv_maxrate)
EXTERN_CVAR(sv_emptyreset)
EXTERN_CVAR(sv_emptyfreeze)
EXTERN_CVAR(sv_clientcount)
EXTERN_CVAR(sv_globalspectatorchat)
EXTERN_CVAR(sv_allowtargetnames)
EXTERN_CVAR(sv_flooddelay)
EXTERN_CVAR(sv_ticbuffer)
EXTERN_CVAR(sv_warmup)
EXTERN_CVAR(sv_sharekeys)
EXTERN_CVAR(sv_teamsinplay)
EXTERN_CVAR(g_winnerstays)
EXTERN_CVAR(debug_disconnect)

void SexMessage (const char *from, char *to, int gender,
	const char *victim, const char *killer);
Players::iterator SV_RemoveDisconnectedPlayer(Players::iterator it);
void P_PlayerLeavesGame(player_s* player);
bool P_LineSpecialMovesSector(byte special);

void SV_UpdateShareKeys(player_t& player);
std::string V_GetTeamColor(UserInfo userinfo);

CVAR_FUNC_IMPL (sv_maxclients)
{
	// Describes the max number of clients that are allowed to connect.
	int count = var.asInt();
	Players::iterator it = players.begin();
	while (it != players.end())
	{
		if (count <= 0)
		{
			MSG_WriteMarker(&(it->client.reliablebuf), svc_print);
			MSG_WriteByte(&(it->client.reliablebuf), PRINT_CHAT);
			MSG_WriteString(&(it->client.reliablebuf),
			                "Client limit reduced. Please try connecting again later.\n");
			SV_DropClient(*it);
			it = SV_RemoveDisconnectedPlayer(it);
		}
		else
		{
			count -= 1;
			++it;
		}
	}
}


CVAR_FUNC_IMPL (sv_maxplayers)
{
	int normalcount = 0;
	bool queueExists = false;

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		bool spectator = it->spectator || !it->ingame();

		if (it->QueuePosition > 0)
			queueExists = true;

		if (!spectator)
		{
			normalcount++;

			if (normalcount > var)
			{
				it->spectator = true;
				it->playerstate = PST_LIVE;
				it->joindelay = 0;

				for (Players::iterator pit = players.begin(); pit != players.end(); ++pit)
					SVC_PlayerMembers(pit->client.reliablebuf, *it, SVC_PM_SPECTATOR);

				SV_BroadcastPrintf ("%s became a spectator.\n", it->userinfo.netname.c_str());
				MSG_WriteMarker(&it->client.reliablebuf, svc_print);
				MSG_WriteByte(&it->client.reliablebuf, PRINT_CHAT);
				MSG_WriteString(&it->client.reliablebuf,
								"Active player limit reduced. You are now a spectator!\n");
			}
		}
	}

	if (queueExists)
		SV_ClearPlayerQueue();
}

// [AM] - Force extras on a team to become spectators.
CVAR_FUNC_IMPL (sv_maxplayersperteam)
{
	for (int i = 0; i < NUMTEAMS;i++)
	{
		int normalcount = 0;
		for (Players::iterator it = players.begin();it != players.end();++it)
		{
			bool spectator = it->spectator || !it->ingame();
			if (it->userinfo.team == i && it->ingame() && !spectator)
			{
				normalcount++;

				if (normalcount > var)
				{
					SV_SetPlayerSpec(*it, true);
					SV_PlayerPrintf(it->id, PRINT_HIGH, "Active player limit reduced. You are now a spectator!\n");
				}
			}
		}
	}
}

EXTERN_CVAR (sv_allowcheats)
EXTERN_CVAR (sv_fraglimit)
EXTERN_CVAR (sv_timelimit)
EXTERN_CVAR (sv_intermissionlimit)
EXTERN_CVAR (sv_maxcorpses)

// Action rules
EXTERN_CVAR (sv_allowexit)
EXTERN_CVAR (sv_fragexitswitch)
EXTERN_CVAR (sv_allowjump)
EXTERN_CVAR (sv_freelook)
EXTERN_CVAR (sv_infiniteammo)

// Teamplay/CTF
EXTERN_CVAR (sv_scorelimit)
EXTERN_CVAR (sv_friendlyfire)

// Survival
EXTERN_CVAR (g_lives)

// Private server settings
CVAR_FUNC_IMPL (join_password)
{
	if (strlen(var.cstring()))
		Printf("Join password set.");
	else
		Printf("Join password cleared.");
}

CVAR_FUNC_IMPL (rcon_password) // Remote console password.
{
	if(strlen(var.cstring()) < 5)
	{
		if(!strlen(var.cstring()))
			Printf("RCON password cleared.");
		else
		{
			Printf("RCON password must be at least 5 characters.");
			var.Set("");
		}
	}
	else
		Printf(PRINT_HIGH, "RCON password set.");
}


EXTERN_CVAR(sv_waddownloadcap)
CVAR_FUNC_IMPL(sv_maxrate)
{
	// sv_waddownloadcap can not be larger than sv_maxrate
	if (sv_waddownloadcap > var)
		sv_waddownloadcap.Set(var);

	for (Players::iterator it = players.begin();it != players.end();++it)
		it->client.rate = int(sv_maxrate);
}

CVAR_FUNC_IMPL (sv_waddownloadcap)
{
	// sv_waddownloadcap can not be larger than sv_maxrate
	if (var > sv_maxrate)
		var.Set(sv_maxrate);
}

CVAR_FUNC_IMPL(sv_sharekeys)
{
	if (var)
	{
		// Refresh it to everyone
		for (Players::iterator it = players.begin(); it != players.end(); ++it) {
			SV_UpdateShareKeys(*it);
		}
	}
}

client_c clients;


#define CLIENT_TIMEOUT 65 // 65 seconds

void SV_UpdateConsolePlayer(player_t &player);

void SV_CheckTeam (player_t & playernum);
team_t SV_GoodTeam (void);

void SV_SendServerSettings (player_t &pl);
void SV_ServerSettingChange (void);

// some doom functions
size_t P_NumPlayersOnTeam(team_t team);
size_t P_NumPlayersInGame();

// [AM] Flip a coin between heads and tails
BEGIN_COMMAND (coinflip) {
	std::string result;
	CMD_CoinFlip(result);

	SV_BroadcastPrintf("%s\n", result.c_str());
} END_COMMAND (coinflip)

void CMD_CoinFlip(std::string &result) {
	result = (P_Random() % 2 == 0) ? "Coin came up Heads." : "Coin came up Tails.";
	return;
}

// denis - kick player
BEGIN_COMMAND (kick) {
	std::vector<std::string> arguments = VectorArgs(argc, argv);
	std::string error;

	size_t pid;
	std::string reason;

	if (!CMD_KickCheck(arguments, error, pid, reason)) {
		Printf("Kick: %s.\n", error.c_str());
		return;
	}

	SV_KickPlayer(idplayer(pid), reason);
} END_COMMAND (kick)

// Kick command check.
bool CMD_KickCheck(std::vector<std::string> arguments, std::string &error,
				   size_t &pid, std::string &reason) {
	// Did we pass enough arguments?
	if (arguments.size() < 1) {
		error = "Need a player ID (try 'players') and optionally a reason.";
		return false;
	}

	// Did we actually pass a player number?
	std::istringstream buffer(arguments[0]);
	buffer >> pid;

	if (!buffer) {
		error = "Need a valid player number.";
		return false;
	}

	// Verify that the player we found is a valid client.
	player_t &player = idplayer(pid);

	if (!validplayer(player)) {
		std::ostringstream error_ss;
		error_ss << "could not find client " << pid << ".";
		error = error_ss.str();
		return false;
	}

	// Anything that is not the first argument is the reason.
	arguments.erase(arguments.begin());
	reason = JoinStrings(arguments, " ");

	return true;
}

// Kick a player.
void SV_KickPlayer(player_t &player, const std::string &reason) {
	// Avoid a segfault from an invalid player.
	if (!validplayer(player)) {
		return;
	}

	if (reason.empty())
		SV_BroadcastPrintf("%s was kicked from the server!\n", player.userinfo.netname.c_str());
	else
		SV_BroadcastPrintf("%s was kicked from the server! (Reason: %s)\n",
					player.userinfo.netname.c_str(), reason.c_str());

	player.client.displaydisconnect = false;
	SV_DropClient(player);
}

// Invalidate a player.
//
// Usually happens when corrupted messages are passed to the server by the
// client.  Reasons should only be seen by the server admin.  Player and client
// are both presumed unusable after function is done.
void SV_InvalidateClient(player_t &player, const std::string& reason)
{
	if (&(player.client) == NULL)
	{
		Printf("Player with NULL client fails security check (%s), client cannot be safely dropped.\n");
		return;
	}

	Printf("%s fails security check (%s), dropping client.\n", NET_AdrToString(player.client.address), reason.c_str());
	SV_PlayerPrintf(PRINT_ERROR, player.id,
	                "The server closed your connection for the following reason: %s.\n",
	                reason.c_str());
	SV_DropClient(player);
}

BEGIN_COMMAND (stepmode)
{
    if (step_mode)
        step_mode = false;
    else
        step_mode = true;

    return;
}
END_COMMAND (stepmode)

BEGIN_COMMAND (say)
{
	if (argc > 1)
	{
		std::string chat = C_ArgCombine(argc - 1, (const char **)(argv + 1));
		SV_BroadcastPrintf(PRINT_SERVERCHAT, "[console]: %s\n", chat.c_str());
	}
}
END_COMMAND (say)

void STACK_ARGS call_terms (void);

void SV_QuitCommand()
{
	call_terms();
	exit(EXIT_SUCCESS);
}

BEGIN_COMMAND (rquit)
{
	SV_SendReconnectSignal();

	SV_QuitCommand();
}
END_COMMAND (rquit)

BEGIN_COMMAND (quit)
{
	SV_QuitCommand();
}
END_COMMAND (quit)

// An alias for 'quit'
BEGIN_COMMAND (exit)
{
	SV_QuitCommand();
}
END_COMMAND (exit)

static void SendLevelState(SerializedLevelState sls)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t& cl = it->client;
		SVC_LevelState(cl.reliablebuf, sls);
	}
}

//
// SV_InitNetwork
//
void SV_InitNetwork (void)
{
    network_game = true;

	const char *v = Args.CheckValue ("-port");
    if (v)
    {
       localport = atoi (v);
       Printf (PRINT_HIGH, "using alternate port %i\n", localport);
    }
	else
	   localport = SERVERPORT;

	// set up a socket and net_message buffer
	InitNetCommon();

	// determine my name & address
	// NET_GetLocalAddress ();

	Printf("UDP Initialized.\n");

	const char *w = Args.CheckValue ("-maxclients");
	if (w)
	{
		sv_maxclients.Set(w); // denis - todo
	}

	step_mode = Args.CheckParm ("-stepmode");

	// [AM] Set up levelstate so it calls a netmessage broadcast function on change.
	::levelstate.setStateCB(SendLevelState);

	// Nes - Connect with the master servers. (If valid)
	SV_InitMasters();
}

//Get next free player. Will use the lowest available player id.
Players::iterator SV_GetFreeClient(void)
{
	if (players.size() >= sv_maxclients)
		return players.end();

	if (free_player_ids.empty())
	{
		// list of free ids needs to be initialized
		for (int i = 1; i < MAXPLAYERS; i++)
			free_player_ids.insert(i);
	}

	players.push_back(player_t());
	players.back().playerstate = PST_CONTACT;

	// generate player id
	std::set<byte>::iterator id = free_player_ids.begin();
	players.back().id = *id;
	free_player_ids.erase(id);

	// update tracking cvar
	sv_clientcount.ForceSet(players.size());

	// Return iterator pointing to the just-inserted player
	Players::iterator it = players.end();
	return --it;
}

player_t &SV_FindPlayerByAddr(void)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (NET_CompareAdr(it->client.address, net_from))
		   return *it;
	}

	return idplayer(0);
}

//
// SV_CheckTimeouts
// If a packet has not been received from a client in CLIENT_TIMEOUT
// seconds, drop the conneciton.
//
void SV_CheckTimeouts()
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (gametic - it->client.last_received == CLIENT_TIMEOUT * 35)
		    SV_DropClient(*it);
	}
}

//
// SV_RemoveDisconnectedPlayer
//
// [SL] 2011-05-18 - Destroy a player's mo actor and remove the player_t
// from the global players vector.  Update mo->player pointers.
// [AM] Updated to remove from a list instead.  Eventually, we should turn
//      this into Players::erase().
Players::iterator SV_RemoveDisconnectedPlayer(Players::iterator it)
{
	if (it == players.end() || !validplayer(*it))
		return players.end();

	int player_id = it->id;

	if (!it->spectator)
	{
		P_PlayerLeavesGame(&(*it));
		SV_UpdatePlayerQueuePositions(G_CanJoinGame, &(*it));
	}

	// remove player awareness from all actors
	AActor* mo;
	TThinkerIterator<AActor> iterator;
	while ((mo = iterator.Next()))
		mo->players_aware.unset(it->id);

	// remove this player's actor object
	if (it->mo)
	{
		if (sv_gametype == GM_CTF) //  [Toke - CTF]
			CTF_CheckFlags(*it);

		// [AM] AActor->Destroy() does not destroy the AActor for good, and also
		//      does not null the player reference.  We have to do it here to
		//      prevent actions on a zombie mobj from using a player that we
		//      already erased from the players list.
		it->mo->player = NULL;

		it->mo->Destroy();
		it->mo = AActor::AActorPtr();
	}

	// remove this player from the global players vector
	Players::iterator next;
	next = players.erase(it);
	free_player_ids.insert(player_id);

	Unlag::getInstance().unregisterPlayer(player_id);

	// update tracking cvar
	sv_clientcount.ForceSet(players.size());

	return next;
}

//
// SV_GetPackets
//
void SV_GetPackets()
{
	while (NET_GetPacket())
	{
		player_t &player = SV_FindPlayerByAddr();

		if (!validplayer(player)) // no client with net_from address
		{
			// apparently, someone is trying to connect
			if (gamestate == GS_LEVEL || gamestate == GS_INTERMISSION)
				SV_ConnectClient();

			continue;
		}
		else
		{
			if(player.playerstate != PST_DISCONNECT)
			{
				player.client.last_received = gametic;
				SV_ParseCommands(player);
			}
		}
	}
}


// Print a midscreen message to a client
void SV_MidPrint (const char *msg, player_t *p, int msgtime)
{
    client_t *cl = &p->client;

    MSG_WriteMarker(&cl->reliablebuf, svc_midprint);
    MSG_WriteString(&cl->reliablebuf, msg);
    if (msgtime)
        MSG_WriteShort(&cl->reliablebuf, msgtime);
    else
        MSG_WriteShort(&cl->reliablebuf, 0);
}

//
// SV_Sound
//
void SV_Sound (AActor *mo, byte channel, const char *name, byte attenuation)
{
	int sfx_id;
	client_t* cl;
	int x = 0, y = 0;

	sfx_id = S_FindSound (name);

	if (sfx_id > 255 || sfx_id < 0)
	{
		Printf (PRINT_HIGH, "SV_StartSound: range error. Sfx_id = %d\n", sfx_id);
		return;
	}

	if (mo)
	{
		x = mo->x;
		y = mo->y;
	}

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		cl = &(it->client);

		MSG_WriteMarker (&cl->netbuf, svc_startsound);
		if(mo)
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
		else
			MSG_WriteUnVarint(&cl->netbuf, 0);
		MSG_WriteLong (&cl->netbuf, x);
		MSG_WriteLong (&cl->netbuf, y);
		MSG_WriteByte (&cl->netbuf, channel);
		MSG_WriteByte (&cl->netbuf, sfx_id);
		MSG_WriteByte (&cl->netbuf, attenuation);
		MSG_WriteByte (&cl->netbuf, 255); // client calculates volume on its own
	}

}

void SV_Sound (player_t &pl, AActor *mo, byte channel, const char *name, byte attenuation)
{
	int sfx_id;
	int x = 0, y = 0;

	sfx_id = S_FindSound (name);

	if (sfx_id > 255 || sfx_id < 0)
	{
		Printf (PRINT_HIGH, "SV_StartSound: range error. Sfx_id = %d\n", sfx_id);
		return;
	}

	if(mo)
	{
		x = mo->x;
		y = mo->y;
	}

	client_t *cl = &pl.client;

	MSG_WriteMarker (&cl->netbuf, svc_startsound);
	if (mo == NULL)
		MSG_WriteUnVarint(&cl->netbuf, 0);
	else
		MSG_WriteUnVarint(&cl->netbuf, mo->netid);
	MSG_WriteLong (&cl->netbuf, x);
	MSG_WriteLong (&cl->netbuf, y);
	MSG_WriteByte (&cl->netbuf, channel);
	MSG_WriteByte (&cl->netbuf, sfx_id);
	MSG_WriteByte (&cl->netbuf, attenuation);
	MSG_WriteByte (&cl->netbuf, 255);		// client calculates volume on its own
}

//
// UV_SoundAvoidPlayer
// Sends a sound to clients, but doesn't send it to client 'player'.
//
void UV_SoundAvoidPlayer (AActor *mo, byte channel, const char *name, byte attenuation)
{
	int        sfx_id;
	client_t  *cl;

	if (!mo || !mo->player)
		return;

	player_t &pl = *mo->player;

	sfx_id = S_FindSound (name);

	if (sfx_id > 255 || sfx_id < 0)
	{
		Printf (PRINT_HIGH, "SV_StartSound: range error. Sfx_id = %d\n", sfx_id);
		return;
	}

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if(&pl == &*it)
			continue;

		cl = &(it->client);

		MSG_WriteMarker(&cl->netbuf, svc_startsound);
		MSG_WriteUnVarint(&cl->netbuf, mo->netid);
		MSG_WriteLong(&cl->netbuf, mo->x);
		MSG_WriteLong(&cl->netbuf, mo->y);
		MSG_WriteByte(&cl->netbuf, channel);
		MSG_WriteByte(&cl->netbuf, sfx_id);
		MSG_WriteByte(&cl->netbuf, attenuation);
		MSG_WriteByte(&cl->netbuf, 255); // client calculates volume on its own
	}
}

//
//	SV_SoundTeam
//	Sends a sound to players on the specified teams
//
void SV_SoundTeam (byte channel, const char* name, byte attenuation, int team)
{
	int sfx_id;

	client_t* cl;

	sfx_id = S_FindSound( name );

	if ( sfx_id > 255 || sfx_id < 0 )
	{
		Printf("SV_StartSound: range error. Sfx_id = %d\n", sfx_id );
		return;
	}

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (it->ingame() && it->userinfo.team == team)
		{
			cl = &(it->client);

			MSG_WriteMarker(&cl->netbuf, svc_startsound);
			// Set netid to 0 since it's not a sound originating from any player's location
			MSG_WriteUnVarint(&cl->netbuf, 0); // netid
			MSG_WriteLong(&cl->netbuf, 0); // x
			MSG_WriteLong(&cl->netbuf, 0); // y
			MSG_WriteByte(&cl->netbuf, channel);
			MSG_WriteByte(&cl->netbuf, sfx_id);
			MSG_WriteByte(&cl->netbuf, attenuation);
			MSG_WriteByte(&cl->netbuf, 255); // client calculates volume on its own
		}
	}
}

void SV_Sound (fixed_t x, fixed_t y, byte channel, const char *name, byte attenuation)
{
	int        sfx_id;
	client_t  *cl;

	sfx_id = S_FindSound (name);

	if (sfx_id > 255 || sfx_id < 0)
	{
		Printf (PRINT_HIGH, "SV_StartSound: range error. Sfx_id = %d\n", sfx_id);
		return;
	}

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (!(it->ingame()))
			continue;

		cl = &(it->client);

		MSG_WriteMarker(&cl->netbuf, svc_soundorigin);
		MSG_WriteLong(&cl->netbuf, x);
		MSG_WriteLong(&cl->netbuf, y);
		MSG_WriteByte(&cl->netbuf, channel);
		MSG_WriteByte(&cl->netbuf, sfx_id);
		MSG_WriteByte(&cl->netbuf, attenuation);
		MSG_WriteByte(&cl->netbuf, 255); // client calculates volume on its own
	}
}

//
// SV_UpdateFrags
//
void SV_UpdateFrags(player_t &player)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);
		SVC_PlayerMembers(cl->reliablebuf, player, SVC_PM_SCORE);
	}
}

//
// SV_SendUserInfo
//
void SV_SendUserInfo (player_t &player, client_t* cl)
{
	player_t *p = &player;

	MSG_WriteMarker	(&cl->reliablebuf, svc_userinfo);
	MSG_WriteByte	(&cl->reliablebuf, p->id);
	MSG_WriteString (&cl->reliablebuf, p->userinfo.netname.c_str());
	MSG_WriteByte	(&cl->reliablebuf, p->userinfo.team);
	MSG_WriteLong	(&cl->reliablebuf, p->userinfo.gender);

	for (int i = 3; i >= 0; i--)
		MSG_WriteByte(&cl->reliablebuf, p->userinfo.color[i]);

	// [SL] place holder for deprecated skins
	MSG_WriteString	(&cl->reliablebuf, "");

	MSG_WriteShort	(&cl->reliablebuf, time(NULL) - p->JoinTime);
}

/**
Spreads a player's userinfo to every client.
@param player Player to parse info for.
 */
void SV_BroadcastUserInfo(player_t &player)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
		SV_SendUserInfo(player, &(it->client));
}

/**
 * Stores a players userinfo.
 *
 * @param player Player to parse info for.
 * @return False if the client was kicked because of something seriously
 *         screwy going on with their info.
 */
bool SV_SetupUserInfo(player_t &player)
{
	// read in userinfo from packet
	std::string old_netname(player.userinfo.netname);
	std::string new_netname(MSG_ReadString());
	StripColorCodes(new_netname);

	if (new_netname.length() > MAXPLAYERNAME)
		new_netname.erase(MAXPLAYERNAME);

	if (!ValidString(new_netname))
	{
		SV_InvalidateClient(player, "Name contains invalid characters");
		return false;
	}

	team_t old_team = static_cast<team_t>(player.userinfo.team);
	team_t new_team = static_cast<team_t>(MSG_ReadByte());

	if (new_team >= NUMTEAMS || new_team < 0)
	{
		SV_InvalidateClient(player, "Team preference is invalid");
		return false;
	}
	if (new_team == TEAM_NONE || (new_team == TEAM_GREEN && sv_teamsinplay < NUMTEAMS))
		new_team = TEAM_BLUE; // Set the default team to the player.

	gender_t gender = static_cast<gender_t>(MSG_ReadLong());

	byte color[4];
	for (int i = 3; i >= 0; i--)
		color[i] = MSG_ReadByte();

	MSG_ReadString();	// [SL] place holder for deprecated skins

	fixed_t aimdist = MSG_ReadLong();
	MSG_ReadBool();		// [SL] Read and ignore deprecated cl_unlag setting
	bool predict_weapons = MSG_ReadBool();

	weaponswitch_t switchweapon = static_cast<weaponswitch_t>(MSG_ReadByte());

	byte weapon_prefs[NUMWEAPONS];
	for (size_t i = 0; i < NUMWEAPONS; i++)
	{
		// sanitize the weapon preference input
		byte preflevel = MSG_ReadByte();
		if (preflevel >= NUMWEAPONS)
			preflevel = NUMWEAPONS - 1;

		weapon_prefs[i] = preflevel;
	}

	// ensure sane values for userinfo
	if (gender < 0 || gender >= NUMGENDER)
		gender = GENDER_NEUTER;

	aimdist = clamp(aimdist, 0, 5000 * 16384);

	if (switchweapon >= WPSW_NUMTYPES || switchweapon < 0)
		switchweapon = WPSW_ALWAYS;

	// [SL] 2011-12-02 - Players can update these parameters whenever they like
	player.userinfo.predict_weapons	= predict_weapons;
	player.userinfo.aimdist			= aimdist;
	player.userinfo.switchweapon	= switchweapon;
	memcpy(player.userinfo.weapon_prefs, weapon_prefs, sizeof(weapon_prefs));

	player.userinfo.gender			= gender;
	player.userinfo.team			= new_team;

	memcpy(player.userinfo.color, color, 4);
	memcpy(player.prefcolor, color, 4);

	// sanitize the client's name
	new_netname = TrimString(new_netname);
	if (new_netname.empty())
	{
		if (old_netname.empty())
			new_netname = "PLAYER";
		else
			new_netname = old_netname;
	}

	// Make sure that we're not duplicating any player name
	player_t& other = nameplayer(new_netname);
	if (validplayer(other))
	{
		// Give the player an enumerated name (Player2, Player3, etc.)
		if (player.id != other.id)
		{
			std::string test_netname = std::string(new_netname);

			for (int num = 2;num < MAXPLAYERS + 2;num++)
			{
				std::ostringstream buffer;
				buffer << num;
				std::string strnum = buffer.str();

				// If the enumerated name would be greater than the maximum
				// allowed netname, have the enumeration eat into the last few
				// letters of their netname.
				if (new_netname.length() + strnum.length() >= MAXPLAYERNAME)
					test_netname = new_netname.substr(0, MAXPLAYERNAME - strnum.length()) + strnum;
				else
					test_netname = new_netname + strnum;

				// Check to see if the enumerated name is taken.
				player_t& test = nameplayer(test_netname.c_str());
				if (!validplayer(test))
					break;

				// Check to see if player already has a given enumerated name.
				// Prevents `cl_name Player` from changing their own name from
				// Player2 to Player3.
				if (player.id == test.id)
					break;
			}

			new_netname = test_netname;
		}
		else
		{
			// Player is trying to be cute and change their name to an
			// automatically enumerated name that they already have.  Prevents
			// `cl_name Player2` from changing their own name from Player2 to
			// Player22.
			new_netname = old_netname;
		}
	}

	if (new_netname.length() > MAXPLAYERNAME)
		new_netname.erase(MAXPLAYERNAME);

	player.userinfo.netname = new_netname;

	// Compare names and broadcast if different.
	if (!old_netname.empty() && !iequals(new_netname, old_netname))
	{
		std::string	gendermessage;
		switch (gender) {
			case GENDER_MALE:	gendermessage = "his";  break;
			case GENDER_FEMALE:	gendermessage = "her";  break;
			default:			gendermessage = "its";  break;
		}

		SV_BroadcastPrintf("%s changed %s name to %s.\n",
			old_netname.c_str(), gendermessage.c_str(), player.userinfo.netname.c_str());
	}

	if (G_IsTeamGame())
	{
		SV_CheckTeam(player);

		if (player.mo && player.userinfo.team != old_team && player.ingame() &&
		    !player.spectator && !G_IsLevelState(LevelState::WARMUP))
		{
			// kill player if team is changed
			P_DamageMobj(player.mo, 0, 0, 1000, 0);
			SV_BroadcastPrintf("%s switched to the %s team.\n",
			                   player.userinfo.netname.c_str(),
			                   V_GetTeamColor(player.userinfo.team).c_str());
		}
	}

	return true;
}

//
//	SV_ForceSetTeam
//
//	Forces a client to join a specified team
//
void SV_ForceSetTeam (player_t &who, team_t team)
{
	client_t *cl = &who.client;

	MSG_WriteMarker (&cl->reliablebuf, svc_forceteam);

	who.userinfo.team = team;
	Printf (PRINT_HIGH, "Forcing %s to %s team\n", who.userinfo.netname.c_str(), team == TEAM_NONE ? "NONE" : V_GetTeamColor(team).c_str());
	MSG_WriteShort (&cl->reliablebuf, team);
}

//
//	SV_CheckTeam
//
//	Checks to see if a players team is allowed and corrects if not
//
void SV_CheckTeam (player_t &player)
{
	if (!player.spectator && (player.userinfo.team < 0 || player.userinfo.team >= sv_teamsinplay))
		SV_ForceSetTeam(player, SV_GoodTeam());
}

//
//	SV_GoodTeam
//
//	Finds a working team
//
team_t SV_GoodTeam (void)
{
	int teamcount = sv_teamsinplay;

	// Unsure how this can be triggered?
	if (teamcount == 0)
	{
		I_Error ("Teamplay is set and no teams are enabled!\n");
		return TEAM_NONE;
	}

	// Find the smallest team
	size_t smallest_team_size = MAXPLAYERS;
	team_t smallest_team = (team_t)0;
	for (int i = 0;i < teamcount;i++)
	{
		size_t team_size = P_NumPlayersOnTeam((team_t)i);
		if (team_size < smallest_team_size)
		{
			smallest_team_size = team_size;
			smallest_team = (team_t)i;
		}
	}

	if (sv_maxplayersperteam && smallest_team_size >= sv_maxplayersperteam)
		return TEAM_NONE;

	return smallest_team;
}

//
// SV_SendMobjToClient
//
void SV_SendMobjToClient(AActor *mo, client_t *cl)
{
	if (!mo)
		return;

	MSG_WriteMarker(&cl->reliablebuf, svc_spawnmobj);
	MSG_WriteLong(&cl->reliablebuf, mo->x);
	MSG_WriteLong(&cl->reliablebuf, mo->y);
	MSG_WriteLong(&cl->reliablebuf, mo->z);
	MSG_WriteLong(&cl->reliablebuf, mo->angle);

	MSG_WriteShort(&cl->reliablebuf, mo->type);
	MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
	MSG_WriteByte(&cl->reliablebuf, mo->rndindex);
	MSG_WriteShort(&cl->reliablebuf, (mo->state - states)); // denis - sending state fixes monster ghosts appearing under doors

	if (mo->type == MT_FOUNTAIN)
		MSG_WriteByte(&cl->reliablebuf, mo->args[0]);

	if (mo->type == MT_ZDOOMBRIDGE)
	{
		MSG_WriteByte(&cl->reliablebuf, mo->args[0]);
		MSG_WriteByte(&cl->reliablebuf, mo->args[1]);
	}

	if(mo->flags & MF_MISSILE || mobjinfo[mo->type].flags & MF_MISSILE) // denis - check type as that is what the client will be spawning
	{
		MSG_WriteUnVarint (&cl->reliablebuf, mo->target ? mo->target->netid : 0);
		MSG_WriteUnVarint (&cl->reliablebuf, mo->netid);
		MSG_WriteLong (&cl->reliablebuf, mo->angle);
		MSG_WriteLong (&cl->reliablebuf, mo->momx);
		MSG_WriteLong (&cl->reliablebuf, mo->momy);
		MSG_WriteLong (&cl->reliablebuf, mo->momz);
	}
	else
	{
		if(mo->flags & MF_AMBUSH || mo->flags & MF_DROPPED)
		{
			MSG_WriteMarker(&cl->reliablebuf, svc_mobjinfo);
			MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
			MSG_WriteLong(&cl->reliablebuf, mo->flags);
		}
	}

	// animating corpses
	if((mo->flags & MF_CORPSE) && mo->state - states != S_GIBS)
	{
		MSG_WriteMarker (&cl->reliablebuf, svc_corpse);
		MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
		MSG_WriteByte (&cl->reliablebuf, mo->frame);
		MSG_WriteByte (&cl->reliablebuf, mo->tics);
	}
}

//
// SV_IsTeammate
//
bool SV_IsTeammate(player_t &a, player_t &b)
{
	// same player isn't own teammate
	if(&a == &b)
		return false;

	if (G_IsTeamGame())
	{
		if (a.userinfo.team == b.userinfo.team)
			return true;
		else
			return false;
	}
	else if (sv_gametype == GM_COOP)
		return true;

	else return false;
}

//
// [denis] SV_AwarenessUpdate
//
bool SV_AwarenessUpdate(player_t &player, AActor *mo)
{
	bool ok = false;

	if (!mo)
		return false;

	if(player.mo == mo)
		ok = true;
	else if(!mo->player)
		ok = true;
	else if (mo->flags & MF_SPECTATOR)      // GhostlyDeath -- Spectating things
		ok = false;
	else if(player.mo && mo->player && mo->player->spectator)
		ok = false;
	else if(player.mo && mo->player && SV_IsTeammate(player, *mo->player))
		ok = true;
	else if(player.mo && mo->player && true)
		ok = true;

	bool previously_ok = mo->players_aware.get(player.id);

	client_t *cl = &player.client;

	if(!ok && previously_ok)
	{
		mo->players_aware.unset(player.id);

		MSG_WriteMarker (&cl->reliablebuf, svc_removemobj);
		MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);

		return true;
	}
	else if(!previously_ok && ok)
	{
		mo->players_aware.set(player.id);

		if(!mo->player || mo->player->playerstate != PST_LIVE)
		{
			SV_SendMobjToClient(mo, cl);
		}
		else
		{
			MSG_WriteMarker (&cl->reliablebuf, svc_spawnplayer);
			MSG_WriteByte (&cl->reliablebuf, mo->player->id);
			MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
			MSG_WriteLong (&cl->reliablebuf, mo->angle);
			MSG_WriteLong (&cl->reliablebuf, mo->x);
			MSG_WriteLong (&cl->reliablebuf, mo->y);
			MSG_WriteLong (&cl->reliablebuf, mo->z);
		}

		return true;
	}

	return false;
}

//
// [denis] SV_SpawnMobj
// because you can't expect the constructors to send network messages!
//
void SV_SpawnMobj(AActor *mo)
{
	if (!mo)
		return;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		if (mo->player)
			SV_AwarenessUpdate(*it, mo);
		else
			it->to_spawn.push(mo->ptr());
	}
}

//
// [denis] SV_IsPlayerAllowedToSee
// determine if a client should be able to see an actor
//
bool SV_IsPlayerAllowedToSee(player_t &p, AActor *mo)
{
	if (!mo)
		return false;

	if (mo->flags & MF_SPECTATOR)
		return false; // GhostlyDeath -- always false, as usual!
	else
		return mo->players_aware.get(p.id);
}

#define HARDWARE_CAPABILITY 1000

//
// SV_UpdateHiddenMobj
//
void SV_UpdateHiddenMobj(void)
{
	// denis - todo - throttle this
	AActor *mo;
	TThinkerIterator<AActor> iterator;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		player_t &pl = *it;

		if (!pl.mo)
			continue;

		int updated = 0;

		while (!pl.to_spawn.empty())
		{
			mo = pl.to_spawn.front();

			pl.to_spawn.pop();

			if (mo && !mo->WasDestroyed())
				updated += SV_AwarenessUpdate(pl, mo);

			if (updated > 16)
				break;
		}

		while ((mo = iterator.Next()))
		{
			updated += SV_AwarenessUpdate(pl, mo);

			if (updated > 16)
				break;
		}
	}
}

void SV_UpdateSector(client_t* cl, int sectornum)
{
	sector_t* sector = &sectors[sectornum];

	// Only update moveable sectors to clients
	if (sector != NULL && sector->moveable)
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_sector);
		MSG_WriteShort(&cl->reliablebuf, sectornum);
		MSG_WriteShort(&cl->reliablebuf, P_FloorHeight(sector) >> FRACBITS);
		MSG_WriteShort(&cl->reliablebuf, P_CeilingHeight(sector) >> FRACBITS);
		MSG_WriteShort(&cl->reliablebuf, sector->floorpic);
		MSG_WriteShort(&cl->reliablebuf, sector->ceilingpic);
		MSG_WriteShort(&cl->reliablebuf, sector->special);
	}
}

void SV_BroadcastSector(int sectornum)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
		SV_UpdateSector(&(it->client), sectornum);
}

//
// SV_UpdateSectors
// Update doors, floors, ceilings etc... that have at some point moved
//
void SV_UpdateSectors(client_t* cl)
{
	for (int sectornum = 0; sectornum < numsectors; sectornum++)
	{
		SV_UpdateSector(cl, sectornum);

		sector_s* sector = &sectors[sectornum];
		if (!sector->SectorChanges)
			continue;

		MSG_WriteMarker(&cl->reliablebuf, svc_sectorproperties);
		MSG_WriteShort(&cl->reliablebuf, sectornum);
		MSG_WriteShort(&cl->reliablebuf, sector->SectorChanges);

		for (int i = 0, prop = 1; prop < SPC_Max; i++)
		{
			prop = 1 << i;
			if ((prop & sector->SectorChanges) == 0)
				continue;

			switch (prop)
			{
			case SPC_FlatPic:
				MSG_WriteShort(&cl->reliablebuf, sector->floorpic);
				MSG_WriteShort(&cl->reliablebuf, sector->ceilingpic);
				break;
			case SPC_LightLevel:
				MSG_WriteShort(&cl->reliablebuf, sector->lightlevel);
				break;
			case SPC_Color:
				MSG_WriteByte(&cl->reliablebuf, sector->colormap->color.getr());
				MSG_WriteByte(&cl->reliablebuf, sector->colormap->color.getg());
				MSG_WriteByte(&cl->reliablebuf, sector->colormap->color.getb());
				break;
			case SPC_Fade:
				MSG_WriteByte(&cl->reliablebuf, sector->colormap->fade.getr());
				MSG_WriteByte(&cl->reliablebuf, sector->colormap->fade.getg());
				MSG_WriteByte(&cl->reliablebuf, sector->colormap->fade.getb());
				break;
			case SPC_Gravity:
				MSG_WriteLong(&cl->reliablebuf, sector->gravity);
				break;
			case SPC_Panning:
				MSG_WriteLong(&cl->reliablebuf, sector->ceiling_xoffs);
				MSG_WriteLong(&cl->reliablebuf, sector->ceiling_yoffs);
				MSG_WriteLong(&cl->reliablebuf, sector->floor_xoffs);
				MSG_WriteLong(&cl->reliablebuf, sector->floor_yoffs);
				break;
			case SPC_Scale:
				MSG_WriteLong(&cl->reliablebuf, sector->ceiling_xscale);
				MSG_WriteLong(&cl->reliablebuf, sector->ceiling_yscale);
				MSG_WriteLong(&cl->reliablebuf, sector->floor_xscale);
				MSG_WriteLong(&cl->reliablebuf, sector->floor_yscale);
				break;
			case SPC_Rotation:
				MSG_WriteLong(&cl->reliablebuf, sector->floor_angle);
				MSG_WriteLong(&cl->reliablebuf, sector->ceiling_angle);
				break;
			case SPC_AlignBase:
				MSG_WriteLong(&cl->reliablebuf, sector->base_ceiling_angle);
				MSG_WriteLong(&cl->reliablebuf, sector->base_ceiling_yoffs);
				MSG_WriteLong(&cl->reliablebuf, sector->base_floor_angle);
				MSG_WriteLong(&cl->reliablebuf, sector->base_floor_yoffs);
				break;
			default:
				break;
			}
		}
	}
}

//
// SV_DestroyFinishedMovingSectors
//
// Calls Destroy() on moving sectors that are done moving.
//
void SV_DestroyFinishedMovingSectors()
{
	std::list<movingsector_t>::iterator itr;
	itr = movingsectors.begin();

	while (itr != movingsectors.end())
	{
		sector_t *sector = itr->sector;

		if (P_MovingCeilingCompleted(sector))
		{
			itr->moving_ceiling = false;
			if (sector->ceilingdata)
				sector->ceilingdata->Destroy();
			sector->ceilingdata = NULL;
		}
		if (P_MovingFloorCompleted(sector))
		{
			itr->moving_floor = false;
			if (sector->floordata)
				sector->floordata->Destroy();
			sector->floordata = NULL;
		}

		if (!itr->moving_ceiling && !itr->moving_floor)
			movingsectors.erase(itr++);
		else
			++itr;
	}
}

//
// SV_SendMovingSectorUpdate
//
//
void SV_SendMovingSectorUpdate(player_t &player, sector_t *sector)
{
	if (!sector || !validplayer(player))
		return;

	int sectornum = sector - sectors;
	if (sectornum < 0 || sectornum >= numsectors)
		return;

	buf_t *netbuf = &(player.client.netbuf);

	// Determine which moving planes are in this sector
	movertype_t floor_mover = SEC_INVALID, ceiling_mover = SEC_INVALID;

	if (sector->ceilingdata && sector->ceilingdata->IsA(RUNTIME_CLASS(DCeiling)))
		ceiling_mover = SEC_CEILING;
	if (sector->ceilingdata && sector->ceilingdata->IsA(RUNTIME_CLASS(DDoor)))
		ceiling_mover = SEC_DOOR;
	if (sector->floordata && sector->floordata->IsA(RUNTIME_CLASS(DFloor)))
		floor_mover = SEC_FLOOR;
	if (sector->floordata && sector->floordata->IsA(RUNTIME_CLASS(DPlat)))
		floor_mover = SEC_PLAT;
	if (sector->ceilingdata && sector->ceilingdata->IsA(RUNTIME_CLASS(DElevator)))
	{
		ceiling_mover = SEC_ELEVATOR;
		floor_mover = SEC_INVALID;
	}
	if (sector->ceilingdata && sector->ceilingdata->IsA(RUNTIME_CLASS(DPillar)))
	{
		ceiling_mover = SEC_PILLAR;
		floor_mover = SEC_INVALID;
	}

	// no moving planes?  skip it.
	if (ceiling_mover == SEC_INVALID && floor_mover == SEC_INVALID)
		return;

	// Create bitfield to denote moving planes in this sector
	byte movers = byte(ceiling_mover) | (byte(floor_mover) << 4);

	MSG_WriteMarker(netbuf, svc_movingsector);
	MSG_WriteShort(netbuf, sectornum);
	MSG_WriteShort(netbuf, P_CeilingHeight(sector) >> FRACBITS);
	MSG_WriteShort(netbuf, P_FloorHeight(sector) >> FRACBITS);
	MSG_WriteByte(netbuf, movers);

	if (ceiling_mover == SEC_ELEVATOR)
	{
		DElevator *Elevator = static_cast<DElevator *>(sector->ceilingdata);

        MSG_WriteByte(netbuf, Elevator->m_Type);
        MSG_WriteByte(netbuf, Elevator->m_Status);
        MSG_WriteByte(netbuf, Elevator->m_Direction);
        MSG_WriteShort(netbuf, Elevator->m_FloorDestHeight >> FRACBITS);
        MSG_WriteShort(netbuf, Elevator->m_CeilingDestHeight >> FRACBITS);
        MSG_WriteLong(netbuf, Elevator->m_Speed);
	}

	if (ceiling_mover == SEC_PILLAR)
	{
		DPillar *Pillar = static_cast<DPillar *>(sector->ceilingdata);

        MSG_WriteByte(netbuf, Pillar->m_Type);
        MSG_WriteByte(netbuf, Pillar->m_Status);
        MSG_WriteShort(netbuf, Pillar->m_FloorSpeed >> FRACBITS);
        MSG_WriteShort(netbuf, Pillar->m_CeilingSpeed >> FRACBITS);
        MSG_WriteShort(netbuf, Pillar->m_FloorTarget >> FRACBITS);
        MSG_WriteShort(netbuf, Pillar->m_CeilingTarget >> FRACBITS);
        MSG_WriteBool(netbuf, Pillar->m_Crush);
	}

	if (ceiling_mover == SEC_CEILING)
	{
		DCeiling *Ceiling = static_cast<DCeiling *>(sector->ceilingdata);

        MSG_WriteByte(netbuf, Ceiling->m_Type);
        MSG_WriteShort(netbuf, Ceiling->m_BottomHeight >> FRACBITS);
        MSG_WriteShort(netbuf, Ceiling->m_TopHeight >> FRACBITS);
        MSG_WriteLong(netbuf, Ceiling->m_Speed);
        MSG_WriteLong(netbuf, Ceiling->m_Speed1);
        MSG_WriteLong(netbuf, Ceiling->m_Speed2);
        MSG_WriteBool(netbuf, Ceiling->m_Crush);
        MSG_WriteBool(netbuf, Ceiling->m_Silent);
        MSG_WriteByte(netbuf, Ceiling->m_Direction);
        MSG_WriteShort(netbuf, Ceiling->m_Texture);
        MSG_WriteShort(netbuf, Ceiling->m_NewSpecial);
        MSG_WriteShort(netbuf, Ceiling->m_Tag);
        MSG_WriteByte(netbuf, Ceiling->m_OldDirection);
	}

	if (ceiling_mover == SEC_DOOR)
	{
		DDoor *Door = static_cast<DDoor *>(sector->ceilingdata);

        MSG_WriteByte(netbuf, Door->m_Type);
        MSG_WriteShort(netbuf, Door->m_TopHeight >> FRACBITS);
        MSG_WriteLong(netbuf, Door->m_Speed);
        MSG_WriteLong(netbuf, Door->m_TopWait);
        MSG_WriteLong(netbuf, Door->m_TopCountdown);
		MSG_WriteByte(netbuf, Door->m_Status);
		// Check for an invalid m_Line (doors triggered by tag 666)
        MSG_WriteLong(netbuf, Door->m_Line ? (Door->m_Line - lines) : -1);
	}

	if (floor_mover == SEC_FLOOR)
	{
		DFloor *Floor = static_cast<DFloor *>(sector->floordata);

        MSG_WriteByte(netbuf, Floor->m_Type);
        MSG_WriteByte(netbuf, Floor->m_Status);
        MSG_WriteBool(netbuf, Floor->m_Crush);
        MSG_WriteByte(netbuf, Floor->m_Direction);
        MSG_WriteShort(netbuf, Floor->m_NewSpecial);
        MSG_WriteShort(netbuf, Floor->m_Texture);
        MSG_WriteShort(netbuf, Floor->m_FloorDestHeight >> FRACBITS);
        MSG_WriteLong(netbuf, Floor->m_Speed);
        MSG_WriteLong(netbuf, Floor->m_ResetCount);
        MSG_WriteShort(netbuf, Floor->m_OrgHeight >> FRACBITS);
        MSG_WriteLong(netbuf, Floor->m_Delay);
        MSG_WriteLong(netbuf, Floor->m_PauseTime);
        MSG_WriteLong(netbuf, Floor->m_StepTime);
        MSG_WriteLong(netbuf, Floor->m_PerStepTime);
        MSG_WriteShort(netbuf, Floor->m_Height >> FRACBITS);
        MSG_WriteByte(netbuf, Floor->m_Change);
		MSG_WriteLong(netbuf, Floor->m_Line ? (Floor->m_Line - lines) : -1);
	}

	if (floor_mover == SEC_PLAT)
	{
		DPlat *Plat = static_cast<DPlat *>(sector->floordata);

        MSG_WriteLong(netbuf, Plat->m_Speed);
        MSG_WriteShort(netbuf, Plat->m_Low >> FRACBITS);
        MSG_WriteShort(netbuf, Plat->m_High >> FRACBITS);
        MSG_WriteLong(netbuf, Plat->m_Wait);
        MSG_WriteLong(netbuf, Plat->m_Count);
        MSG_WriteByte(netbuf, Plat->m_Status);
        MSG_WriteByte(netbuf, Plat->m_OldStatus);
        MSG_WriteBool(netbuf, Plat->m_Crush);
        MSG_WriteShort(netbuf, Plat->m_Tag);
        MSG_WriteByte(netbuf, Plat->m_Type);
        MSG_WriteShort(netbuf, Plat->m_Height >> FRACBITS);
        MSG_WriteShort(netbuf, Plat->m_Lip >> FRACBITS);
	}
}

//
// SV_UpdateMovingSectors
// Update doors, floors, ceilings etc... that are actively moving
//
void SV_UpdateMovingSectors(player_t &player)
{
	std::list<movingsector_t>::iterator itr;
	for (itr = movingsectors.begin(); itr != movingsectors.end(); ++itr)
	{
		sector_t *sector = itr->sector;

		SV_SendMovingSectorUpdate(player, sector);
	}
}


//
// SV_SendGametic
// Sends gametic to synchronize with the client
//
// [SL] 2011-05-11 - Instead of sending the whole gametic (4 bytes),
// send only the least significant byte to save bandwidth.
void SV_SendGametic(client_t* cl)
{
	MSG_WriteMarker	(&cl->netbuf, svc_svgametic);
	MSG_WriteByte	(&cl->netbuf, (byte)(gametic & 0xFF));
}

short P_GetButtonTexture(line_t* line);

void SV_LineStateUpdate(client_t *cl)
{
	for (int lineNum = 0; lineNum < numlines; lineNum++)
	{
		line_t* line = &lines[lineNum];

		if (line->PropertiesChanged)
		{
			MSG_WriteMarker(&cl->reliablebuf, svc_lineupdate);
			MSG_WriteShort(&cl->reliablebuf, lineNum);
			MSG_WriteShort(&cl->reliablebuf, line->flags);
			MSG_WriteByte(&cl->reliablebuf, line->lucency);
		}

		if (!line->SidedefChanged)
			continue;
		
		for (int sideNum = 0; sideNum < 2; sideNum++)
		{
			if (line->sidenum[sideNum] != R_NOSIDE)
			{
				side_t* currentSideDef = sides + line->sidenum[sideNum];
				if (!currentSideDef->SidedefChanges)
					continue;

				MSG_WriteMarker(&cl->reliablebuf, svc_linesideupdate);
				MSG_WriteShort(&cl->reliablebuf, lineNum);
				MSG_WriteByte(&cl->reliablebuf, sideNum);
				MSG_WriteByte(&cl->reliablebuf, currentSideDef->SidedefChanges);

				for (int i = 0, prop = 1; prop < SDPC_Max; i++)
				{
					prop = 1 << i;
					if ((prop & currentSideDef->SidedefChanges) == 0)
						continue;

					switch (prop)
					{
					case SDPC_TexTop:
						MSG_WriteShort(&cl->reliablebuf, currentSideDef->toptexture);
						break;
					case SDPC_TexMid:
						MSG_WriteShort(&cl->reliablebuf, currentSideDef->midtexture);
						break;
					case SDPC_TexBottom:
						MSG_WriteShort(&cl->reliablebuf, currentSideDef->bottomtexture);
						break;
					default:
						break;
					}
				}
			}
		}
	}
}

void SV_ThinkerUpdate(client_t* cl)
{
	TThinkerIterator<DScroller> scrollIter;
	TThinkerIterator<DFireFlicker> fireIter;
	TThinkerIterator<DFlicker> flickerIter;
	TThinkerIterator<DLightFlash> lightFlashIter;
	TThinkerIterator<DStrobe> strobeIter;
	TThinkerIterator<DGlow>	glowIter;
	TThinkerIterator<DGlow2> glow2Iter;
	TThinkerIterator<DPhased> phasedIter;

	DScroller *scroller;
	DFireFlicker* fireFlicker;
	DFlicker* flicker;
	DLightFlash* lightFlash;
	DStrobe* strobe;
	DGlow* glow;
	DGlow2* glow2;
	DPhased* phased;

	while ((scroller = scrollIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_Scroller);
		MSG_WriteByte(&cl->reliablebuf, scroller->GetType());
		MSG_WriteLong(&cl->reliablebuf, scroller->GetScrollX());
		MSG_WriteLong(&cl->reliablebuf, scroller->GetScrollY());
		MSG_WriteLong(&cl->reliablebuf, scroller->GetAffectee());
	}

	while ((fireFlicker = fireIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_FireFlicker);
		MSG_WriteShort(&cl->reliablebuf, fireFlicker->GetSector() - sectors);
		MSG_WriteShort(&cl->reliablebuf, fireFlicker->GetMinLight());
		MSG_WriteShort(&cl->reliablebuf, fireFlicker->GetMaxLight());
	}

	while ((flicker = flickerIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_Flicker);
		MSG_WriteShort(&cl->reliablebuf, flicker->GetSector() - sectors);
		MSG_WriteShort(&cl->reliablebuf, flicker->GetMinLight());
		MSG_WriteShort(&cl->reliablebuf, flicker->GetMaxLight());
	}

	while ((lightFlash = lightFlashIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_LightFlash);
		MSG_WriteShort(&cl->reliablebuf, lightFlash->GetSector() - sectors);
		MSG_WriteShort(&cl->reliablebuf, lightFlash->GetMinLight());
		MSG_WriteShort(&cl->reliablebuf, lightFlash->GetMaxLight());
	}

	while ((strobe = strobeIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_Strobe);
		MSG_WriteShort(&cl->reliablebuf, strobe->GetSector() - sectors);
		MSG_WriteShort(&cl->reliablebuf, strobe->GetMinLight());
		MSG_WriteShort(&cl->reliablebuf, strobe->GetMaxLight());
		MSG_WriteShort(&cl->reliablebuf, strobe->GetDarkTime());
		MSG_WriteShort(&cl->reliablebuf, strobe->GetBrightTime());
		MSG_WriteByte(&cl->reliablebuf, strobe->GetCount());
	}

	while ((glow = glowIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_Glow);
		MSG_WriteShort(&cl->reliablebuf, glow->GetSector() - sectors);
	}

	while ((glow2 = glow2Iter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_Glow2);
		MSG_WriteShort(&cl->reliablebuf, glow2->GetSector() - sectors);
		MSG_WriteShort(&cl->reliablebuf, glow2->GetStart());
		MSG_WriteShort(&cl->reliablebuf, glow2->GetEnd());
		MSG_WriteShort(&cl->reliablebuf, glow2->GetMaxTics());
		MSG_WriteByte(&cl->reliablebuf, glow2->GetOneShot());
	}

	while ((phased = phasedIter.Next()))
	{
		MSG_WriteMarker(&cl->reliablebuf, svc_thinkerupdate);
		MSG_WriteByte(&cl->reliablebuf, TT_Phased);
		MSG_WriteShort(&cl->reliablebuf, phased->GetSector() - sectors);
		MSG_WriteShort(&cl->reliablebuf, phased->GetBaseLevel());
		MSG_WriteByte(&cl->reliablebuf, phased->GetPhase());
	}
}

//
// SV_ClientFullUpdate
//
void SV_ClientFullUpdate(player_t &pl)
{
	client_t *cl = &pl.client;

	MSG_WriteMarker(&cl->reliablebuf, svc_fullupdatestart);

	// Send the player all level locals.
	SVC_LevelLocals(cl->reliablebuf, ::level, SVC_MSG_ALL);

	// send player's info to the client
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (it->mo)
			SV_AwarenessUpdate(pl, it->mo);

		SV_SendUserInfo(*it, cl);
		if (cl->reliablebuf.cursize >= MaxPacketSize && !SV_SendPacket(pl))
			return;
	}

	// update levelstate
	SVC_LevelState(cl->reliablebuf, ::levelstate.serialize());

	// update all player members
	for (Players::iterator it = players.begin(); it != players.end(); ++it)
		SVC_PlayerMembers(cl->reliablebuf, *it, SVC_MSG_ALL);

	// [deathz0r] send team frags/captures if teamplay is enabled
	if (G_IsTeamGame())
	{
		for (int i = 0; i < NUMTEAMS; i++)
			SVC_TeamMembers(cl->reliablebuf, static_cast<team_t>(i));
	}

	SV_UpdateHiddenMobj();

	// update flags
	if (sv_gametype == GM_CTF)
		CTF_Connect(pl);

	SV_UpdateSectors(cl);
	if (cl->reliablebuf.cursize >= MaxPacketSize && !SV_SendPacket(pl))
		return;

	P_UpdateButtons(cl);
	if (cl->reliablebuf.cursize >= MaxPacketSize && !SV_SendPacket(pl))
		return;

	SV_LineStateUpdate(cl);
	if (cl->reliablebuf.cursize >= MaxPacketSize && !SV_SendPacket(pl))
		return;

	SV_ThinkerUpdate(cl);
	if (cl->reliablebuf.cursize >= MaxPacketSize && !SV_SendPacket(pl))
		return;

	MSG_WriteMarker(&cl->reliablebuf, svc_fullupdatedone);

	SV_SendPacket(pl);
}

//===========================
// SV_UpdateSecret
// Updates a sector to a client and the number of secrets found.
//===========================
void SV_UpdateSecret(int sectornum, player_t &player)
{
	// Don't announce secrets on PvP gamemodes
	if (sv_gametype != GM_COOP)
		return;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		client_t* cl = &(it->client);

		SVC_LevelLocals(cl->reliablebuf, ::level, SVC_LL_SECRETS);
		SVC_PlayerMembers(cl->reliablebuf, player, SVC_PM_SCORE);

		if (&*it == &player)
			continue;

		SVC_SecretFound(cl->reliablebuf, player.id, sectornum);
	}

}

//
//	SV_SendServerSettings
//
//	Sends server setting info
//

void SV_SendPackets(void);

void SV_SendServerSettings (player_t &pl)
{
	// GhostlyDeath <June 19, 2008> -- Loop through all CVARs and send the CVAR_SERVERINFO stuff only
	cvar_t *var = GetFirstCvar();

    client_t *cl = &pl.client;

	while (var)
	{
		if (var->flags() & CVAR_SERVERINFO)
		{
            if ((cl->reliablebuf.cursize + 1 + 1 + (strlen(var->name()) + 1) + (strlen(var->cstring()) + 1) + 1) >= 512)
                SV_SendPacket(pl);

            MSG_WriteMarker(&cl->reliablebuf, svc_serversettings);

            MSG_WriteByte(&cl->reliablebuf, 1); // TODO: REMOVE IN 0.7

			MSG_WriteString(&cl->reliablebuf, var->name());
			MSG_WriteString(&cl->reliablebuf, var->cstring());

            MSG_WriteByte(&cl->reliablebuf, 2); // TODO: REMOVE IN 0.7
		}

		var = var->GetNext();
	}
}

//
//	SV_ServerSettingChange
//
//	Sends server settings to clients when changed
//
void SV_ServerSettingChange (void)
{
	if (gamestate != GS_LEVEL)
		return;

	for (Players::iterator it = players.begin();it != players.end();++it)
		SV_SendServerSettings(*it);
}

// SV_CheckClientVersion
bool SV_CheckClientVersion(client_t *cl, Players::iterator it)
{
	int GameVer = 0;
	std::string VersionStr;
	std::string OurVersionStr(DOTVERSIONSTR);
	bool AllowConnect = true;
	int cl_major = 0;
	int cl_minor = 0;
	int cl_patch = 0;
	int sv_major, sv_minor, sv_patch;
	BREAKVER(GAMEVER, sv_major, sv_minor, sv_patch);

	switch (cl->version)
	{
	case 65:
		GameVer = MSG_ReadLong();

		cl_major = VERMAJ(GameVer);
		cl_minor = VERMIN(GameVer);
		cl_patch = VERPATCH(GameVer);

		StrFormat(VersionStr, "%d.%d.%d", cl_major, cl_minor, cl_patch);

		cl->packedversion = GameVer;

		// Major and minor versions must be identical, client is allowed
		// to have a newer patch.
		if ((cl_major == sv_major) && (cl_minor == sv_minor) &&
		    (cl_patch >= sv_patch))
			AllowConnect = true;
		else
			AllowConnect = false;
		break;
	case 64:
		VersionStr = "0.2a or 0.3";
		break;
	case 63:
		VersionStr = "Pre-0.2";
		break;
	case 62:
		VersionStr = "0.1a";
		break;
	default:
		VersionStr = "Unknown";
		break;
	}

	// GhostlyDeath -- removes the constant AllowConnects above
	if (cl->version != 65)
		AllowConnect = false;

	// GhostlyDeath -- boot em
	if (!AllowConnect)
	{
		std::ostringstream FormattedString;
		bool older = false;

		// GhostlyDeath -- Version Mismatch message

        FormattedString <<
            std::endl <<
            "Your version of Odamex " <<
            VersionStr <<
            " does not match the server " <<
            OurVersionStr <<
            std::endl;

		// GhostlyDeath -- Check to see if it's older or not
		if (cl_major < sv_major)
		{
			older = true;
		}
		else if (cl_major > sv_major)
		{
			older = false;
		}
		else
		{
			if (cl_minor < sv_minor)
				older = true;
			else if (cl_minor > sv_minor)
				older = false;
		}

		// GhostlyDeath -- Print message depending on older or newer
		if (older)
		{
			FormattedString <<
                "For updates, visit http://odamex.net/" <<
                std::endl;
		}
		else
        {
			FormattedString <<
                "If a new version just came out, " <<
                "give server administrators time to update their servers." <<
                std::endl;
        }

		// GhostlyDeath -- email address set?
		if (*(sv_email.cstring()))
		{
			char emailbuf[100];
			memset(emailbuf, 0, sizeof(emailbuf));
			const char* in = sv_email.cstring();
			char* out = emailbuf;

			for (int i = 0; i < 100 && *in; i++, in++, out++)
				*out = *in;

			FormattedString <<
                "If problems persist, contact the server administrator at " <<
                emailbuf <<
                std::endl;
		}

		// GhostlyDeath -- Now we tell them our built up message and boot em
		cl->displaydisconnect = false;	// Don't spam the players

		MSG_WriteMarker(&cl->reliablebuf, svc_print);
		MSG_WriteByte(&cl->reliablebuf, PRINT_HIGH);
		MSG_WriteString(&cl->reliablebuf,
                        (const char *)FormattedString.str().c_str());

		MSG_WriteMarker(&cl->reliablebuf, svc_disconnect);

		SV_SendPacket(*it);

		// GhostlyDeath -- And we tell the server
		Printf("%s -- Version mismatch (%s != %s)\n",
                NET_AdrToString(net_from),
                VersionStr.c_str(),
                OurVersionStr.c_str());
	}

	return AllowConnect;
}

//
//	SV_ConnectClient
//
//	Called when a client connects
//
void G_DoReborn (player_t &playernum);

void SV_ConnectClient()
{
	int challenge = MSG_ReadLong();

	// New querying system
	if (SV_QryParseEnquiry(challenge) == 0)
		return;

	if (challenge == LAUNCHER_CHALLENGE)  // for Launcher
	{
		SV_SendServerInfo();
		return;
	}

	if (challenge != CHALLENGE)
		return;

	if (!SV_IsValidToken(MSG_ReadLong()))
		return;

	Printf("%s is trying to connect...\n", NET_AdrToString (net_from));

	// find an open slot
	Players::iterator it = SV_GetFreeClient();

	if (it == players.end()) // a server is full
	{
		Printf("%s disconnected (server full).\n", NET_AdrToString (net_from));

		static buf_t smallbuf(16);
		MSG_WriteLong(&smallbuf, 0);
		MSG_WriteMarker(&smallbuf, svc_full);
		NET_SendPacket(smallbuf, net_from);

		return;
	}

	player_t* player = &(*it);
	client_t* cl = &(player->client);

	// clear and reinitialize client network info
	cl->address = net_from;
	cl->last_received = gametic;
	cl->reliable_bps = 0;
	cl->unreliable_bps = 0;
	cl->lastcmdtic = 0;
	cl->lastclientcmdtic = 0;
	cl->allow_rcon = false;
	cl->displaydisconnect = false;

	SZ_Clear(&cl->netbuf);
	SZ_Clear(&cl->reliablebuf);
	SZ_Clear(&cl->relpackets);

	memset(cl->packetseq, -1, sizeof(cl->packetseq));
	memset(cl->packetbegin, 0, sizeof(cl->packetbegin));
	memset(cl->packetsize, 0, sizeof(cl->packetsize));

	cl->sequence = 0;
	cl->last_sequence = -1;
	cl->packetnum = 0;
	
	// generate a random string
	std::stringstream ss;
	ss << time(NULL) << level.time << VERSION << NET_AdrToString(net_from);
	cl->digest = MD5SUM(ss.str());

	// Set player time
	player->JoinTime = time(NULL);

	cl->version = MSG_ReadShort();
	byte connection_type = MSG_ReadByte();

	// [SL] 2011-05-11 - Register the player with the reconciliation system
	// for unlagging
	Unlag::getInstance().registerPlayer(player->id);

	// Check if the client uses the same version as the server.
	if (!SV_CheckClientVersion(cl, it))
	{
		SV_DropClient(*player);
		return;
	}

	// Get the userinfo from the client.
	clc_t userinfo = (clc_t)MSG_ReadByte();
	if (userinfo != clc_userinfo)
	{
		SV_InvalidateClient(*player, "Client didn't send any userinfo");
		return;
	}

	if (!SV_SetupUserInfo(*player))
		return;

	// [SL] Read and ignore deprecated client rate. Clients now always use sv_maxrate.
	MSG_ReadLong();
	cl->rate = int(sv_maxrate);

	// Check if the IP is banned from our list or not.
	if (SV_BanCheck(cl))
	{
		cl->displaydisconnect = false;
		SV_DropClient(*player);
		return;
	}

	// Check if the user entered a good password (if any)
	std::string passhash = MSG_ReadString();
	if (strlen(join_password.cstring()) && MD5SUM(join_password.cstring()) != passhash)
	{
		Printf("%s disconnected (password failed).\n", NET_AdrToString(net_from));

		MSG_WriteMarker(&cl->reliablebuf, svc_print);
		MSG_WriteByte(&cl->reliablebuf, PRINT_HIGH);
		MSG_WriteString(&cl->reliablebuf, "Server is passworded, no password specified or bad password.\n");

		SV_SendPacket(*player);
		SV_DropClient(*player);
		return;
	}

	// send consoleplayer number
	MSG_WriteMarker(&cl->reliablebuf, svc_consoleplayer);
	MSG_WriteByte(&cl->reliablebuf, player->id);
	MSG_WriteString(&cl->reliablebuf, cl->digest.c_str());
	SV_SendPacket(*player);
}

void SV_ConnectClient2(player_t& player)
{
	client_t* cl = &player.client;

	// [AM] FIXME: I don't know if it's safe to set players as PST_ENTER
	//             this early.
	player.playerstate = PST_LIVE;

	// [Toke] send server settings
	SV_SendServerSettings(player);

	cl->displaydisconnect = true;

	cl->download.name = "";
	cl->download.md5 = "";

	SV_BroadcastUserInfo(player);

	// Newly connected players get ENTER state.
	P_ClearPlayerScores(player, SCORES_CLEAR_ALL);
	player.playerstate = PST_ENTER;

	if (!step_mode)
	{
		player.spectator = true;
		for (Players::iterator pit = players.begin(); pit != players.end(); ++pit)
			SVC_PlayerMembers(pit->client.reliablebuf, player, SVC_PM_SPECTATOR);
	}

	// Send a map name
	SVC_LoadMap(player.client.reliablebuf, ::wadfiles, ::patchfiles, level.mapname,
	            level.time);

	// [SL] 2011-12-07 - Force the player to jump to intermission if not in a level
	if (gamestate == GS_INTERMISSION)
		MSG_WriteMarker(&cl->reliablebuf, svc_exitlevel);

	G_DoReborn(player);
	SV_ClientFullUpdate(player);

	SV_BroadcastPrintf("%s has connected.\n", player.userinfo.netname.c_str());

	// tell others clients about it
	for (Players::iterator pit = players.begin(); pit != players.end(); ++pit)
	{
		MSG_WriteMarker(&pit->client.reliablebuf, svc_connectclient);
		MSG_WriteByte(&pit->client.reliablebuf, player.id);
	}

	// Notify this player of other player's queue positions
	SV_SendPlayerQueuePositions(&player, true); 

	// Send out the server's MOTD.
	SV_MidPrint((char*)sv_motd.cstring(), &player, 6);
}

//
// SV_DisconnectClient
//
void SV_DisconnectClient(player_t &who)
{
	char str[100];
	std::string disconnectmessage;

	// already gone though this procedure?
	if (who.playerstate == PST_DISCONNECT)
		return;

	// tell others clients about it
	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
	   client_t &cl = it->client;
	   MSG_WriteMarker(&cl.reliablebuf, svc_disconnectclient);
	   MSG_WriteByte(&cl.reliablebuf, who.id);
	}

	Maplist_Disconnect(who);
	Vote_Disconnect(who);

	if (who.client.displaydisconnect)
	{
		// print some final stats for the disconnected player
		std::string status;
		if (who.playerstate == PST_DOWNLOAD)
			status = "downloading";
		else if (who.spectator)
			status = "SPECTATOR";
		else
		{
			if (G_IsTeamGame())
			{
				sprintf(str, "%s TEAM, ", GetTeamInfo(who.userinfo.team)->ColorStringUpper.c_str());
				status += str;
			}

			// Points (CTF).
			if (sv_gametype == GM_CTF)
			{
				sprintf(str, "%d POINTS, ", who.points);
				status += str;
			}

			// Frags (DM/TDM/CTF) or Kills (Coop).
			if (sv_gametype == GM_COOP)
				sprintf(str, "%d KILLS, ", who.killcount);
			else
				sprintf(str, "%d FRAGS, ", who.fragcount);

			status += str;

			// Deaths.
			sprintf(str, "%d DEATHS", who.deathcount);
			status += str;
		}

		// Name and reason for disconnect.
		if (gametic - who.client.last_received == CLIENT_TIMEOUT*35)
			SV_BroadcastPrintf("%s timed out. (%s)\n",
							who.userinfo.netname.c_str(), status.c_str());
		else
			SV_BroadcastPrintf("%s disconnected. (%s)\n",
							who.userinfo.netname.c_str(), status.c_str());
	}

	who.playerstate = PST_DISCONNECT;
	SV_UpdatePlayerQueuePositions(G_CanJoinGame, &who);
}

//
// SV_DropClient
// Called when the player is leaving the server unwillingly.
//
void SV_DropClient2(player_t &who, const char* file, const int line)
{
	client_t *cl = &who.client;

	MSG_WriteMarker(&cl->reliablebuf, svc_disconnect);

	SV_SendPacket(who);

	SV_DisconnectClient(who);

	if (::debug_disconnect)
		Printf("  (%s:%d)\n", file, line);
}

//
// SV_SendDisconnectSignal
//
void SV_SendDisconnectSignal()
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		MSG_WriteMarker(&cl->reliablebuf, svc_disconnect);
		SV_SendPacket(*it);

		if (it->mo)
			it->mo->Destroy();
	}

	players.clear();
}

//
// SV_SendReconnectSignal
// All clients will reconnect. Called when the server changes a map
//
void SV_SendReconnectSignal()
{
	// tell others clients about it
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		MSG_WriteMarker(&(it->client.reliablebuf), svc_reconnect);
		SV_SendPacket(*it);

		if (it->mo)
			it->mo->Destroy();
	}

	players.clear();
}

//
// SV_ExitLevel
// Called when sv_timelimit or sv_fraglimit hit.
//
void SV_ExitLevel()
{
	for (Players::iterator it = players.begin(); it != players.end(); ++it)
		MSG_WriteMarker(&(it->client.reliablebuf), svc_exitlevel);
}

//
// Comparison functors for sorting vectors of players
//
struct compare_player_frags
{
	bool operator()(const player_t* arg1, const player_t* arg2) const
	{
		if (!G_IsDuelGame() && G_IsRoundsGame())
		{
			return arg2->totalpoints < arg1->totalpoints;
		}

		return arg2->fragcount < arg1->fragcount;
	}
};

struct compare_player_kills
{
	bool operator()(const player_t* arg1, const player_t* arg2) const
	{
		return arg2->killcount < arg1->killcount;
	}
};

struct compare_player_points
{
	bool operator()(const player_t* arg1, const player_t* arg2) const
	{
		if (G_IsRoundsGame())
		{
			return arg2->totalpoints < arg1->totalpoints;
		}

		return arg2->points < arg1->points;
	}
};

struct compare_player_names
{
	bool operator()(const player_t* arg1, const player_t* arg2) const
	{
		return arg1->userinfo.netname < arg2->userinfo.netname;
	}
};


static float SV_CalculateKillDeathRatio(const player_t* player)
{
	if (player->killcount <= 0)	// Copied from HU_DMScores1.
		return 0.0f;
	else if (player->killcount >= 1 && player->deathcount == 0)
		return float(player->killcount);
	else
		return float(player->killcount) / float(player->deathcount);
}

static float SV_CalculateFragDeathRatio(const player_t* player)
{

	int frags = 0;
	int deaths = 0;

	if (G_IsRoundsGame() && !G_IsDuelGame())
	{
		frags = player->totalpoints;
		deaths = player->totaldeaths;
	}
	else
	{
		frags = player->fragcount;
		deaths = player->deathcount;
	}

	if (frags <= 0) // Copied from HU_DMScores1.
		return 0.0f;
	else if (frags >= 1 && deaths == 0)
		return float(frags);
	else
		return float(frags) / float(deaths);
}

//
// SV_DrawScores
// Draws scoreboard to console. Used during level exit or a command.
//
// [AM] Commonize this with client.
//
void SV_DrawScores()
{
	char str[1024];

	typedef std::list<const player_t*> PlayerPtrList;
	PlayerPtrList sortedplayers;
	PlayerPtrList sortedspectators;

	for (Players::const_iterator it = players.begin(); it != players.end(); ++it)
		if (!it->spectator && it->ingame())
			sortedplayers.push_back(&*it);
		else
			sortedspectators.push_back(&*it);

	Printf_Bold("\n");

	if (sv_gametype == GM_CTF)
	{
		compare_player_points comparison_functor;
		sortedplayers.sort(comparison_functor);

        Printf_Bold("                    CAPTURE THE FLAG");
        Printf_Bold("-----------------------------------------------------------");

		if (sv_scorelimit)
			sprintf(str, "Scorelimit: %-6d", sv_scorelimit.asInt());
		else
			sprintf(str, "Scorelimit: N/A   ");

		Printf_Bold("%s  ", str);

		if (sv_timelimit)
			sprintf(str, "Timelimit: %-7d", sv_timelimit.asInt());
		else
			sprintf(str, "Timelimit: N/A");

		Printf_Bold("%18s\n", str);

		for (int team_num = 0; team_num < sv_teamsinplay; team_num++)
		{
			if (team_num == TEAM_BLUE)
                Printf_Bold("--------------------------------------------------BLUE TEAM");
			else if (team_num == TEAM_RED)
                Printf_Bold("---------------------------------------------------RED TEAM");
			else if (team_num == TEAM_GREEN)
				Printf_Bold("-------------------------------------------------GREEN TEAM");
			else		// shouldn't happen
                Printf_Bold("-----------------------------------------------UNKNOWN TEAM");

            Printf_Bold("ID  Address          Name            Points Caps Frags Time");
            Printf_Bold("-----------------------------------------------------------");

			for (PlayerPtrList::const_iterator it = sortedplayers.begin(); it != sortedplayers.end(); ++it)
			{
				const player_t* itplayer = *it;
				if (itplayer->userinfo.team == team_num)
				{
					Printf_Bold("%-3d %-16s %-15s %-6d N/A  %-5d %-3d",
							itplayer->id,
							NET_AdrToString(itplayer->client.address),
							itplayer->userinfo.netname.c_str(),
							P_GetPointCount(itplayer),
							//itplayer->captures,
					        P_GetFragCount(itplayer),
							itplayer->GameTime / 60);
				}
			}
		}
	}

	else if (sv_gametype == GM_TEAMDM)
	{
		compare_player_frags comparison_functor;
		sortedplayers.sort(comparison_functor);

        Printf_Bold("                     TEAM DEATHMATCH");
        Printf_Bold("-----------------------------------------------------------");

		if (sv_fraglimit)
			sprintf(str, "Fraglimit: %-7d", sv_fraglimit.asInt());
		else
			sprintf(str, "Fraglimit: N/A    ");

		Printf_Bold("%s  ", str);

		if (sv_timelimit)
			sprintf(str, "Timelimit: %-7d", sv_timelimit.asInt());
		else
			sprintf(str, "Timelimit: N/A");

		Printf_Bold("%18s\n", str);

		for (int team_num = 0; team_num < sv_teamsinplay; team_num++)
		{
			if (team_num == TEAM_BLUE)
                Printf_Bold("--------------------------------------------------BLUE TEAM");
			else if (team_num == TEAM_RED)
                Printf_Bold("---------------------------------------------------RED TEAM");
			else if (team_num == TEAM_GREEN)
				Printf_Bold("-------------------------------------------------GREEN TEAM");
			else		// shouldn't happen
                Printf_Bold("-----------------------------------------------UNKNOWN TEAM");

            Printf_Bold("ID  Address          Name            Frags Deaths  K/D Time");
            Printf_Bold("-----------------------------------------------------------");

			for (PlayerPtrList::const_iterator it = sortedplayers.begin(); it != sortedplayers.end(); ++it)
			{
				const player_t* itplayer = *it;
				if (itplayer->userinfo.team == team_num)
				{
					Printf_Bold("%-3d %-16s %-15s %-5d %-6d %2.1f %-3d",
							itplayer->id,
							NET_AdrToString(itplayer->client.address),
							itplayer->userinfo.netname.c_str(),
							P_GetFragCount(itplayer),
							P_GetDeathCount(itplayer),
							SV_CalculateFragDeathRatio(itplayer),
							itplayer->GameTime / 60);
				}
			}
		}
	}

	else if (sv_gametype == GM_DM)
	{
		compare_player_frags comparison_functor;
		sortedplayers.sort(comparison_functor);

        Printf_Bold("                        DEATHMATCH");
        Printf_Bold("-----------------------------------------------------------");

		if (sv_fraglimit)
			sprintf(str, "Fraglimit: %-7d", sv_fraglimit.asInt());
		else
			sprintf(str, "Fraglimit: N/A    ");

		Printf_Bold("%s  ", str);

		if (sv_timelimit)
			sprintf(str, "Timelimit: %-7d", sv_timelimit.asInt());
		else
			sprintf(str, "Timelimit: N/A");

		Printf_Bold("%18s\n", str);

        Printf_Bold("ID  Address          Name            Frags Deaths  K/D Time");
        Printf_Bold("-----------------------------------------------------------");

		for (PlayerPtrList::const_iterator it = sortedplayers.begin(); it != sortedplayers.end(); ++it)
		{
			const player_t* itplayer = *it;
			Printf_Bold("%-3d %-16s %-15s %-5d %-6d %2.1f %-3d",
					itplayer->id,
					NET_AdrToString(itplayer->client.address),
					itplayer->userinfo.netname.c_str(),
					P_GetFragCount(itplayer),
					P_GetDeathCount(itplayer),
					SV_CalculateFragDeathRatio(itplayer),
					itplayer->GameTime / 60);
		}

	}

	else if (sv_gametype == GM_COOP)
	{
		compare_player_kills comparison_functor;
		sortedplayers.sort(comparison_functor);

        Printf_Bold("                       COOPERATIVE");
        Printf_Bold("-----------------------------------------------------------");
        Printf_Bold("ID  Address          Name            Kills Deaths  K/D Time");
        Printf_Bold("-----------------------------------------------------------");

		for (PlayerPtrList::const_iterator it = sortedplayers.begin(); it != sortedplayers.end(); ++it)
		{
			const player_t* itplayer = *it;
			Printf_Bold("%-3d %-16s %-15s %-5d %-6d %2.1f %-3d",
					itplayer->id,
					NET_AdrToString(itplayer->client.address),
					itplayer->userinfo.netname.c_str(),
					itplayer->killcount,
					itplayer->deathcount,
					SV_CalculateKillDeathRatio(itplayer),
					itplayer->GameTime / 60);
		}
	}

	if (!sortedspectators.empty())
	{
		compare_player_names comparison_functor;
		sortedspectators.sort(comparison_functor);

    	Printf_Bold("-------------------------------------------------SPECTATORS");

		for (PlayerPtrList::const_iterator it = sortedspectators.begin(); it != sortedspectators.end(); ++it)
		{
			const player_t* itplayer = *it;
			Printf_Bold("%-3d %-16s %-15s\n",
					itplayer->id,
					NET_AdrToString(itplayer->client.address),
					itplayer->userinfo.netname.c_str());
		}
	}

	Printf_Bold("\n");
}

BEGIN_COMMAND (showscores)
{
    SV_DrawScores();
}
END_COMMAND (showscores)

FORMAT_PRINTF(2, 3)
void STACK_ARGS SV_BroadcastPrintf(int printlevel, const char* format, ...)
{
	va_list argptr;
	std::string string;
	client_t* cl;

	va_start(argptr, format);
	VStrFormat(string, format, argptr);
	va_end(argptr);

	Printf(printlevel, "%s", string.c_str()); // print to the console

	// Hacky code to display messages as normal ones to clients
	if (printlevel == PRINT_NORCON)
		printlevel = PRINT_HIGH;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		cl = &(it->client);

		MSG_WriteMarker (&cl->reliablebuf, svc_print);
		MSG_WriteByte (&cl->reliablebuf, (byte)printlevel);
		MSG_WriteString (&cl->reliablebuf, string.c_str());
	}
}

FORMAT_PRINTF(1, 2)
void STACK_ARGS SV_BroadcastPrintf(const char* fmt, ...)
{
	va_list argptr;
	char string[2048];
	client_t* cl;

	va_start(argptr, fmt);
	vsnprintf(string, sizeof(string), fmt, argptr);
	va_end(argptr);

	SV_BroadcastPrintf(PRINT_NORCON, "%s", string);
}

// GhostlyDeath -- same as above but ONLY for spectators
void STACK_ARGS SV_SpectatorPrintf(int level, const char *fmt, ...)
{
	va_list argptr;
	char string[2048];
	client_t *cl;

	va_start(argptr,fmt);
	vsprintf(string, fmt,argptr);
	va_end(argptr);

	Printf(level, "%s", string);  // print to the console

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		cl = &(it->client);

		bool spectator = it->spectator || !it->ingame();
		if (spectator)
		{
			MSG_WriteMarker(&cl->reliablebuf, svc_print);
			MSG_WriteByte(&cl->reliablebuf, level);
			MSG_WriteString(&cl->reliablebuf, string);
		}
	}
}

// Print directly to a specific client.
void STACK_ARGS SV_ClientPrintf(client_t *cl, int level, const char *fmt, ...)
{
	va_list argptr;
	char string[2048];

	va_start(argptr, fmt);
	vsprintf(string, fmt, argptr);
	va_end(argptr);

	MSG_WriteMarker(&cl->reliablebuf, svc_print);
	MSG_WriteByte(&cl->reliablebuf, level);
	MSG_WriteString(&cl->reliablebuf, string);
}

// Print directly to a specific player.
void STACK_ARGS SV_PlayerPrintf(int level, int player_id, const char *fmt, ...)
{
	va_list argptr;
	char string[2048];

	va_start(argptr,fmt);
	vsprintf(string, fmt,argptr);
	va_end(argptr);

	client_t* cl = &idplayer(player_id).client;
	MSG_WriteMarker(&cl->reliablebuf, svc_print);
	MSG_WriteByte(&cl->reliablebuf, level);
	MSG_WriteString(&cl->reliablebuf, string);
}

void STACK_ARGS SV_TeamPrintf(int level, int who, const char *fmt, ...)
{
	if (sv_gametype != GM_TEAMDM && sv_gametype != GM_CTF)
		return;

	va_list argptr;
	char string[2048];

	va_start(argptr,fmt);
	vsprintf(string, fmt,argptr);
	va_end(argptr);

	Printf(level, "%s", string);  // print to the console

	player_t* player = &idplayer(who);

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		if (it->userinfo.team != player->userinfo.team)
			continue;

		bool spectator = it->spectator || !it->ingame();
		if (spectator)
			continue;

		client_t* cl = &(it->client);

		if (cl->allow_rcon) // [mr.crispy -- sept 23 2013] RCON guy already got it when it printed to the console
			continue;

		MSG_WriteMarker(&cl->reliablebuf, svc_print);
		MSG_WriteByte(&cl->reliablebuf, level);
		MSG_WriteString(&cl->reliablebuf, string);
	}
}

/**
 * Send a message to teammates of a player.
 *
 * @param player  Player who said the message.
 * @param message Message that the player said.
 */
void SVC_TeamSay(player_t &player, const char* message)
{
	const char* team = GetTeamInfo(player.userinfo.team)->ColorStringUpper.c_str();

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		// Player needs to be valid.
		if (!validplayer(*it))
			continue;

		bool spectator = it->spectator || !it->ingame();

		// Player needs to be on the same team
		if (spectator || it->userinfo.team != player.userinfo.team)
			continue;

		MSG_WriteMarker(&(it->client.reliablebuf), svc_say);
		MSG_WriteByte(&(it->client.reliablebuf), 1);
		MSG_WriteByte(&(it->client.reliablebuf), player.id);
		MSG_WriteString(&(it->client.reliablebuf), message);
	}
}

/**
 * Send a message to all spectators.
 *
 * @param player  Player who said the message.
 * @param message Message that the player said.
 */
void SVC_SpecSay(player_t &player, const char* message)
{
	if (strnicmp(message, "/me ", 4) == 0)
		Printf(PRINT_TEAMCHAT, "<SPEC> * %s %s\n", player.userinfo.netname.c_str(), &message[4]);
	else
		Printf(PRINT_TEAMCHAT, "<SPEC> %s: %s\n", player.userinfo.netname.c_str(), message);

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		// Player needs to be valid.
		if (!validplayer(*it))
			continue;

		bool spectator = it->spectator || !it->ingame();

		if (!spectator)
			continue;

		MSG_WriteMarker(&(it->client.reliablebuf), svc_say);
		MSG_WriteByte(&(it->client.reliablebuf), 1);
		MSG_WriteByte(&(it->client.reliablebuf), player.id);
		MSG_WriteString(&(it->client.reliablebuf), message);
	}
}

/**
 * Send a message to everybody.
 *
 * @param player  Player who said the message.
 * @param message Message that the player said.
 */
void SVC_Say(player_t &player, const char* message)
{
	if (strnicmp(message, "/me ", 4) == 0)
		Printf(PRINT_CHAT, "<CHAT> * %s %s\n", player.userinfo.netname.c_str(), &message[4]);
	else
		Printf(PRINT_CHAT, "<CHAT> %s: %s\n", player.userinfo.netname.c_str(), message);

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		// Player needs to be valid.
		if (!validplayer(*it))
			continue;

		MSG_WriteMarker(&(it->client.reliablebuf), svc_say);
		MSG_WriteByte(&(it->client.reliablebuf), 0);
		MSG_WriteByte(&(it->client.reliablebuf), player.id);
		MSG_WriteString(&(it->client.reliablebuf), message);
	}
}

/**
 * Send a message to a specific player from a specific other player.
 *
 * @param player  Sending player.
 * @param dplayer Player to send to.
 * @param message Message to send.
 */
void SVC_PrivMsg(player_t &player, player_t &dplayer, const char* message)
{
	if (strnicmp(message, "/me ", 4) == 0)
		Printf(PRINT_CHAT, "<PRIVMSG> * %s (to %s) %s\n",
				player.userinfo.netname.c_str(), dplayer.userinfo.netname.c_str(), &message[4]);
	else
		Printf(PRINT_CHAT, "<PRIVMSG> %s (to %s): %s\n",
				player.userinfo.netname.c_str(), dplayer.userinfo.netname.c_str(), message);

	MSG_WriteMarker(&dplayer.client.reliablebuf, svc_say);
	MSG_WriteByte(&dplayer.client.reliablebuf, 1);
	MSG_WriteByte(&dplayer.client.reliablebuf, player.id);
	MSG_WriteString(&dplayer.client.reliablebuf, message);

	// [AM] Send a duplicate message to the sender, so he knows the message
	//      went through.
	if (player.id != dplayer.id)
	{
		MSG_WriteMarker(&player.client.reliablebuf, svc_say);
		MSG_WriteByte(&player.client.reliablebuf, 1);
		MSG_WriteByte(&player.client.reliablebuf, player.id);
		MSG_WriteString(&player.client.reliablebuf, message);
	}
}

//
// SV_Say
// Show a chat string and send it to others clients.
//
bool SV_Say(player_t &player)
{
	byte message_visibility = MSG_ReadByte();

	std::string message(MSG_ReadString());
	StripColorCodes(message);

	if (!ValidString(message))
	{
		SV_InvalidateClient(player, "Chatstring contains invalid characters");
		return false;
	}

	if (message.empty() || message.length() > MAX_CHATSTR_LEN)
		return true;

	// Flood protection
	if (player.LastMessage.Time)
	{
		const dtime_t min_diff = I_ConvertTimeFromMs(1000) * sv_flooddelay;
		dtime_t diff = I_GetTime() - player.LastMessage.Time;

		if (diff < min_diff)
			return true;

		player.LastMessage.Time = 0;
	}

	if (player.LastMessage.Time == 0)
	{
		player.LastMessage.Time = I_GetTime();
		player.LastMessage.Message = message;
	}

	bool spectator = player.spectator || !player.ingame();

	if (message_visibility == 0)
	{
		if (spectator && !sv_globalspectatorchat)
			SVC_SpecSay(player, message.c_str());
		else
			SVC_Say(player, message.c_str());
	}
	else if (message_visibility == 1)
	{
		if (spectator)
			SVC_SpecSay(player, message.c_str());
		else if (G_IsTeamGame())
			SVC_TeamSay(player, message.c_str());
		else
			SVC_Say(player, message.c_str());
	}

	return true;
}

//
// SV_PrivMsg
// Show a chat string and show it to a single other client.
//
bool SV_PrivMsg(player_t &player)
{
	player_t& dplayer = idplayer(MSG_ReadByte());

	std::string str(MSG_ReadString());
	StripColorCodes(str);

	if (!ValidString(str))
	{
		SV_InvalidateClient(player, "Private Message contains invalid characters");
		return false;
	}

	if (!validplayer(dplayer))
		return true;

	if (str.empty() || str.length() > MAX_CHATSTR_LEN)
		return true;

	// In competitive gamemodes, don't allow spectators to message players.
	if (sv_gametype != GM_COOP && player.spectator && !dplayer.spectator)
		return true;

	// Flood protection
	if (player.LastMessage.Time)
	{
		const dtime_t min_diff = I_ConvertTimeFromMs(1000) * sv_flooddelay;
		dtime_t diff = I_GetTime() - player.LastMessage.Time;

		if (diff < min_diff)
			return true;

		player.LastMessage.Time = 0;
	}

	if (player.LastMessage.Time == 0)
	{
		player.LastMessage.Time = I_GetTime();
		player.LastMessage.Message = str;
	}

	SVC_PrivMsg(player, dplayer, str.c_str());

	return true;
}

//
// SV_UpdateMissiles
// Updates missiles position sometimes.
//
void SV_UpdateMissiles(player_t &pl)
{
    AActor *mo;

    TThinkerIterator<AActor> iterator;
    while ( (mo = iterator.Next() ) )
    {
        if (!(mo->flags & MF_MISSILE || mo->flags & MF_SKULLFLY))
			continue;

		if (mo->type == MT_PLASMA)
			continue;

		// update missile position every 30 tics
		if (((gametic+mo->netid) % 30) && (mo->type != MT_TRACER) && (mo->type != MT_FATSHOT))
			continue;
		// Revenant tracers and Mancubus fireballs need to be  updated more often
		else if (((gametic+mo->netid) % 5) && (mo->type == MT_TRACER || mo->type == MT_FATSHOT))
			continue;

		if(SV_IsPlayerAllowedToSee(pl, mo))
		{
			client_t *cl = &pl.client;

			MSG_WriteMarker (&cl->netbuf, svc_movemobj);
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
			MSG_WriteByte (&cl->netbuf, mo->rndindex);
			MSG_WriteLong (&cl->netbuf, mo->x);
			MSG_WriteLong (&cl->netbuf, mo->y);
			MSG_WriteLong (&cl->netbuf, mo->z);

			MSG_WriteMarker (&cl->netbuf, svc_mobjspeedangle);
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
			MSG_WriteLong (&cl->netbuf, mo->angle);
			MSG_WriteLong (&cl->netbuf, mo->momx);
			MSG_WriteLong (&cl->netbuf, mo->momy);
			MSG_WriteLong (&cl->netbuf, mo->momz);

			if (mo->tracer)
			{
				MSG_WriteMarker (&cl->netbuf, svc_actor_tracer);
				MSG_WriteUnVarint(&cl->netbuf, mo->netid);
				MSG_WriteUnVarint(&cl->netbuf, mo->tracer->netid);
			}

            if (cl->netbuf.cursize >= 1024)
                if(!SV_SendPacket(pl))
                    return;
		}
    }
}

// Update the given actors state immediately.
void SV_UpdateMobjState(AActor *mo)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (!(it->ingame()))
			continue;

		if (SV_IsPlayerAllowedToSee(*it, mo))
		{
			client_t *cl = &(it->client);
			statenum_t mostate = (statenum_t)(mo->state - states);

			MSG_WriteMarker(&cl->reliablebuf, svc_mobjstate);
			MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
			MSG_WriteShort(&cl->reliablebuf, (short)mostate);
		}
	}
}

// Keep tabs on monster positions and angles.
void SV_UpdateMonsters(player_t &pl)
{
	AActor *mo;

	TThinkerIterator<AActor> iterator;
	while ((mo = iterator.Next()))
	{
		// Ignore corpses.
		if (mo->flags & MF_CORPSE)
			continue;

		// We don't handle updating non-monsters here.
		if (!(mo->flags & MF_COUNTKILL || mo->type == MT_SKULL))
			continue;

		// update monster position every 7 tics
		if ((gametic+mo->netid) % 7)
			continue;

		if (SV_IsPlayerAllowedToSee(pl, mo) && mo->target)
		{
			client_t *cl = &pl.client;

			MSG_WriteMarker(&cl->netbuf, svc_movemobj);
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
			MSG_WriteByte(&cl->netbuf, mo->rndindex);
			MSG_WriteLong(&cl->netbuf, mo->x);
			MSG_WriteLong(&cl->netbuf, mo->y);
			MSG_WriteLong(&cl->netbuf, mo->z);

			MSG_WriteMarker(&cl->netbuf, svc_mobjspeedangle);
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
			MSG_WriteLong(&cl->netbuf, mo->angle);
			MSG_WriteLong(&cl->netbuf, mo->momx);
			MSG_WriteLong(&cl->netbuf, mo->momy);
			MSG_WriteLong(&cl->netbuf, mo->momz);

			MSG_WriteMarker(&cl->netbuf, svc_actor_movedir);
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
			MSG_WriteByte(&cl->netbuf, mo->movedir);
			MSG_WriteLong(&cl->netbuf, mo->movecount);

			MSG_WriteMarker(&cl->netbuf, svc_actor_target);
			MSG_WriteUnVarint(&cl->netbuf, mo->netid);
			MSG_WriteUnVarint(&cl->netbuf, mo->target->netid);

			if (cl->netbuf.cursize >= 1024)
			{
				if (!SV_SendPacket(pl))
					return;
			}
		}
	}
}

//
// SV_ActorTarget
//
void SV_ActorTarget(AActor *actor)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (!(it->ingame()))
			continue;

		client_t *cl = &(it->client);

		if(!SV_IsPlayerAllowedToSee(*it, actor))
			continue;

		MSG_WriteMarker (&cl->reliablebuf, svc_actor_target);
		MSG_WriteUnVarint(&cl->reliablebuf, actor->netid);
		MSG_WriteUnVarint(&cl->reliablebuf, actor->target ? actor->target->netid : 0);
	}
}

//
// SV_ActorTracer
//
void SV_ActorTracer(AActor *actor)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (!(it->ingame()))
			continue;

		client_t *cl = &(it->client);

		MSG_WriteMarker (&cl->reliablebuf, svc_actor_tracer);
		MSG_WriteUnVarint(&cl->reliablebuf, actor->netid);
		MSG_WriteUnVarint(&cl->reliablebuf, actor->tracer ? actor->tracer->netid : 0);
	}
}

//
// SV_RemoveCorpses
// Removes some corpses
//
void SV_RemoveCorpses (void)
{
	AActor *mo;
	int     corpses = 0;

	// joek - Number of corpses infinite
	if(sv_maxcorpses <= 0)
		return;

	if (!P_AtInterval(TICRATE))
		return;
	else
	{
		TThinkerIterator<AActor> iterator;
		while ( (mo = iterator.Next() ) )
		{
			if (mo->type == MT_PLAYER && (!mo->player || mo->health <=0) )
				corpses++;
		}
	}

	TThinkerIterator<AActor> iterator;
	while (corpses > sv_maxcorpses && (mo = iterator.Next() ) )
	{
		if (mo->type == MT_PLAYER && !mo->player)
		{
			mo->Destroy();
			corpses--;
		}
	}
}

//
// SV_SendPingRequest
// Pings the client and requests a reply
//
// [SL] 2011-05-11 - Changed from SV_SendGametic to SV_SendPingRequest
//
void SV_SendPingRequest(client_t* cl)
{
	if (!P_AtInterval(100))
		return;

	MSG_WriteMarker (&cl->reliablebuf, svc_pingrequest);
	MSG_WriteLong (&cl->reliablebuf, I_MSTime());
}

void SV_UpdateMonsterRespawnCount()
{
	if (sv_gametype != GM_COOP)
		return;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		client_t* cl = &(it->client);
		SVC_LevelLocals(cl->reliablebuf, ::level, SVC_LL_MONSTER_RESPAWNS);
	}
}

// calculates ping using gametic which was sent by SV_SendGametic and
// current gametic
void SV_CalcPing(player_t &player)
{
	unsigned int ping = I_MSTime() - MSG_ReadLong();

	if(ping > 999)
		ping = 999;

	player.ping = ping;
}

//
// SV_UpdatePing
// send pings to a client
//
void SV_UpdatePing(client_t* cl)
{
	if (!P_AtInterval(101))
		return;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		if (!(it->ingame()))
			continue;

		MSG_WriteMarker(&cl->reliablebuf, svc_updateping);
		MSG_WriteByte(&cl->reliablebuf, it->id);  // player
		MSG_WriteLong(&cl->reliablebuf, it->ping);
	}
}


//
// SV_UpdateDeadPlayers
// Update player's frame while he's dying.
//
void SV_UpdateDeadPlayers()
{
 /*   AActor *mo;

    TThinkerIterator<AActor> iterator;
    while ( (mo = iterator.Next() ) )
    {
        if (mo->type != MT_PLAYER || mo->player)
			continue;

		if (mo->oldframe != mo->frame)
			for (size_t i = 0; i < players.size(); i++)
			{
				client_t *cl = &clients[i];

				MSG_WriteMarker (&cl->reliablebuf, svc_mobjframe);
				MSG_WriteUnVarint (&cl->reliablebuf, mo->netid);
				MSG_WriteByte (&cl->reliablebuf, mo->frame);
			}

		mo->oldframe = mo->frame;
    }
*/
}


//
// SV_ClearClientsBPS
//
void SV_ClearClientsBPS(void)
{
	if (!P_AtInterval(TICRATE))
		return;

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		cl->reliable_bps = 0;
		cl->unreliable_bps = 0;
	}
}

//
// SV_SendPackets
//
void SV_SendPackets()
{
	if (players.empty())
		return;

	static size_t fair_send = 0;
	size_t num_players = players.size();

	// Wrap the starting index around if necessary.
	if (fair_send >= num_players)
		fair_send = 0;

	// Shift the starting point.
	Players::iterator begin = players.begin();
	for (size_t i = 0;i < fair_send;i++)
		++begin;

	// Loop through all players in a staggered fashion.
	Players::iterator it = begin;
	do
	{
		// [AM] Don't send packets to players who haven't acked packet 0
		if (it->playerstate != PST_CONTACT)
			SV_SendPacket(*it);

		++it;
		if (it == players.end())
			it = players.begin();
	}
	while (it != begin);

	// Advance the send index.
	fair_send++;
}

void SV_SendPlayerStateUpdate(client_t *client, player_t *player)
{
	if (!client || !player || !player->mo)
		return;

	SVC_PlayerState(client->netbuf, *player);
}

void SV_SpyPlayer(player_t &viewer)
{
	byte id = MSG_ReadByte();

	player_t &other = idplayer(id);
	if (!validplayer(other) || !P_CanSpy(viewer, other))
		return;

	viewer.spying = id;
	SV_SendPlayerStateUpdate(&viewer.client, &other);
}

//
// SV_WriteCommands
//
void SV_WriteCommands(void)
{
	// [SL] 2011-05-11 - Save player positions and moving sector heights so
	// they can be reconciled later for unlagging
	Unlag::getInstance().recordPlayerPositions();
	Unlag::getInstance().recordSectorPositions();

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		client_t *cl = &(it->client);

		// [SL] 2011-05-11 - Send the client the server's gametic
		// this gametic is returned to the server with the client's
		// next cmd
		if (it->ingame())
			SV_SendGametic(cl);

		for (Players::iterator pit = players.begin();pit != players.end();++pit)
		{
			if (!(pit->ingame()) || !(pit->mo))
				continue;

			// a player is updated about their own position elsewhere
			if (&*it == &*pit)
				continue;

			// GhostlyDeath -- Screw spectators
			if (pit->spectator)
				continue;

			if(!SV_IsPlayerAllowedToSee(*it, pit->mo))
				continue;

			MSG_WriteMarker(&cl->netbuf, svc_moveplayer);
			MSG_WriteByte(&cl->netbuf, pit->id); // player number

			// [SL] 2011-09-14 - the most recently processed ticcmd from the
			// client we're sending this message to.
			MSG_WriteLong(&cl->netbuf, it->tic);

			MSG_WriteLong(&cl->netbuf, pit->mo->x);
			MSG_WriteLong(&cl->netbuf, pit->mo->y);
			MSG_WriteLong(&cl->netbuf, pit->mo->z);

			if (GAMEVER > 60)
			{
				MSG_WriteShort(&cl->netbuf, pit->mo->angle >> FRACBITS);
				MSG_WriteShort(&cl->netbuf, pit->mo->pitch >> FRACBITS);
			}
			else
			{
				MSG_WriteLong(&cl->netbuf, pit->mo->angle);
			}

			if (pit->mo->frame == 32773)
				MSG_WriteByte(&cl->netbuf, PLAYER_FULLBRIGHTFRAME);
			else
				MSG_WriteByte(&cl->netbuf, pit->mo->frame);

			// write velocity
			MSG_WriteLong(&cl->netbuf, pit->mo->momx);
			MSG_WriteLong(&cl->netbuf, pit->mo->momy);
			MSG_WriteLong(&cl->netbuf, pit->mo->momz);

			// [Russell] - hack, tell the client about the partial
			// invisibility power of another player.. (cheaters can disable
			// this but its all we have for now)
			if (GAMEVER > 60)
				MSG_WriteByte(&cl->netbuf, pit->powers[pw_invisibility]);
			else
				MSG_WriteLong(&cl->netbuf, pit->powers[pw_invisibility]);
		}

		// [SL] Send client info about player he is spying on
		player_t *target = &idplayer(it->spying);
		if (validplayer(*target) && &(*it) != target && P_CanSpy(*it, *target))
			SV_SendPlayerStateUpdate(&(it->client), target);

		SV_UpdateConsolePlayer(*it);

		SV_UpdateMissiles(*it);

		SV_UpdateMonsters(*it);

		SV_SendPingRequest(cl);     // request ping reply

		SV_UpdatePing(cl);          // send the ping value of all cients to this client
	}

	SV_UpdateHiddenMobj();

	SV_UpdateDeadPlayers(); // Update dying players.
}

void SV_PlayerTriedToCheat(player_t &player)
{
	SV_BroadcastPrintf("%s tried to cheat!\n", player.userinfo.netname.c_str());
	SV_DropClient(player);
}

//
// SV_FlushPlayerCmds
//
// Clears a player's queue of ticcmds, ignoring and discarding them
//
void SV_FlushPlayerCmds(player_t &player)
{
	std::queue<NetCommand> empty;
	std::swap(player.cmdqueue, empty);
}

//
// SV_CalculateNumTiccmds
//
// [SL] 2011-09-16 - Calculate how many ticcmds should be processed.  Under
// most circumstances, it should be 1 per gametic to have the smoothest
// player movement possible.
//
int SV_CalculateNumTiccmds(player_t &player)
{
	if (!player.mo || player.cmdqueue.empty())
		return 0;

	static const size_t maximum_queue_size = TICRATE / 4;

	if (!sv_ticbuffer || player.spectator || player.playerstate == PST_DEAD)
	{
		// Process all queued ticcmds.
		return maximum_queue_size;
	}
	if (player.mo->momx == 0 && player.mo->momy == 0 && player.mo->momz == 0)
	{
		// Player is not moving
		return 2;
	}
	if (player.cmdqueue.size() > 2 && gametic % 2*TICRATE == player.id % 2*TICRATE)
	{
		// Process an extra ticcmd once every 2 seconds to reduce the
		// queue size. Use player id to stagger the timing to prevent everyone
		// from running an extra ticcmd at the same time.
		return 2;
	}
	if (player.cmdqueue.size() > maximum_queue_size)
	{
		// The player experienced a large latency spike so try to catch up by
		// processing more than one ticcmd at the expense of appearing perfectly
		//  smooth
		return 2;
	}

	// always run at least 1 ticcmd if possible
	return 1;
}

//
// SV_ProcessPlayerCmd
//
// Decides how many of a player's queued ticcmds should be processed and
// prepares the player.cmd structure for P_PlayerThink().  Also has the
// responsibility of ensuring the Unlag class has the appropriate latency
// for the player whose ticcmd we're processing.
//
void SV_ProcessPlayerCmd(player_t &player)
{
	const int max_forward_move = 50 << 8;
	#if 0
	const int max_sr40_side_move = 40 << 8;
	#endif
	const int max_sr50_side_move = 50 << 8;

	if (player.joindelay)
		player.joindelay--;
	if (player.suicidedelay)
		player.suicidedelay--;

	if (!validplayer(player) || !player.mo)
		return;

	#ifdef _TICCMD_QUEUE_DEBUG_
	DPrintf("Cmd queue size for %s: %d\n",
				player.userinfo.netname, player.cmdqueue.size());
	#endif	// _TICCMD_QUEUE_DEBUG_

	int num_cmds = SV_CalculateNumTiccmds(player);

	for (int i = 0; i < num_cmds && !player.cmdqueue.empty(); i++)
	{
		NetCommand *netcmd = &(player.cmdqueue.front());
		player.cmd = ticcmd_t();
		player.tic = netcmd->getTic();

		// Set the latency amount for Unlagging
		Unlag::getInstance().setRoundtripDelay(player.id, netcmd->getWorldIndex() & 0xFF);

		if ((netcmd->hasForwardMove() && abs(netcmd->getForwardMove()) > max_forward_move) ||
		    (netcmd->hasSideMove() && abs(netcmd->getSideMove()) > max_sr50_side_move))
		{
			SV_PlayerTriedToCheat(player);
			return;
		}

		#if 0
		if ((netcmd->hasSideMove() && abs(netcmd->getSideMove()) > max_sr40_side_move) &&
		    (player.mo && player.mo->prevangle != netcmd->getAngle()))
		{
			// verify SR50 isn't combined with yaw
			SV_PlayerTriedToCheat(player);
			return;
		}
		#endif

		netcmd->toPlayer(&player);

		if (!sv_freelook)
			player.mo->pitch = 0;

		// Apply this ticcmd using the game logic
		if (gamestate == GS_LEVEL)
		{
			P_PlayerThink(&player);
			player.mo->RunThink();
		}

		player.cmdqueue.pop();		// remove this tic from the queue after being processed
	}
}

//
// SV_GetPlayerCmd
//
// Extracts a player's ticcmd message from their network buffer and queues
// the ticcmd for later processing.  The client always sends its previous
// ticcmd followed by its current ticcmd just in case there is a dropped
// packet.

void SV_GetPlayerCmd(player_t &player)
{
	client_t *cl = &player.client;

	// The client-tic at the time this message was sent.  The server stores
	// this and sends it back the next time it tells the client
	int tic = MSG_ReadLong();

	// Read the last 10 ticcmds from the client and add any new ones
	// to the cmdqueue
	for (int i = 9; i >= 0; i--)
	{
		NetCommand netcmd;
		netcmd.read(&net_message);
		netcmd.setTic(tic - i);

		if (netcmd.getTic() > cl->lastclientcmdtic && gamestate == GS_LEVEL)
		{
			if (!player.spectator)
				player.cmdqueue.push(netcmd);
			cl->lastclientcmdtic = netcmd.getTic();
			cl->lastcmdtic = gametic;
		}
	}
}

void SV_UpdateConsolePlayer(player_t &player)
{
	AActor *mo = player.mo;
	client_t *cl = &player.client;

	if (!mo)
		return;

	// GhostlyDeath -- Spectators are on their own really
	if (player.spectator)
	{
        SV_UpdateMovingSectors(player);
		return;
	}

	// client player will update his position if packets were missed
	MSG_WriteMarker (&cl->netbuf, svc_updatelocalplayer);

	// client-tic of the most recently processed ticcmd for this client
	MSG_WriteLong (&cl->netbuf, player.tic);

	MSG_WriteLong (&cl->netbuf, mo->x);
	MSG_WriteLong (&cl->netbuf, mo->y);
	MSG_WriteLong (&cl->netbuf, mo->z);

	MSG_WriteLong (&cl->netbuf, mo->momx);
	MSG_WriteLong (&cl->netbuf, mo->momy);
	MSG_WriteLong (&cl->netbuf, mo->momz);

    MSG_WriteByte (&cl->netbuf, mo->waterlevel);

    SV_UpdateMovingSectors(player);
}

//
//	SV_ChangeTeam
//																							[Toke - CTF]
//	Allows players to change teams properly in teamplay and CTF
//
void SV_ChangeTeam (player_t &player)  // [Toke - Teams]
{
	team_t team = (team_t)MSG_ReadByte();

	if (team >= TEAM_NONE || team < 0)
		return;

	if (team >= sv_teamsinplay)
		return;

	team_t old_team = player.userinfo.team;
	player.userinfo.team = team;

	if (G_IsTeamGame() && player.mo && player.userinfo.team != old_team &&
	    !G_IsLevelState(LevelState::WARMUP))
	{
		P_DamageMobj(player.mo, 0, 0, 1000, 0);
	}
	SV_BroadcastPrintf("%s has joined the %s team.\n", player.userinfo.netname.c_str(),
	                   V_GetTeamColor(team).c_str());

	// Team changes can result with not enough players on a team.
	G_AssertValidPlayerCount();
}

//
// SV_Spectate
//
void SV_Spectate(player_t &player)
{
	// [AM] Code has three possible values; true, false and 5.  True specs the
	//      player, false unspecs him and 5 updates the server with the spec's
	//      new position.
	byte Code = MSG_ReadByte();

	if (!player.ingame())
		return;

	if (Code == 5)
	{
		// GhostlyDeath -- Prevent Cheaters
		if (!player.spectator || !player.mo)
		{
			for (int i = 0; i < 3; i++)
				MSG_ReadLong();
			return;
		}

		// GhostlyDeath -- Code 5! Anyway, this just updates the player for "antiwallhack" fun
		player.mo->x = MSG_ReadLong();
		player.mo->y = MSG_ReadLong();
		player.mo->z = MSG_ReadLong();
	}
	else
	{
		SV_SetPlayerSpec(player, Code);
	}
}

// Change a player into a spectator or vice-versa.  Pass 'true' for silent
// param to spec or unspec the player without a broadcasted message.
void P_SetSpectatorFlags(player_t &player);

void SV_SetPlayerSpec(player_t &player, bool setting, bool silent)
{
	if (player.ingame() == false)
		return;

	if (!setting && player.spectator)
	{
		// Join delay means they're mashing buttons too fast.
		if (player.joindelay > 0)
			return;

		if (G_CanJoinGame() != JOIN_OK)
		{
			// Not allowed to join yet - add them to the queue.
			if (player.QueuePosition == 0)
				SV_AddPlayerToQueue(&player);

			return;
		}
		SV_JoinPlayer(player, silent);
	}
	else if (setting && !player.spectator)
		SV_SpecPlayer(player, silent);
	else if (setting && player.spectator && player.QueuePosition > 0)
		SV_RemovePlayerFromQueue(&player);
}

/**
 * @brief Have a player join the game.  Note that this function does no 
 *        checking against maxplayers or round limits or whatever, that's
 *        the job of the caller.
 * 
 * @param player Player that should join the game.
 * @param silent True if the join should be done "silently".
*/
void SV_JoinPlayer(player_t& player, bool silent)
{
	// Figure out which team the player should be assigned to.
	if (G_IsTeamGame())
	{
		bool invalidteam = player.userinfo.team >= sv_teamsinplay;
		bool toomanyplayers =
		    sv_maxplayersperteam &&
		    P_NumPlayersOnTeam(player.userinfo.team) >= sv_maxplayersperteam;
		if (invalidteam || toomanyplayers)
		{
			// If this check fails, our "CanJoin" function didn't do a good-enough
			// job of scoping out a potential team.
			team_t newteam = SV_GoodTeam();
			if (newteam == TEAM_NONE)
				return;

			SV_ForceSetTeam(player, newteam);
			SV_CheckTeam(player);
		}
	}

	// [SL] 2011-09-01 - Clear any previous SV_MidPrint (sv_motd for example)
	SV_MidPrint("", &player, 0);

	// Warn everyone we're not a spectator anymore.
	player.spectator = false;

	// Whatever mobj we had it doesn't matter anymore.
	if (player.mo)
		P_KillMobj(NULL, player.mo, NULL, true);

	// Fresh joins get fresh player scores.
	P_ClearPlayerScores(player, SCORES_CLEAR_ALL);

	// Ensure our player is in the ENTER state.
	player.playerstate = PST_ENTER;

	// Set player unready if we're in warmup mode.
	if (sv_warmup)
	{
		SV_SetReady(player, false, true);
		player.timeout_ready = 0;
	}

	// Finally, persist info about our freshly-joining player to the world.
	for (Players::iterator it = ::players.begin(); it != ::players.end(); ++it)
	{
		if (!it->ingame())
			continue;

		SVC_PlayerMembers(it->client.reliablebuf, player, SVC_MSG_ALL);
	}

	// Everything is set, now warn everyone the player joined.
	if (!silent)
	{
		if (sv_gametype != GM_TEAMDM && sv_gametype != GM_CTF)
			SV_BroadcastPrintf("%s joined the game.\n",
			                   player.userinfo.netname.c_str());
		else
			SV_BroadcastPrintf("%s joined the game on the %s team.\n",
			                   player.userinfo.netname.c_str(),
			                   V_GetTeamColor(player.userinfo.team).c_str());
	}
}

void SV_SpecPlayer(player_t &player, bool silent)
{
	// call CTF_CheckFlags _before_ the player becomes a spectator.
	// Otherwise a flag carrier will drop his flag at (0,0), which
	// is often right next to one of the bases...
	if (sv_gametype == GM_CTF)
		CTF_CheckFlags(player);

	// [tm512 2014/04/18] Avoid setting spectator flags on a dead player
	// Instead we respawn the player, move him back, and immediately spectate him afterwards
	if (player.playerstate == PST_DEAD)
		G_DoReborn(player);

	player.spectator = true;
	for (Players::iterator it = ::players.begin(); it != ::players.end(); ++it)
		SVC_PlayerMembers(it->client.reliablebuf, player, SVC_PM_SPECTATOR);

	// [AM] Set player unready if we're in warmup mode.
	if (sv_warmup)
	{
		SV_SetReady(player, false, true);
		player.timeout_ready = 0;
	}

	player.playerstate = PST_LIVE;
	player.joindelay = ReJoinDelay;

	P_SetSpectatorFlags(player);

	if (!silent)
		SV_BroadcastPrintf(PRINT_HIGH, "%s became a spectator.\n", player.userinfo.netname.c_str());

	P_PlayerLeavesGame(&player);
	SV_UpdatePlayerQueuePositions(G_CanJoinGame, &player);
}

bool CMD_ForcespecCheck(const std::vector<std::string> &arguments,
						std::string &error, size_t &pid) {
	if (arguments.empty()) {
		error = "need a player id (try 'players').";
		return false;
	}

	std::istringstream buffer(arguments[0]);
	buffer >> pid;

	if (!buffer) {
		error = "player id needs to be a numeric value.";
		return false;
	}

	// Verify the player actually exists.
	player_t &player = idplayer(pid);
	if (!validplayer(player)) {
		std::ostringstream error_ss;
		error_ss << "could not find client " << pid << ".";
		error = error_ss.str();
		return false;
	}

	// Verify that the player is actually in a spectatable state.
	if (!player.ingame()) {
		std::ostringstream error_ss;
		error_ss << "cannot spectate client " << pid << ".";
		error = error_ss.str();
		return false;
	}

	// Verify that the player isn't already spectating.
	if (player.spectator) {
		std::ostringstream error_ss;
		error_ss << "client " << pid << " is already a spectator.";
		error = error_ss.str();
		return false;
	}

	return true;
}

BEGIN_COMMAND (forcespec) {
	std::vector<std::string> arguments = VectorArgs(argc, argv);
	std::string error;

	size_t pid;

	if (!CMD_ForcespecCheck(arguments, error, pid)) {
		Printf("forcespec: %s\n", error.c_str());
		return;
	}

	// Actually spec the given player
	player_t &player = idplayer(pid);
	SV_SetPlayerSpec(player, true);
} END_COMMAND (forcespec)

// Change the player's ready state and broadcast it to all connected players.
void SV_SetReady(player_t &player, bool setting, bool silent)
{
	if (!validplayer(player) || !player.ingame())
		return;

	// Change the player's ready state only if the new state is different from
	// the current state.
	bool changed = true;
	if (player.ready && !setting) {
		player.ready = false;
		if (!silent) {
			if (player.spectator)
				SV_PlayerPrintf(PRINT_HIGH, player.id, "You are no longer willing to play.\n");
			else
				SV_PlayerPrintf(PRINT_HIGH, player.id, "You are no longer ready to play.\n");
		}
	} else if (!player.ready && setting) {
		player.ready = true;
		if (!silent) {
			if (player.spectator)
				SV_PlayerPrintf(PRINT_HIGH, player.id, "You are now willing to play.\n");
			else
				SV_PlayerPrintf(PRINT_HIGH, player.id, "You are now ready to play.\n");
		}
	} else {
		changed = false;
	}

	if (changed) {
		// Broadcast the new ready state to all connected players.
		for (Players::iterator it = players.begin();it != players.end();++it)
			SVC_PlayerMembers(it->client.reliablebuf, player, SVC_PM_READY);
	}

	::levelstate.readyToggle();
}

/**
 * @brief Tell the client about any custom commands we have.
 * 
 * @detail A stock server is not expected to have any custom commands.
 *         Custom servers can implement their own features, and this is
 *         where you tell players about it.
 * 
 * @param player Player who asked for help.
 */
static void HelpCmd(player_t& player)
{
	SV_PlayerPrintf(PRINT_HIGH, player.id,
	                "odasrv v%s\n\n"
	                "This server has no custom commands\n",
	                GitShortHash());
}

/**
 * @brief Toggle a player as ready/unready.
 * 
 * @param player Player to toggle.
 */
static void ReadyCmd(player_t &player)
{
	// If the player is not ingame, he shouldn't be sending us ready packets.
	if (!player.ingame()) {
		return;
	}

	if (player.timeout_ready > level.time) {
		// We must be on a new map.  Reset the timeout.
		player.timeout_ready = 0;
	}

	// Check to see if warmup will allow us to toggle our ready state.
	if (!::G_CanReadyToggle())
	{
		SV_PlayerPrintf(PRINT_HIGH, player.id, "You can't ready in the middle of a match!\n");
		return;
	}

	// Check to see if the player's timeout has expired.
	if (player.timeout_ready > 0) {
		int timeout = level.time - player.timeout_ready;

		// Players can only toggle their ready state every 3 seconds.
		int timeout_check = 3 * TICRATE;
		int timeout_waitsec = 3 - (timeout / TICRATE);

		if (timeout < timeout_check) {
			SV_PlayerPrintf(PRINT_HIGH, player.id, "Please wait another %d second%s to change your ready state.\n",
			                timeout_waitsec, timeout_waitsec != 1 ? "s" : "");
			return;
		}
	}

	// Set the timeout.
	player.timeout_ready = level.time;

	// Toggle ready state
	SV_SetReady(player, !player.ready);
}

/**
 * @brief Send the player a MOTD on demand.
 * 
 * @param player Player who wants the MOTD.
 */
void MOTDCmd(player_t& player)
{
	SV_MidPrint((char*)sv_motd.cstring(), &player, 6);
}

/**
 * @brief Interpret a "netcmd" string from a client.
 * 
 * @param player Player who sent the netcmd.
 */
void SV_NetCmd(player_t& player)
{
	std::vector<std::string> netargs;

	// Parse arguments into a vector.
	netargs.push_back(MSG_ReadString());
	size_t netargc = MSG_ReadByte();

	for (size_t i = 0; i < netargc; i++)
	{
		netargs.push_back(MSG_ReadString());
	}

	if (netargs.at(0) == "help")
	{
		HelpCmd(player);
	}
	else if (netargs.at(0) == "motd")
	{
		MOTDCmd(player);
	}
	else if (netargs.at(0) == "ready")
	{
		ReadyCmd(player);
	}
	else if (netargs.at(0) == "vote")
	{
		SV_VoteCmd(player, netargs);
	}
}

//
// SV_RConLogout
//
//   Removes rcon privileges
//
void SV_RConLogout (player_t &player)
{
	client_t *cl = &player.client;

	// read and ignore the password field since rcon_logout doesn't use a password
	MSG_ReadString();

	if (cl->allow_rcon)
	{
		Printf("RCON logout from %s - %s", player.userinfo.netname.c_str(), NET_AdrToString(cl->address));
		cl->allow_rcon = false;
	}
}


//
// SV_RConPassword
// denis
//
void SV_RConPassword (player_t &player)
{
	client_t *cl = &player.client;

	std::string challenge = MSG_ReadString();
	std::string password = rcon_password.cstring();

	// Don't display login messages again if the client is already logged in
	if (cl->allow_rcon)
		return;

	if (!password.empty() && MD5SUM(password + cl->digest) == challenge)
	{
		cl->allow_rcon = true;
		Printf(PRINT_HIGH, "RCON login from %s - %s", player.userinfo.netname.c_str(), NET_AdrToString(cl->address));
	}
	else
	{
		Printf(PRINT_HIGH, "RCON login failure from %s - %s", player.userinfo.netname.c_str(), NET_AdrToString(cl->address));
		MSG_WriteMarker (&cl->reliablebuf, svc_print);
		MSG_WriteByte (&cl->reliablebuf, PRINT_HIGH);
		MSG_WriteString (&cl->reliablebuf, "Bad password\n");
	}
}

//
// SV_Suicide
//
void SV_Suicide(player_t &player)
{
	if (!player.mo)
		return;

	// WHY do you want to commit suicide in the intermission screen ?!?!
	if (gamestate != GS_LEVEL)
		return;

	// merry suicide!
	P_DamageMobj (player.mo, NULL, NULL, 10000, MOD_SUICIDE);
	//player.mo->player = NULL;
	//player.mo = NULL;
}

//
// SV_Cheat
//
void SV_Cheat(player_t &player)
{
	byte cheats = MSG_ReadByte();

	if(!sv_allowcheats)
		return;

	player.cheats = cheats;
}

void SV_CheatPulse(player_t &player)
{
    byte cheats = MSG_ReadByte();
    int i;

    if (!sv_allowcheats)
    {
        if (cheats == 3)
            MSG_ReadByte();

        return;
    }

    if (cheats == 1)
    {
        player.armorpoints = deh.FAArmor;
        player.armortype = deh.FAAC;

        weapontype_t pendweap = player.pendingweapon;

        for (i = 0; i<NUMWEAPONS; i++)
            P_GiveWeapon (&player, (weapontype_t)i, false);

        player.pendingweapon = pendweap;

        for (i=0; i<NUMAMMO; i++)
            player.ammo[i] = player.maxammo[i];

        return;
    }

    if (cheats == 2)
    {
        player.armorpoints = deh.KFAArmor;
        player.armortype = deh.KFAAC;

        weapontype_t pendweap = player.pendingweapon;

        for (i = 0; i<NUMWEAPONS; i++)
            P_GiveWeapon (&player, (weapontype_t)i, false);

        player.pendingweapon = pendweap;

        for (i=0; i<NUMAMMO; i++)
            player.ammo[i] = player.maxammo[i];

        for (i=0; i<NUMCARDS; i++)
            player.cards[i] = true;

        return;
    }

    if (cheats == 3)
    {
        byte power = MSG_ReadByte();

        if (!player.powers[power])
            P_GivePower(&player, power);
        else if (power != pw_strength)
            player.powers[power] = 1;
        else
            player.powers[power] = 0;

        return;
    }

    if (cheats == 4)
    {
        player.weaponowned[wp_chainsaw] = true;

        return;
    }
}

void SV_WantWad(player_t &player)
{
	client_t *cl = &player.client;

	if(!sv_waddownload)
	{
		// read and ignore the rest of the wad request
		MSG_ReadString();
		MSG_ReadString();
		MSG_ReadLong();

		MSG_WriteMarker(&cl->reliablebuf, svc_print);
		MSG_WriteByte(&cl->reliablebuf, PRINT_HIGH);
		MSG_WriteString(&cl->reliablebuf, "Server: Downloading is disabled\n");

		SV_DropClient(player);
		return;
	}

	if(player.ingame())
		SV_Suicide(player);

	std::string request = MSG_ReadString();
	std::string md5 = MSG_ReadString();
	size_t next_offset = MSG_ReadLong();

	std::string curr_request = D_CleanseFileName(cl->download.name);

	//DPrintf("pre-check client requesting {name: \"%s\", hash: \"%s\"} against {name: \"%s\", hash: \"%s\"}\n",
	//		request.c_str(), md5.c_str(),
	//		curr_request.c_str(), cl->download.md5.c_str()
	//);

	// [jsd] quick check for continuation of download:
	if (curr_request == request &&
		cl->download.md5 == md5)
	{
		cl->download.next_offset = next_offset;
		player.playerstate = PST_DOWNLOAD;

		return;
	}

	std::transform(md5.begin(), md5.end(), md5.begin(), toupper);

	//DPrintf("client requesting {name: \"%s\", hash: \"%s\"}\n", request.c_str(), md5.c_str());

	size_t i;
	std::string filename;
	for (i = 0; i < wadfiles.size(); i++)
	{
		filename = wadfiles[i].getBasename();
		if (filename == request && (md5.empty() || wadfiles[i].getHash() == md5))
			break;
	}

	if (i == wadfiles.size())
	{
		MSG_WriteMarker (&cl->reliablebuf, svc_print);
		MSG_WriteByte (&cl->reliablebuf, PRINT_HIGH);
		MSG_WriteString (&cl->reliablebuf, "Server: Bad wad request\n");
		SV_DropClient(player);
		return;
	}

	// denis - do not download commercial wads
	if (W_IsIWAD(wadfiles[i]))
	{
		MSG_WriteMarker (&cl->reliablebuf, svc_print);
		MSG_WriteByte (&cl->reliablebuf, PRINT_HIGH);
		std::string message;
		StrFormat(message, "Server: %s is a commercial wad and will not be downloaded\n",
		          wadfiles[i].getBasename().c_str());
		MSG_WriteString(&cl->reliablebuf, message.c_str());

		SV_DropClient(player);
		return;
	}

	if (player.playerstate != PST_DOWNLOAD ||
	    cl->download.name != wadfiles[i].getBasename())
		Printf("> client %d is downloading %s\n", player.id, filename.c_str());

	cl->download.name = wadfiles[i].getBasename();
	cl->download.md5 = md5;
	cl->download.next_offset = next_offset;
	player.playerstate = PST_DOWNLOAD;
}

//
// SV_ParseCommands
//

void SV_SendPlayerInfo(player_t &player);

void SV_ParseCommands(player_t &player)
{
	 while(validplayer(player))
	 {
		clc_t cmd = (clc_t)MSG_ReadByte();

		if(cmd == (clc_t)-1)
			break;

		switch(cmd)
		{
		case clc_disconnect:
			SV_DisconnectClient(player);
			return;

		case clc_userinfo:
			if (!SV_SetupUserInfo(player))
				return;
			SV_BroadcastUserInfo(player);
			break;

		case clc_getplayerinfo:
			SV_SendPlayerInfo (player);
			break;

		case clc_say:
			if (!SV_Say(player))
				return;
			break;

		case clc_privmsg:
			if (!SV_PrivMsg(player))
				return;
			break;

		case clc_move:
			SV_GetPlayerCmd(player);
			break;

		case clc_pingreply:  // [SL] 2011-05-11 - Changed to clc_pingreply
			SV_CalcPing(player);
			break;

		case clc_rate:
			MSG_ReadLong();		// [SL] Read and ignore. Clients now always use sv_maxrate.
			break;

		case clc_ack:
			SV_AcknowledgePacket(player);
			break;

		case clc_rcon:
			{
				std::string str(MSG_ReadString());
				StripColorCodes(str);

				if (player.client.allow_rcon)
				{
					Printf(PRINT_HIGH, "RCON command from %s - %s -> %s",
							player.userinfo.netname.c_str(), NET_AdrToString(net_from), str.c_str());
					AddCommandString(str);
				}
			}
			break;

		case clc_rcon_password:
			{
				bool login = MSG_ReadByte();

				if (login)
					SV_RConPassword(player);
				else
					SV_RConLogout(player);

				break;
			}

		case clc_changeteam:
			SV_ChangeTeam(player);
			break;

		case clc_spectate:
            {
                SV_Spectate (player);
            }
			break;

		case clc_netcmd:
			SV_NetCmd(player);
			break;

		case clc_kill:
			if(player.mo && player.suicidedelay == 0 && gamestate == GS_LEVEL &&
               (sv_allowcheats || sv_gametype == GM_COOP))
            {
				SV_Suicide (player);
            }
			break;

		case clc_launcher_challenge:
			MSG_ReadByte();
			MSG_ReadByte();
			MSG_ReadByte();
			SV_SendServerInfo();
			break;

		case clc_challenge:
			{
				size_t len = MSG_BytesLeft();
				MSG_ReadChunk(len);
			}
			break;

		case clc_wantwad:
			SV_WantWad(player);
			break;

		case clc_cheat:
			SV_Cheat(player);
			break;

        case clc_cheatpulse:
            SV_CheatPulse(player);
            break;

		case clc_abort:
			Printf("Client abort.\n");
			SV_DropClient(player);
			return;

		case clc_spy:
			SV_SpyPlayer(player);
			break;

		// [AM] Vote
		case clc_callvote:
			SV_Callvote(player);
			break;

		// [AM] Maplist
		case clc_maplist:
			SV_Maplist(player);
			break;
		case clc_maplist_update:
			SV_MaplistUpdate(player);
			break;

		default:
			Printf("SV_ParseCommands: Unknown client message %d.\n", (int)cmd);
			SV_DropClient(player);
			return;
		}

		if (net_message.overflowed)
		{
			Printf ("SV_ReadClientMessage: badread %d(%s)\n",
					    (int)cmd,
					    clc_info[cmd].getName());
			SV_DropClient(player);
			return;
		}
	 }
}

EXTERN_CVAR (sv_waddownloadcap)
EXTERN_CVAR (sv_download_test)


static void TimeCheck()
{
	G_TimeCheckEndGame();

	// [SL] 2011-10-25 - Send the clients the remaining time (measured in seconds)
	if (P_AtInterval(1 * TICRATE)) // every second
	{
		for (Players::iterator it = players.begin(); it != players.end(); ++it)
			SVC_LevelLocals(it->client.netbuf, level, SVC_LL_TIME);
	}
}

static void IntermissionTimeCheck()
{
	level.inttimeleft = mapchange / TICRATE;

	// [SL] 2011-10-25 - Send the clients the remaining time (measured in seconds)
	// [ML] 2012-2-1 - Copy it for intermission fun
	if (P_AtInterval(1 * TICRATE)) // every second
	{
		for (Players::iterator it = players.begin(); it != players.end(); ++it)
		{
			MSG_WriteMarker(&(it->client.netbuf), svc_inttimeleft);
			MSG_WriteShort(&(it->client.netbuf), level.inttimeleft);
		}
	}
}

//
// SV_GameTics
//
//	Runs once gametic (35 per second gametic)
//
void SV_GameTics (void)
{
	if (sv_gametype == GM_CTF)
		CTF_RunTics();

	switch (gamestate)
	{
		case GS_LEVEL:
			SV_RemoveCorpses();
			::levelstate.tic();
			TimeCheck();
			Vote_Runtic();
		break;
		case GS_INTERMISSION:
			IntermissionTimeCheck();
		break;

		default:
		break;
	}

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
		SV_ProcessPlayerCmd(*it);
}

void SV_TouchSpecial(AActor *special, player_t *player)
{
    client_t *cl = &player->client;

    if (cl == NULL || special == NULL)
        return;

    MSG_WriteMarker(&cl->reliablebuf, svc_touchspecial);
    MSG_WriteUnVarint(&cl->reliablebuf, special->netid);
}

void SV_PlayerTimes (void)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (it->ingame())
			(it->GameTime) += 1;
	}
}


//
// SV_Frozen
//
// Returns true if the game state should be frozen (not advance).
//
bool SV_Frozen()
{
	return sv_emptyfreeze && players.empty() && gamestate == GS_LEVEL;
}


//
// SV_StepTics
//
void SV_StepTics(QWORD count)
{
	DObject::BeginFrame();

	// run the newtime tics
	while (count--)
	{
		SV_GameTics();

		G_Ticker();

		SV_WriteCommands();
		SV_SendPackets();
		SV_ClearClientsBPS();
		SV_CheckTimeouts();
		SV_DestroyFinishedMovingSectors();

		// increment player_t::GameTime for all players once a second
		static int TicCount = 0;
		// Only do this once a second.
		if (TicCount++ >= 35)
		{
			SV_PlayerTimes();
			TicCount = 0;
		}

		gametic++;
	}

	DObject::EndFrame();
}

//
// SV_DisplayTics
//
// Nothing to display...
//
void SV_DisplayTics()
{
}

//
// SV_RunTics
//
// Checks for incoming packets, processes console usage, and calls SV_StepTics.
//
void SV_RunTics()
{
	SV_GetPackets();

	std::string cmd = I_ConsoleInput();
	if (cmd.length())
		AddCommandString(cmd);

	if (CON.is_open())
	{
		CON.clear();
		if (!CON.eof())
		{
			std::getline(CON, cmd);
			AddCommandString(cmd);
		}
	}

	SV_BanlistTics();
	SV_UpdateMaster();

	// only run game-related tickers if the server isn't frozen
	// (sv_emptyfreeze enabled and no clients)
	if (!step_mode && !SV_Frozen())
		SV_StepTics(1);

	// Remove any recently disconnected clients
	for (Players::iterator it = players.begin(); it != players.end();)
	{
		if (it->playerstate == PST_DISCONNECT)
			it = SV_RemoveDisconnectedPlayer(it);
		else
			++it;
	}

	// [SL] 2011-05-18 - Handle sv_emptyreset
	static size_t last_player_count = players.size();
	if (gamestate == GS_LEVEL && sv_emptyreset && players.empty() &&
			last_player_count > 0)
	{
		// The last player just disconnected so reset the level.
		// [SL] Ordinarily we should call G_DeferedInitNew but this is called
		// at the end of a gametic and the level reset should take place now
		// rather than at the start of the next gametic.
		maplist_entry_t lobby_entry;
		lobby_entry = Maplist::instance().get_lobbymap();

		if (!Maplist::instance().lobbyempty())
		{

			std::string wadstr;
			for (size_t i = 0; i < lobby_entry.wads.size(); i++)
			{
				if (i != 0)
				{
					wadstr += " ";
				}
				wadstr += C_QuoteString(lobby_entry.wads.at(i));
			}
			G_LoadWadString(wadstr, lobby_entry.map);
		}
		else
		{
			// [SL] create a copy of level.mapname because G_InitNew uses strncpy
			// to copy the mapname parameter to level.mapname, which is undefined
			// behavior.
			char mapname[9];
			strncpy(mapname, level.mapname, 8);
			mapname[8] = 0;

			G_InitNew(mapname);
		}
	}
	last_player_count = players.size();
}


BEGIN_COMMAND(step)
{
        QWORD newtics = argc > 1 ? atoi(argv[1]) : 1;

	extern unsigned char prndindex;

	SV_StepTics(newtics);

	// debugging output
	if (players.size() && players.begin() != players.end())
		Printf("level.time %d, prndindex %d, %d %d %d\n", level.time, prndindex, players.begin()->mo->x, players.begin()->mo->y, players.begin()->mo->z);
	else
		Printf("level.time %d, prndindex %d\n", level.time, prndindex);
}
END_COMMAND(step)


// For Debugging
BEGIN_COMMAND (playerinfo)
{
	player_t *player = &consoleplayer();

	if (argc > 1)
	{
		player_t &p = idplayer(atoi(argv[1]));

		if (!validplayer(p))
		{
			Printf (PRINT_HIGH, "Bad player number.\n");
			return;
		}
		else
			player = &p;
	}
	else
	{
		Printf("Usage : playerinfo <#playerid>\n");
		Printf("Gives additional infos about the selected player (use \"playerlist\" to display player IDs).\n");
		return;
	}

	if (!validplayer(*player))
	{
		Printf("Not a valid player\n");
		return;
	}

	char ip[16];
	sprintf(ip, "%d.%d.%d.%d",
			player->client.address.ip[0], player->client.address.ip[1],
			player->client.address.ip[2], player->client.address.ip[3]);

	char color[8];
	sprintf(color, "#%02X%02X%02X",
			player->userinfo.color[1], player->userinfo.color[2], player->userinfo.color[3]);

	const char* team = GetTeamInfo(player->userinfo.team)->ColorStringUpper.c_str();

	Printf("---------------[player info]----------- \n");
	Printf(" IP Address       - %s \n",		ip);
	Printf(" userinfo.netname - %s \n",		player->userinfo.netname.c_str());
	if (sv_gametype == GM_CTF || sv_gametype == GM_TEAMDM) {
		Printf(" userinfo.team    - %s \n", team);
	}
	Printf(" userinfo.aimdist - %d \n",		player->userinfo.aimdist >> FRACBITS);
	Printf(" userinfo.color   - %s \n",		color);
	Printf(" userinfo.gender  - %d \n",		player->userinfo.gender);
	Printf(" time             - %d \n",		player->GameTime);
	Printf(" spectator        - %d \n",		player->spectator);
	if (sv_gametype == GM_COOP)
	{
		Printf(" kills - %d  deaths - %d\n", player->killcount, player->deathcount);
	}
	else
	{
		Printf(" frags - %d  deaths - %d  points - %d\n", player->fragcount,
		       player->deathcount, player->points);
	}
	if (g_lives)
	{
		Printf(" lives - %d  wins - %d\n", player->lives, player->roundwins);
	}
	Printf("--------------------------------------- \n");
}
END_COMMAND (playerinfo)

BEGIN_COMMAND(playerlist)
{
	bool anybody = false;
	int frags = 0;
	int deaths = 0;
	int points = 0;

	for (Players::reverse_iterator it = players.rbegin(); it != players.rend(); ++it)
	{

		if (G_IsRoundsGame() && !G_IsDuelGame())
		{
			frags = it->totalpoints;
			deaths = it->totaldeaths;
			points = it->totalpoints;
		}
		else
		{
			frags = it->fragcount;
			deaths = it->deathcount;
			points = it->points;
		}

		std::string strMain, strScore;
		StrFormat(strMain, "(%02d): %s %s - %s - time:%d - ping:%d", it->id,
		          it->userinfo.netname.c_str(), it->spectator ? "(SPEC)" : "",
		          NET_AdrToString(it->client.address), it->GameTime, it->ping);

		if (sv_gametype == GM_COOP)
		{
			if (g_lives)
			{
				// Kills and Lives
				StrFormat(strScore, " - kills:%d - lives:%d", it->killcount, it->lives);
			}
			else
			{
				// Kills and Deaths
				StrFormat(strScore, " - kills:%d - deaths:%d", it->killcount,
				          it->deathcount);
			}
		}
		else if (sv_gametype == GM_DM)
		{
			if (g_lives)
			{
				// Wins, Lives, and Frags
				StrFormat(strScore, " - wins:%d - lives:%d - frags:%d", it->roundwins,
				          it->lives, frags);
			}
			else
			{
				// Frags, Deaths
				StrFormat(strScore, " - frags:%d - deaths:%d", frags, deaths);
			}
		}
		else if (sv_gametype == GM_TEAMDM)
		{
			if (g_lives)
			{
				// Frags and Lives
				StrFormat(strScore, " - frags:%d - lives:%d", frags, it->lives);
			}
			else
			{
				// Frags
				StrFormat(strScore, " - frags:%d", frags);
			}
		}
		else if (sv_gametype == GM_CTF)
		{
			if (g_lives)
			{
				// Points and Lives
				StrFormat(strScore, " - points:%d - lives:%d", points, it->lives);
			}
			else
			{
				// Points and Frags
				// Special case here: frags will only be from the current round, not global.
				StrFormat(strScore, " - points:%d - frags:%d", points, it->fragcount);
			}
		}

		Printf("%s%s\n", strMain.c_str(), strScore.c_str());
		anybody = true;
	}

	if (!anybody)
	{
		Printf("There are no players on the server.\n");
		return;
	}
}
END_COMMAND(playerlist)

BEGIN_COMMAND (players)
{
	AddCommandString("playerlist");
}
END_COMMAND (players)

void OnChangedSwitchTexture (line_t *line, int useAgain)
{
	int l = line - lines;
	unsigned state = 0, time = 0;
	P_GetButtonInfo(line, state, time);

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		MSG_WriteMarker(&cl->reliablebuf, svc_switch);
		MSG_WriteLong(&cl->reliablebuf, l);
		MSG_WriteByte(&cl->reliablebuf, line->switchactive);
		MSG_WriteByte(&cl->reliablebuf, line->special);
		MSG_WriteByte(&cl->reliablebuf, state);
		MSG_WriteShort(&cl->reliablebuf, P_GetButtonTexture(line));
		MSG_WriteLong(&cl->reliablebuf, time);
	}
}

void OnActivatedLine (line_t *line, AActor *mo, int side, LineActivationType activationType)
{
	if (P_LineSpecialMovesSector(line->special))
		return;

	int l = line - lines;

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		if (!(it->ingame()))
			continue;

		client_t *cl = &(it->client);

		MSG_WriteMarker (&cl->reliablebuf, svc_activateline);
		MSG_WriteLong (&cl->reliablebuf, l);
		MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
		MSG_WriteByte (&cl->reliablebuf, side);
		MSG_WriteByte (&cl->reliablebuf, activationType);
	}
}

// [RH]
// ClientObituary: Show a message when a player dies
//
void ClientObituary(AActor* self, AActor* inflictor, AActor* attacker)
{
	char gendermessage[1024];

	if (!self || !self->player)
		return;

	// Don't print obituaries after the end of a round
	if (!G_CanShowObituary() || gamestate != GS_LEVEL)
		return;

	int gender = self->player->userinfo.gender;

	// Treat voodoo dolls as unknown deaths
	if (inflictor && inflictor->player == self->player)
		MeansOfDeath = MOD_UNKNOWN;

	if (G_IsCoopGame())
		MeansOfDeath |= MOD_FRIENDLY_FIRE;

	if (G_IsTeamGame() &&
		attacker && attacker->player &&
		self->player->userinfo.team == attacker->player->userinfo.team)
		MeansOfDeath |= MOD_FRIENDLY_FIRE;

	bool friendly = MeansOfDeath & MOD_FRIENDLY_FIRE;
	int mod = MeansOfDeath & ~MOD_FRIENDLY_FIRE;
	const char* message = NULL;
	OString messagename;

	switch (mod)
	{
	case MOD_SUICIDE:
		messagename = OB_SUICIDE;
		break;
	case MOD_FALLING:
		messagename = OB_FALLING;
		break;
	case MOD_CRUSH:
		messagename = OB_CRUSH;
		break;
	case MOD_EXIT:
		messagename = OB_EXIT;
		break;
	case MOD_WATER:
		messagename = OB_WATER;
		break;
	case MOD_SLIME:
		messagename = OB_SLIME;
		break;
	case MOD_LAVA:
		messagename = OB_LAVA;
		break;
	case MOD_BARREL:
		messagename = OB_BARREL;
		break;
	case MOD_SPLASH:
		messagename = OB_SPLASH;
		break;
	}

	if (!messagename.empty())
		message = GStrings(messagename);

	if (attacker && message == NULL)
	{
		if (attacker == self)
		{
			switch (mod)
			{
			case MOD_R_SPLASH:
				messagename = OB_R_SPLASH;
				break;
			case MOD_ROCKET:
				messagename = OB_ROCKET;
				break;
			default:
				messagename = OB_KILLEDSELF;
				break;
			}
			message = GStrings(messagename);
		}
		else if (!attacker->player)
		{
			if (mod == MOD_HIT)
			{
				switch (attacker->type)
				{
					case MT_UNDEAD:
						messagename = OB_UNDEADHIT;
						break;
					case MT_TROOP:
						messagename = OB_IMPHIT;
						break;
					case MT_HEAD:
						messagename = OB_CACOHIT;
						break;
					case MT_SERGEANT:
						messagename = OB_DEMONHIT;
						break;
					case MT_SHADOWS:
						messagename = OB_SPECTREHIT;
						break;
					case MT_BRUISER:
						messagename = OB_BARONHIT;
						break;
					case MT_KNIGHT:
						messagename = OB_KNIGHTHIT;
						break;
					default:
						break;
				}
			}
			else
			{
				switch (attacker->type)
				{
				case MT_POSSESSED:
					messagename = OB_ZOMBIE;
					break;
				case MT_SHOTGUY:
					messagename = OB_SHOTGUY;
					break;
				case MT_VILE:
					messagename = OB_VILE;
					break;
				case MT_UNDEAD:
					messagename = OB_UNDEAD;
					break;
				case MT_FATSO:
					messagename = OB_FATSO;
					break;
				case MT_CHAINGUY:
					messagename = OB_CHAINGUY;
					break;
				case MT_SKULL:
					messagename = OB_SKULL;
					break;
				case MT_TROOP:
					messagename = OB_IMP;
					break;
				case MT_HEAD:
					messagename = OB_CACO;
					break;
				case MT_BRUISER:
					messagename = OB_BARON;
					break;
				case MT_KNIGHT:
					messagename = OB_KNIGHT;
					break;
				case MT_SPIDER:
					messagename = OB_SPIDER;
					break;
				case MT_BABY:
					messagename = OB_BABY;
					break;
				case MT_CYBORG:
					messagename = OB_CYBORG;
					break;
				case MT_WOLFSS:
					messagename = OB_WOLFSS;
					break;
				default:
					break;
				}
			}

			if (!messagename.empty())
				message = GStrings(messagename);
		}
	}

	if (message)
	{
		SexMessage(message, gendermessage, gender,
				self->player->userinfo.netname.c_str(), self->player->userinfo.netname.c_str());
		SV_BroadcastPrintf(PRINT_OBITUARY, "%s\n", gendermessage);
		return;
	}

	if (attacker && attacker->player)
	{
		if (friendly)
		{
			gender = attacker->player->userinfo.gender;
			messagename = GStrings.getIndex(GStrings.toIndex(OB_FRIENDLY1) + (P_Random() & 3));
			message = messagename.c_str();
		}
		else
		{
			switch (mod)
			{
			case MOD_FIST:
				messagename = OB_MPFIST;
				break;
			case MOD_CHAINSAW:
				messagename = OB_MPCHAINSAW;
				break;
			case MOD_PISTOL:
				messagename = OB_MPPISTOL;
				break;
			case MOD_SHOTGUN:
				messagename = OB_MPSHOTGUN;
				break;
			case MOD_SSHOTGUN:
				messagename = OB_MPSSHOTGUN;
				break;
			case MOD_CHAINGUN:
				messagename = OB_MPCHAINGUN;
				break;
			case MOD_ROCKET:
				messagename = OB_MPROCKET;
				break;
			case MOD_R_SPLASH:
				messagename = OB_MPR_SPLASH;
				break;
			case MOD_PLASMARIFLE:
				messagename = OB_MPPLASMARIFLE;
				break;
			case MOD_BFG_BOOM:
				messagename = OB_MPBFG_BOOM;
				break;
			case MOD_BFG_SPLASH:
				messagename = OB_MPBFG_SPLASH;
				break;
			case MOD_TELEFRAG:
				messagename = OB_MPTELEFRAG;
				break;
			case MOD_RAILGUN:
				messagename = OB_RAILGUN;
				break;
			}

			if (!messagename.empty())
				message = GStrings(messagename);
		}


	}

	if (message && attacker && attacker->player)
	{
		SexMessage(message, gendermessage, gender,
				self->player->userinfo.netname.c_str(), attacker->player->userinfo.netname.c_str());
		SV_BroadcastPrintf(PRINT_OBITUARY, "%s\n", gendermessage);
		return;
	}

	SexMessage(GStrings(OB_DEFAULT), gendermessage, gender,
			self->player->userinfo.netname.c_str(), self->player->userinfo.netname.c_str());
	SV_BroadcastPrintf(PRINT_OBITUARY, "%s\n", gendermessage);
}

void SV_SendDamagePlayer(player_t *player, int healthDamage, int armorDamage)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		MSG_WriteMarker(&cl->reliablebuf, svc_damageplayer);
		MSG_WriteUnVarint(&cl->reliablebuf, player->mo->netid);
		MSG_WriteShort(&cl->reliablebuf, healthDamage);
		MSG_WriteByte(&cl->reliablebuf, armorDamage);
		if (player->client.packedversion > MAKEVER(0, 9, 2))
		{
			MSG_WriteVarint(&cl->reliablebuf, player->health);
			MSG_WriteVarint(&cl->reliablebuf, player->armorpoints);
		}
	}
}

void SV_SendDamageMobj(AActor *target, int pain)
{
	if (!target)
		return;

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		MSG_WriteMarker(&cl->reliablebuf, svc_damagemobj);
		MSG_WriteUnVarint(&cl->reliablebuf, target->netid);
		MSG_WriteShort(&cl->reliablebuf, target->health);
		MSG_WriteByte(&cl->reliablebuf, pain);

		MSG_WriteMarker (&cl->netbuf, svc_movemobj);
		MSG_WriteUnVarint(&cl->netbuf, target->netid);
		MSG_WriteByte (&cl->netbuf, target->rndindex);
		MSG_WriteLong (&cl->netbuf, target->x);
		MSG_WriteLong (&cl->netbuf, target->y);
		MSG_WriteLong (&cl->netbuf, target->z);

		MSG_WriteMarker (&cl->netbuf, svc_mobjspeedangle);
		MSG_WriteUnVarint(&cl->netbuf, target->netid);
		MSG_WriteLong (&cl->netbuf, target->angle);
		MSG_WriteLong (&cl->netbuf, target->momx);
		MSG_WriteLong (&cl->netbuf, target->momy);
		MSG_WriteLong (&cl->netbuf, target->momz);
	}
}

void SV_SendKillMobj(AActor *source, AActor *target, AActor *inflictor,
				     bool joinkill)
{
	if (!target)
		return;

	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		if (!SV_IsPlayerAllowedToSee(*it, target))
			continue;

		// send death location first
		MSG_WriteMarker(&cl->reliablebuf, svc_movemobj);
		MSG_WriteUnVarint(&cl->reliablebuf, target->netid);
		MSG_WriteByte(&cl->reliablebuf, target->rndindex);

		// [SL] 2012-12-26 - Get real position since this actor is at
		// a reconciled position with sv_unlag 1
		fixed_t xoffs = 0, yoffs = 0, zoffs = 0;
		if (target->player)
		{
			Unlag::getInstance().getReconciliationOffset(
					target->player->id, xoffs, yoffs, zoffs);
		}

		MSG_WriteLong(&cl->reliablebuf, target->x + xoffs);
		MSG_WriteLong(&cl->reliablebuf, target->y + yoffs);
		MSG_WriteLong(&cl->reliablebuf, target->z + zoffs);

		MSG_WriteMarker (&cl->reliablebuf, svc_mobjspeedangle);
		MSG_WriteUnVarint(&cl->reliablebuf, target->netid);
		MSG_WriteLong (&cl->reliablebuf, target->angle);
		MSG_WriteLong (&cl->reliablebuf, target->momx);
		MSG_WriteLong (&cl->reliablebuf, target->momy);
		MSG_WriteLong (&cl->reliablebuf, target->momz);

		SVC_KillMobj(cl->reliablebuf, source, target, inflictor, ::MeansOfDeath, joinkill);
	}
}

// Tells clients to remove an actor from the world as it doesn't exist anymore
void SV_SendDestroyActor(AActor *mo)
{
	if (mo->netid && mo->type != MT_PUFF)
	{
		for (Players::iterator it = players.begin();it != players.end();++it)
		{
			if (mo->players_aware.get(it->id))
			{
				client_t *cl = &(it->client);

            	// denis - todo - need a queue for destroyed (lost awareness)
            	// objects, as a flood of destroyed things could easily overflow a
            	// buffer
				MSG_WriteMarker(&cl->reliablebuf, svc_removemobj);
				MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
			}
		}
	}
}

// Missile exploded so tell clients about it
void SV_ExplodeMissile(AActor *mo)
{
	for (Players::iterator it = players.begin();it != players.end();++it)
	{
		client_t *cl = &(it->client);

		if (!SV_IsPlayerAllowedToSee(*it, mo))
			continue;

		MSG_WriteMarker (&cl->reliablebuf, svc_movemobj);
		MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
		MSG_WriteByte (&cl->reliablebuf, mo->rndindex);
		MSG_WriteLong (&cl->reliablebuf, mo->x);
		MSG_WriteLong (&cl->reliablebuf, mo->y);
		MSG_WriteLong (&cl->reliablebuf, mo->z);

		MSG_WriteMarker(&cl->reliablebuf, svc_explodemissile);
		MSG_WriteUnVarint(&cl->reliablebuf, mo->netid);
	}
}

//
// SV_SendPlayerInfo
//
// Sends a player their current inventory
//
void SV_SendPlayerInfo(player_t &player)
{
	client_t *cl = &player.client;
	SVC_PlayerInfo(cl->reliablebuf, player);
}

//
// SV_PreservePlayer
//
void SV_PreservePlayer(player_t &player)
{
	if (!serverside || sv_gametype != GM_COOP || !validplayer(player) || !player.ingame())
		return;

	if(!unnatural_level_progression)
		player.playerstate = PST_LIVE; // denis - carry weapons and keys over to next level

	G_DoReborn(player);

	SV_SendPlayerInfo(player);
}

void SV_AddPlayerToQueue(player_t* player)
{
	player->QueuePosition = 255;
	SV_UpdatePlayerQueuePositions(G_CanJoinGame, NULL);
}

void SV_RemovePlayerFromQueue(player_t* player)
{
	player->joindelay = ReJoinDelay;
	SV_UpdatePlayerQueuePositions(G_CanJoinGame, player);
}

void SV_UpdatePlayerQueueLevelChange(const WinInfo& win)
{
	if (::g_winnerstays)
	{
		int queuedPlayerCount = 0;
		std::vector<player_t*> loserPlayers;

		PlayerResults pr = PlayerQuery().execute();
		for (PlayersView::iterator it = pr.players.begin(); it != pr.players.end(); ++it)
		{
			switch (win.type)
			{
			case WinInfo::WIN_PLAYER:
				// Boot everybody but the winner.
				if ((*it)->id != win.id)
					loserPlayers.push_back(*it);
				break;
			case WinInfo::WIN_TEAM:
				// Boot everybody except the winning team.
				if ((*it)->userinfo.team != win.id)
					loserPlayers.push_back(*it);
				break;
			case WinInfo::WIN_DRAW:
			case WinInfo::WIN_NOBODY:
				// Draws are just another way of saying there were no winners.
				loserPlayers.push_back(*it);
				break;
			default:
				// Everyone won, or something strange happened.
				break;
			}

			// NOBODY/UNKNOWN should default to never touching the queue.
		}

		std::vector<std::string> names;
		for (PlayersView::iterator it = loserPlayers.begin(); it != loserPlayers.end();
		     ++it)
		{
			SV_SetPlayerSpec(**it, true, true);
			names.push_back((*it)->userinfo.netname);

			// Allow this player to queue up immediately without waiting for
			// ReJoinDelay
			(*it)->joindelay = 0;
		}

		if (names.size() > 2)
		{
			names.back() = std::string("and ") + names.back();
			SV_BroadcastPrintf("%s lost the last game and were forced to spectate.\n",
			                   JoinStrings(names, ", ").c_str());
		}
		else if (names.size() == 2)
		{
			SV_BroadcastPrintf(
			    "%s and %s lost the last game and were forced to spectate.\n",
			    names.at(0).c_str(), names.at(1).c_str());
		}
		else if (names.size() == 1)
		{
			SV_BroadcastPrintf("%s lost the last game and was forced to spectate.\n",
			                   names.at(0).c_str());
		}
	}

	SV_UpdatePlayerQueuePositions(G_CanJoinGameStart, NULL);
}

void SV_UpdatePlayerQueuePositions(JoinTest joinTest, player_t* disconnectPlayer)
{
	int playerCount = 0;
	int queuePos = 1;
	PlayersView queued;
	PlayersView queueUpdates;

	for (Players::iterator it = ::players.begin(); it != ::players.end(); ++it)
	{
		if (it->QueuePosition > 0 && disconnectPlayer != &(*it))
			queued.push_back(&(*it));

		if (!it->spectator && it->ingame())
			playerCount++;
	}

	std::sort(queued.begin(), queued.end(), CompareQueuePosition);

	for (size_t i = 0; i < queued.size(); i++)
	{
		player_t* p = queued.at(i);

		if (p->QueuePosition == 0)
			continue;

		if (joinTest() == JOIN_OK)
		{
			p->QueuePosition = 0;
			SV_JoinPlayer(*p, false);
			queueUpdates.push_back(p);
			playerCount++;
		}
		else
		{
			if (p->QueuePosition != queuePos)
				queueUpdates.push_back(p);
			p->QueuePosition = queuePos++;
		}
	}

	if (disconnectPlayer && disconnectPlayer->QueuePosition > 0)
	{
		disconnectPlayer->QueuePosition = 0;
		queueUpdates.push_back(disconnectPlayer);
	}

	for (Players::iterator dest = ::players.begin(); dest != ::players.end(); ++dest)
	{
		for (PlayersView::iterator it = queueUpdates.begin(); it != queueUpdates.end();
		     ++it)
		{
			SV_SendPlayerQueuePosition(*it, &(*dest));
		}
	}
}

void SV_SendPlayerQueuePositions(player_t* dest, bool initConnect)
{
	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		if (initConnect && it->QueuePosition == 0)
			continue;
		SV_SendPlayerQueuePosition(&(*it), dest);
	}
}

void SV_SendPlayerQueuePosition(player_t* source, player_t* dest)
{
	MSG_WriteMarker(&(dest->client.reliablebuf), svc_playerqueuepos);
	MSG_WriteByte(&(dest->client.reliablebuf), source->id);
	MSG_WriteByte(&(dest->client.reliablebuf), source->QueuePosition);
}

bool CompareQueuePosition(const player_t* p1, const player_t* p2)
{
	return p1->QueuePosition < p2->QueuePosition;
}

void SV_ClearPlayerQueue()
{
	for (Players::iterator it = players.begin(); it != players.end(); ++it)
		it->QueuePosition = 0;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
		SV_SendPlayerQueuePositions(&(*it), false);
}

void SV_SendExecuteLineSpecial(byte special, line_t* line, AActor* activator, int arg0,
                               int arg1, int arg2, int arg3, int arg4)
{
	if (P_LineSpecialMovesSector(special))
		return;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		if (!(it->ingame()))
			continue;

		client_t* cl = &it->client;

		MSG_WriteMarker(&cl->reliablebuf, svc_executelinespecial);
		MSG_WriteByte(&cl->reliablebuf, special);
		if (line)
			MSG_WriteShort(&cl->reliablebuf, line - lines);
		else
			MSG_WriteShort(&cl->reliablebuf, 0xFFFF);
		MSG_WriteUnVarint(&cl->reliablebuf, activator ? activator->netid : 0);
		MSG_WriteVarint(&cl->reliablebuf, arg0);
		MSG_WriteVarint(&cl->reliablebuf, arg1);
		MSG_WriteVarint(&cl->reliablebuf, arg2);
		MSG_WriteVarint(&cl->reliablebuf, arg3);
		MSG_WriteVarint(&cl->reliablebuf, arg4);
	}
}

//
// If playerOnly is true and the activator is a player, then it will only be
// sent to the activating player.
//
// [AM] FIXME: Use vargs or a vector or even a ptr/length instead of....this.
//
void SV_ACSExecuteSpecial(byte special, AActor* activator, const char* print,
                          bool playerOnly, int arg0, int arg1, int arg2, int arg3,
                          int arg4, int arg5, int arg6, int arg7, int arg8)
{
	byte argc = 0;
	player_s* sendPlayer = NULL;
	if (playerOnly && activator != NULL && activator->player != NULL)
		sendPlayer = activator->player;

	if (arg0 != -1)
		argc += 1;
	if (arg1 != -1)
		argc += 1;
	if (arg2 != -1)
		argc += 1;
	if (arg3 != -1)
		argc += 1;
	if (arg4 != -1)
		argc += 1;
	if (arg5 != -1)
		argc += 1;
	if (arg6 != -1)
		argc += 1;
	if (arg7 != -1)
		argc += 1;
	if (arg8 != -1)
		argc += 1;

	for (Players::iterator it = players.begin(); it != players.end(); ++it)
	{
		if (!(it->ingame()) || (sendPlayer != NULL && sendPlayer != &(*it)))
			continue;

		client_t* cl = &it->client;

		MSG_WriteMarker(&cl->reliablebuf, svc_executeacsspecial);
		MSG_WriteByte(&cl->reliablebuf, special);
		MSG_WriteUnVarint(&cl->reliablebuf, activator ? activator->netid : 0);

		MSG_WriteByte(&cl->reliablebuf, argc);
		if (arg0 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg0);
		if (arg1 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg1);
		if (arg2 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg2);
		if (arg3 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg3);
		if (arg4 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg4);
		if (arg5 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg5);
		if (arg6 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg6);
		if (arg7 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg7);
		if (arg8 != -1)
			MSG_WriteVarint(&cl->reliablebuf, arg8);

		if (print)
			MSG_WriteString(&cl->reliablebuf, print);
		else
			MSG_WriteString(&cl->reliablebuf, "");
	}
}

void SV_UpdateShareKeys(player_t& player)
{
	// Player needs to be valid.
	if (!validplayer(player))
		return;

	// Disallow to spectators
	if (player.spectator)
		return;

	// Don't send to dead players... Yet, since they'll get it upon respawning
	if (player.health <= 0)
		return;

	// Update their keys informations
	for (int i = 0; i < NUMCARDS; i++) {
		player.cards[i] = keysfound[i];
	}

	// Refresh that new data to the client
	SV_SendPlayerInfo(player);
}

void SV_ShareKeys(card_t card, player_t &player)
{
	// Add it to the KeysCheck array
	keysfound[card] = true;
	char* coloritem;

	// If the server hasn't accepted to share keys yet, stop it.
	if (!sv_sharekeys)
		return;

	// Broadcast the key shared to 
	gitem_t* item;
	if (item = FindCardItem(card))
	{
		switch (card)
		{
		case it_bluecard:
		case it_blueskull:
			coloritem = TEXTCOLOR_BLUE; 
			break;
		case it_redcard:
		case it_redskull:
			coloritem = TEXTCOLOR_RED;
			break;
		case it_yellowcard:
		case it_yellowskull:
			coloritem = TEXTCOLOR_GOLD;
			break;
		default:
			coloritem = TEXTCOLOR_NORMAL;
		}

		SV_BroadcastPrintf("%s found the %s%s%s!\n", player.userinfo.netname.c_str(),
		                   coloritem, item->pickup_name, TEXTCOLOR_NORMAL);
	}
	else
		SV_BroadcastPrintf("%s found a key!\n", player.userinfo.netname.c_str());

	// Refresh the inventory to everyone
	// ToDo: If we're the player who picked it, don't refresh our own inventory
	for (Players::iterator it = players.begin(); it != players.end(); ++it) {
		SV_UpdateShareKeys(*it);
	}
}

VERSION_CONTROL (sv_main_cpp, "$Id$")

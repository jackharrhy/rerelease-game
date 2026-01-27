// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// NOTE(notscared) Server portal - teleports player to another game server

#include "g_local.h"

/*
 * Helper function to stuff a console command to a specific client.
 * This sends the command string to the client's console buffer.
 */
static void stuffcmd(edict_t *ent, const char *cmd)
{
	gi.WriteByte(svc_stufftext);
	gi.WriteString(cmd);
	gi.unicast(ent, true);
}

/*QUAKED trigger_server_portal (.5 .5 .5) ?
Portal to another server. When touched by a player, the client
will disconnect from the current server and connect to the target server.

Keys:
server_address: IP:Port of the target server (e.g. "192.168.1.50:27910")
message: Optional message shown to player before transfer
wait: Cooldown in seconds before the same player can use the portal again (default 2)
*/

TOUCH(trigger_server_portal_touch) (edict_t *self, edict_t *other, const trace_t &tr, bool other_touching_self) -> void
{
	// Only players can use server portals
	if (!other->client)
		return;

	// Check if server_address is set
	if (!self->server_address || !*self->server_address)
	{
		gi.Com_Print("trigger_server_portal: no server_address set!\n");
		return;
	}

	// Debounce - prevent spam
	if (level.time < other->touch_debounce_time)
		return;
	
	// Set debounce time (default 2 seconds if wait is not set)
	float wait_time = self->wait ? self->wait : 2.0f;
	other->touch_debounce_time = level.time + gtime_t::from_sec(wait_time);

	// Show message if set
	if (self->message && *self->message)
		gi.Center_Print(other, self->message);

	// Log the transfer
	gi.Com_Print(G_Fmt("Player {} connecting to server {}\n", 
		other->client->pers.netname, self->server_address).data());

	// Send connect command to this client only
	stuffcmd(other, G_Fmt("connect {}\n", self->server_address).data());
}

void SP_trigger_server_portal(edict_t *self)
{
	// Default wait time of 2 seconds
	if (!self->wait)
		self->wait = 2.0f;

	// Validate server_address
	if (!self->server_address || !*self->server_address)
	{
		gi.Com_Print("trigger_server_portal without server_address\n");
		G_FreeEdict(self);
		return;
	}

	self->touch = trigger_server_portal_touch;
	self->solid = SOLID_TRIGGER;
	self->movetype = MOVETYPE_NONE;

	gi.setmodel(self, self->model);
	gi.linkentity(self);
}

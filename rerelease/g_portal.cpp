// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// NOTE(notscared) Server portal - transfers player to another backend server via proxy

#include "g_local.h"

/*QUAKED trigger_server_portal (.5 .5 .5) ?
Portal to another backend server. When touched by a player, the proxy
will transfer them to the target server seamlessly.

Keys:
target_server: Name of the backend server (e.g. "hub", "test1")
message: Optional message shown to player before transfer
wait: Cooldown in seconds before the same player can use the portal again (default 2)
*/

TOUCH(trigger_server_portal_touch) (edict_t *self, edict_t *other, const trace_t &tr, bool other_touching_self) -> void
{
	// Only players can use server portals
	if (!other->client)
		return;

	// Check if target_server is set
	if (!self->target_server || !*self->target_server)
	{
		gi.Com_Print("trigger_server_portal: no target_server set!\n");
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
	gi.Com_Print(G_Fmt("Player {} transferring to server {}\n", 
		other->client->pers.netname, self->target_server).data());

	// Request transfer via proxy
	gi.RequestTransfer(other, self->target_server);
}

void SP_trigger_server_portal(edict_t *self)
{
	// Default wait time of 2 seconds
	if (!self->wait)
		self->wait = 2.0f;

	// Validate target_server
	if (!self->target_server || !*self->target_server)
	{
		gi.Com_Print("trigger_server_portal without target_server\n");
		G_FreeEdict(self);
		return;
	}

	self->touch = trigger_server_portal_touch;
	self->solid = SOLID_TRIGGER;
	self->movetype = MOVETYPE_NONE;

	gi.setmodel(self, self->model);
	gi.linkentity(self);
}

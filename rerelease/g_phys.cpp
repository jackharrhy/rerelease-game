// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.
// g_phys.c

#include "g_local.h"

/*


pushmove objects do not obey gravity, and do not interact with each other or trigger fields, but block normal movement
and push normal objects when they move.

onground is set for toss objects when they come to a complete rest.  it is set for steping or walking objects

doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
corpses are SOLID_NOT and MOVETYPE_TOSS
crates are SOLID_BBOX and MOVETYPE_TOSS
walking monsters are SOLID_SLIDEBOX and MOVETYPE_STEP
flying/floating monsters are SOLID_SLIDEBOX and MOVETYPE_FLY

solid_edge items only clip against bsp models.

*/

void SV_Physics_NewToss(edict_t *ent); // PGM

// [Paril-KEX] fetch the clipmask for this entity; certain modifiers
// affect the clipping behavior of objects.
contents_t G_GetClipMask(edict_t *ent)
{
	contents_t mask = ent->clipmask;

	// default masks
	if (!mask)
	{
		if (ent->svflags & SVF_MONSTER)
			mask = MASK_MONSTERSOLID;
		else if (ent->svflags & SVF_PROJECTILE)
			mask = MASK_PROJECTILE;
		else
			mask = MASK_SHOT & ~CONTENTS_DEADMONSTER;
	}
	
	// non-solid objects (items, etc) shouldn't try to clip
	// against players/monsters
	if (ent->solid == SOLID_NOT || ent->solid == SOLID_TRIGGER)
		mask &= ~(CONTENTS_MONSTER | CONTENTS_PLAYER);

	// monsters/players that are also dead shouldn't clip
	// against players/monsters
	if ((ent->svflags & (SVF_MONSTER | SVF_PLAYER)) && (ent->svflags & SVF_DEADMONSTER))
		mask &= ~(CONTENTS_MONSTER | CONTENTS_PLAYER);

	return mask;
}

/*
============
SV_TestEntityPosition

============
*/
edict_t *SV_TestEntityPosition(edict_t *ent)
{
	trace_t	   trace;

	trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, ent->s.origin, ent, G_GetClipMask(ent));

	if (trace.startsolid)
		return g_edicts;

	return nullptr;
}

/*
================
SV_CheckVelocity
================
*/
void SV_CheckVelocity(edict_t *ent)
{
	//
	// bound velocity
	//
	float speed = ent->velocity.length();

	if (speed > sv_maxvelocity->value)
		ent->velocity = (ent->velocity / speed) * sv_maxvelocity->value;
}

/*
=============
SV_RunThink

Runs thinking code for this frame if necessary
=============
*/
bool SV_RunThink(edict_t *ent)
{
	gtime_t thinktime = ent->nextthink;
	if (thinktime <= 0_ms)
		return true;
	if (thinktime > level.time)
		return true;

	ent->nextthink = 0_ms;
	if (!ent->think)
		gi.Com_Error("nullptr ent->think");
	ent->think(ent);

	return false;
}

/*
==================
G_Impact

Two entities have touched, so run their touch functions
==================
*/
void G_Impact(edict_t *e1, const trace_t &trace)
{
	edict_t *e2 = trace.ent;

	if (e1->touch && (e1->solid != SOLID_NOT || (e1->flags & FL_ALWAYS_TOUCH)))
		e1->touch(e1, e2, trace, false);

	if (e2->touch && (e2->solid != SOLID_NOT || (e2->flags & FL_ALWAYS_TOUCH)))
		e2->touch(e2, e1, trace, true);
}

/*
============
SV_FlyMove

The basic solid body movement clip that slides along multiple planes
============
*/
void SV_FlyMove(edict_t *ent, float time, contents_t mask)
{
	ent->groundentity = nullptr;

	touch_list_t touch;
	PM_StepSlideMove_Generic(ent->s.origin, ent->velocity, time, ent->mins, ent->maxs, touch, false, [&](const vec3_t &start, const vec3_t &mins, const vec3_t &maxs, const vec3_t &end)
	{
		return gi.trace(start, mins, maxs, end, ent, mask);
	});

	for (size_t i = 0; i < touch.num; i++)
	{
		auto &trace = touch.traces[i];

		if (trace.plane.normal[2] > 0.7f)
		{
			ent->groundentity = trace.ent;
			ent->groundentity_linkcount = trace.ent->linkcount;
		}

		//
		// run the impact function
		//
		G_Impact(ent, trace);

		// impact func requested velocity kill
		if (ent->flags & FL_KILL_VELOCITY)
		{
			ent->flags &= ~FL_KILL_VELOCITY;
			ent->velocity = {};
		}
	}
}

/*
============
SV_AddGravity

============
*/
void SV_AddGravity(edict_t *ent)
{
	ent->velocity += ent->gravityVector * (ent->gravity * level.gravity * gi.frame_time_s);
}

/*
===============================================================================

PUSHMOVE

===============================================================================
*/

/*
============
SV_PushEntity

Does not change the entities velocity at all
============
*/
trace_t SV_PushEntity(edict_t *ent, const vec3_t &push)
{
	vec3_t start = ent->s.origin;
	vec3_t end = start + push;

	trace_t trace = gi.trace(start, ent->mins, ent->maxs, end, ent, G_GetClipMask(ent));
	
	ent->s.origin = trace.endpos + (trace.plane.normal * .5f);
	gi.linkentity(ent);

	if (trace.fraction != 1.0f || trace.startsolid)
	{
		G_Impact(ent, trace);

		// if the pushed entity went away and the pusher is still there
		if (!trace.ent->inuse && ent->inuse)
		{
			// move the pusher back and try again
			ent->s.origin = start;
			gi.linkentity(ent);
			return SV_PushEntity(ent, push);
		}
	}

	// ================
	// PGM
	// FIXME - is this needed?
	ent->gravity = 1.0;
	// PGM
	// ================

	if (ent->inuse)
		G_TouchTriggers(ent);

	return trace;
}

struct pushed_t
{
	edict_t *ent;
	vec3_t	 origin;
	vec3_t	 angles;
	bool	 rotated;
	float	 yaw;
};

pushed_t pushed[MAX_EDICTS], *pushed_p;

edict_t *obstacle;

/*
============
SV_PushEntities

Moves entities that are on or inside a pusher that has already moved.
Assumes the pusher is already in its final position.
Returns true if all entities moved successfully, false if blocked.
Sets obstacle global if blocked.
============
*/
static bool SV_PushEntities(edict_t *pusher, const vec3_t &move, const vec3_t &amove)
{
	edict_t *check, *block = nullptr;
	vec3_t mins, maxs;
	pushed_t *p;
	vec3_t org, org2, move2, forward, right, up;

	// find the bounding box (pusher is already in final position)
	mins = pusher->absmin;
	maxs = pusher->absmax;

	// we need this for pushing things later
	org = -amove;
	AngleVectors(org, forward, right, up);

	// see if any solid entities are inside the final position
	check = g_edicts + 1;
	for (uint32_t e = 1; e < globals.num_edicts; e++, check++)
	{
		if (!check->inuse)
			continue;
		if (check->movetype == MOVETYPE_PUSH || check->movetype == MOVETYPE_STOP || check->movetype == MOVETYPE_NONE ||
			check->movetype == MOVETYPE_NOCLIP)
			continue;

		if (!check->linked)
			continue; // not linked in anywhere

		// Skip the pusher itself and the driver (driver is handled separately in vehicle_think)
		if (check == pusher || check == pusher->owner)
			continue;

		// if the entity is standing on the pusher, it will definitely be moved
		if (check->groundentity != pusher)
		{
			// see if the ent needs to be tested
			if (check->absmin[0] >= maxs[0] || check->absmin[1] >= maxs[1] || check->absmin[2] >= maxs[2] ||
				check->absmax[0] <= mins[0] || check->absmax[1] <= mins[1] || check->absmax[2] <= mins[2])
				continue;

			// see if the ent's bbox is inside the pusher's final position
			if (!SV_TestEntityPosition(check))
				continue;
		}

		// Move entities if:
		// 1. Pusher is MOVETYPE_PUSH (trains, platforms) - move all entities in bbox
		// 2. Pusher is MOVETYPE_VEHICLE - move all entities in bbox (vehicles)
		// 3. Entity has pusher as groundentity (standing on it)
		if ((pusher->movetype == MOVETYPE_PUSH) || (pusher->movetype == MOVETYPE_VEHICLE) || (check->groundentity == pusher))
		{
			// move this entity
			pushed_p->ent = check;
			pushed_p->origin = check->s.origin;
			pushed_p->angles = check->s.angles;
			pushed_p->rotated = !!amove[YAW];
			if (pushed_p->rotated)
				pushed_p->yaw =
					pusher->client ? (float) pusher->client->ps.pmove.delta_angles[YAW] : pusher->s.angles[YAW];
			pushed_p++;

			vec3_t old_position = check->s.origin;

			// try moving the contacted entity
			check->s.origin += move;
			if (check->client)
			{
				// Paril: disabled because in vanilla delta_angles are never
				// lerped. delta_angles can probably be lerped as long as event
				// isn't EV_PLAYER_TELEPORT or a new RDF flag is set
				// check->client->ps.pmove.delta_angles[YAW] += amove[YAW];
			}
			else
				check->s.angles[YAW] += amove[YAW];

			// figure movement due to the pusher's amove
			org = check->s.origin - pusher->s.origin;
			org2[0] = org.dot(forward);
			org2[1] = -(org.dot(right));
			org2[2] = org.dot(up);
			move2 = org2 - org;
			check->s.origin += move2;

			// may have pushed them off an edge
			if (check->groundentity != pusher)
				check->groundentity = nullptr;

			block = SV_TestEntityPosition(check);

			// [Paril-KEX] this is a bit of a hack; allow dead player skulls
			// to be a blocker because otherwise elevators/doors get stuck
			if (block && check->client && !check->takedamage)
			{
				check->s.origin = old_position;
				block = nullptr;
			}

			if (!block)
			{ // pushed ok
				gi.linkentity(check);
				// impact?
				continue;
			}

			// if it is ok to leave in the old position, do it.
			// this is only relevent for riding entities, not pushed
			check->s.origin = old_position;
			block = SV_TestEntityPosition(check);
			if (!block)
			{
				pushed_p--;
				continue;
			}
		}

		// save off the obstacle so we can call the block function
		obstacle = check;

		// move back any entities we already moved
		// go backwards, so if the same entity was pushed
		// twice, it goes back to the original position
		for (p = pushed_p - 1; p >= pushed; p--)
		{
			p->ent->s.origin = p->origin;
			p->ent->s.angles = p->angles;
			if (p->rotated)
			{
				//if (p->ent->client)
				//	p->ent->client->ps.pmove.delta_angles[YAW] = p->yaw;
				//else
					p->ent->s.angles[YAW] = p->yaw;
			}
			gi.linkentity(p->ent);
		}
		return false;
	}

	// FIXME: is there a better way to handle this?
	//  see if anything we moved has touched a trigger
	for (p = pushed_p - 1; p >= pushed; p--)
		G_TouchTriggers(p->ent);

	return true;
}

/*
============
SV_Push

Objects need to be moved back on a failed push,
otherwise riders would continue to slide.
============
*/
bool SV_Push(edict_t *pusher, vec3_t &move, vec3_t &amove)
{
	// save the pusher's original position
	pushed_p->ent = pusher;
	pushed_p->origin = pusher->s.origin;
	pushed_p->angles = pusher->s.angles;
	pushed_p->rotated = false;
	pushed_p++;

	// move the pusher to it's final position
	pusher->s.origin += move;
	pusher->s.angles += amove;
	gi.linkentity(pusher);

	// use the helper function to move entities
	return SV_PushEntities(pusher, move, amove);
}

/*
================
SV_Physics_Pusher

Bmodel objects don't interact with each other, but
push all box objects
================
*/
void SV_Physics_Pusher(edict_t *ent)
{
	vec3_t	 move, amove;
	edict_t *part;

	// if not a team captain, so movement will be handled elsewhere
	if (ent->flags & FL_TEAMSLAVE)
		return;

	// make sure all team slaves can move before commiting
	// any moves or calling any think functions
	// if the move is blocked, all moved objects will be backed out
retry:
	pushed_p = pushed;
	for (part = ent; part; part = part->teamchain)
	{
		if (part->velocity[0] || part->velocity[1] || part->velocity[2] || part->avelocity[0] || part->avelocity[1] ||
			part->avelocity[2])
		{ // object is moving
			move = part->velocity * gi.frame_time_s;
			amove = part->avelocity * gi.frame_time_s;

			if (!SV_Push(part, move, amove))
				break; // move was blocked
		}
	}
	if (pushed_p > &pushed[MAX_EDICTS])
		gi.Com_Error("pushed_p > &pushed[MAX_EDICTS], memory corrupted");

	if (part)
	{
		// if the pusher has a "blocked" function, call it
		// otherwise, just stay in place until the obstacle is gone
		if (part->moveinfo.blocked)
			part->moveinfo.blocked(part, obstacle);

		if (!obstacle->inuse)
			goto retry;
	}
	else
	{
		// the move succeeded, so call all think functions
		for (part = ent; part; part = part->teamchain)
		{
			// prevent entities that are on trains that have gone away from thinking!
			if (part->inuse)
				SV_RunThink(part);
		}
	}
}

//==================================================================

/*
=============
SV_Physics_None

Non moving objects can only think
=============
*/
void SV_Physics_None(edict_t *ent)
{
	// regular thinking
	SV_RunThink(ent);
}

/*
=============
SV_Physics_Noclip

A moving object that doesn't obey physics
=============
*/
void SV_Physics_Noclip(edict_t *ent)
{
	// regular thinking
	if (!SV_RunThink(ent) || !ent->inuse)
		return;

	ent->s.angles += (ent->avelocity * gi.frame_time_s);
	ent->s.origin += (ent->velocity * gi.frame_time_s);

	gi.linkentity(ent);
}

/*
==============================================================================

TOSS / BOUNCE

==============================================================================
*/

/*
=============
SV_Physics_Toss

Toss, bounce, and fly movement.  When onground, do nothing.
=============
*/
void SV_Physics_Toss(edict_t *ent)
{
	trace_t	 trace;
	vec3_t	 move;
	float	 backoff;
	edict_t *slave;
	bool	 wasinwater;
	bool	 isinwater;
	vec3_t	 old_origin;

	// regular thinking
	SV_RunThink(ent);

	if (!ent->inuse)
		return;

	// if not a team captain, so movement will be handled elsewhere
	if (ent->flags & FL_TEAMSLAVE)
		return;

	if (ent->velocity[2] > 0)
		ent->groundentity = nullptr;

	// check for the groundentity going away
	if (ent->groundentity)
		if (!ent->groundentity->inuse)
			ent->groundentity = nullptr;

	// if onground, return without moving
	if (ent->groundentity && ent->gravity > 0.0f) // PGM - gravity hack
	{
		if (ent->svflags & SVF_MONSTER)
		{
			M_CatagorizePosition(ent, ent->s.origin, ent->waterlevel, ent->watertype);
			M_WorldEffects(ent);
		}

		return;
	}

	old_origin = ent->s.origin;

	SV_CheckVelocity(ent);

	// add gravity
	if (ent->movetype != MOVETYPE_FLY &&
		ent->movetype != MOVETYPE_FLYMISSILE
		// RAFAEL
		// move type for rippergun projectile
		&& ent->movetype != MOVETYPE_WALLBOUNCE
		// RAFAEL
	)
		SV_AddGravity(ent);

	// move angles
	ent->s.angles += (ent->avelocity * gi.frame_time_s);

	// move origin
	int num_tries = 5;
	float time_left = gi.frame_time_s;

	while (time_left)
	{
		if (num_tries == 0)
			break;

		num_tries--;
		move = ent->velocity * time_left;
		trace = SV_PushEntity(ent, move);

		if (!ent->inuse)
			return;

		if (trace.fraction == 1.f)
			break;
		// [Paril-KEX] don't build up velocity if we're stuck.
		// just assume that the object we hit is our ground.
		else if (trace.allsolid)
		{
			ent->groundentity = trace.ent;
			ent->groundentity_linkcount = trace.ent->linkcount;
			ent->velocity = {};
			ent->avelocity = {};
			break;
		}

		time_left -= time_left * trace.fraction;

		if (ent->movetype == MOVETYPE_TOSS)
			ent->velocity = SlideClipVelocity(ent->velocity, trace.plane.normal, 0.5f);
		else
		{
			// RAFAEL
			if (ent->movetype == MOVETYPE_WALLBOUNCE)
				backoff = 2.0f;
			// RAFAEL
			else
				backoff = 1.6f;

			ent->velocity = ClipVelocity(ent->velocity, trace.plane.normal, backoff);
		}

		// RAFAEL
		if (ent->movetype == MOVETYPE_WALLBOUNCE)
			ent->s.angles = vectoangles(ent->velocity);
		// RAFAEL
		// stop if on ground
		else
		{
			if (trace.plane.normal[2] > 0.7f)
			{
				if ((ent->movetype == MOVETYPE_TOSS && ent->velocity.length() < 60.f) ||
					(ent->movetype != MOVETYPE_TOSS && ent->velocity.scaled(trace.plane.normal).length() < 60.f))
				{
					if (!(ent->flags & FL_NO_STANDING) || trace.ent->solid == SOLID_BSP)
					{
						ent->groundentity = trace.ent;
						ent->groundentity_linkcount = trace.ent->linkcount;
					}
					ent->velocity = {};
					ent->avelocity = {};
					break;
				}

				// friction for tossing stuff (gibs, etc)
				if (ent->movetype == MOVETYPE_TOSS)
				{
					ent->velocity *= 0.75f;
					ent->avelocity *= 0.75f;
				}
			}
		}

		// only toss "slides" multiple times
		if (ent->movetype != MOVETYPE_TOSS)
			break;
	}

	// check for water transition
	wasinwater = (ent->watertype & MASK_WATER);
	ent->watertype = gi.pointcontents(ent->s.origin);
	isinwater = ent->watertype & MASK_WATER;

	if (isinwater)
		ent->waterlevel = WATER_FEET;
	else
		ent->waterlevel = WATER_NONE;

	if (ent->svflags & SVF_MONSTER)
	{
		M_CatagorizePosition(ent, ent->s.origin, ent->waterlevel, ent->watertype);
		M_WorldEffects(ent);
	}
	else
	{
		if (!wasinwater && isinwater)
			gi.positioned_sound(old_origin, g_edicts, CHAN_AUTO, gi.soundindex("misc/h2ohit1.wav"), 1, 1, 0);
		else if (wasinwater && !isinwater)
			gi.positioned_sound(ent->s.origin, g_edicts, CHAN_AUTO, gi.soundindex("misc/h2ohit1.wav"), 1, 1, 0);
	}

	// prevent softlocks from keys falling into slime/lava
	if (isinwater && ent->watertype & (CONTENTS_SLIME | CONTENTS_LAVA) && ent->item &&
		(ent->item->flags & IF_KEY) && ent->spawnflags.has(SPAWNFLAG_ITEM_DROPPED))
		ent->velocity = { crandom_open() * 300, crandom_open() * 300, 300.f + (crandom_open() * 300.f) };

	// move teamslaves
	for (slave = ent->teamchain; slave; slave = slave->teamchain)
	{
		slave->s.origin = ent->s.origin;
		gi.linkentity(slave);
	}
}

/*
===============================================================================

STEPPING MOVEMENT

===============================================================================
*/

/*
=============
SV_Physics_Step

Monsters freefall when they don't have a ground entity, otherwise
all movement is done with discrete steps.

This is also used for objects that have become still on the ground, but
will fall if the floor is pulled out from under them.
=============
*/

void SV_AddRotationalFriction(edict_t *ent)
{
	int	  n;
	float adjustment;

	ent->s.angles += (ent->avelocity * gi.frame_time_s);
	adjustment = gi.frame_time_s * sv_stopspeed->value * sv_friction; // PGM now a cvar

	for (n = 0; n < 3; n++)
	{
		if (ent->avelocity[n] > 0)
		{
			ent->avelocity[n] -= adjustment;
			if (ent->avelocity[n] < 0)
				ent->avelocity[n] = 0;
		}
		else
		{
			ent->avelocity[n] += adjustment;
			if (ent->avelocity[n] > 0)
				ent->avelocity[n] = 0;
		}
	}
}

void SV_Physics_Step(edict_t *ent)
{
	bool	   wasonground;
	bool	   hitsound = false;
	float	  *vel;
	float	   speed, newspeed, control;
	float	   friction;
	edict_t	*groundentity;
	contents_t mask = G_GetClipMask(ent);

	// airborne monsters should always check for ground
	if (!ent->groundentity)
		M_CheckGround(ent, mask);

	groundentity = ent->groundentity;

	SV_CheckVelocity(ent);

	if (groundentity)
		wasonground = true;
	else
		wasonground = false;

	if (ent->avelocity[0] || ent->avelocity[1] || ent->avelocity[2])
		SV_AddRotationalFriction(ent);

	// FIXME: figure out how or why this is happening
	if (std::isnan(ent->velocity[0]) || std::isnan(ent->velocity[1]) || std::isnan(ent->velocity[2]))
		ent->velocity = {};

	// add gravity except:
	//   flying monsters
	//   swimming monsters who are in the water
	if (!wasonground)
		if (!(ent->flags & FL_FLY))
			if (!((ent->flags & FL_SWIM) && (ent->waterlevel > WATER_WAIST)))
			{
				if (ent->velocity[2] < level.gravity * -0.1f)
					hitsound = true;
				if (ent->waterlevel != WATER_UNDER)
					SV_AddGravity(ent);
			}

	// friction for flying monsters that have been given vertical velocity
	if ((ent->flags & FL_FLY) && (ent->velocity[2] != 0) && !(ent->monsterinfo.aiflags & AI_ALTERNATE_FLY))
	{
		speed = fabsf(ent->velocity[2]);
		control = speed < sv_stopspeed->value ? sv_stopspeed->value : speed;
		friction = sv_friction / 3;
		newspeed = speed - (gi.frame_time_s * control * friction);
		if (newspeed < 0)
			newspeed = 0;
		newspeed /= speed;
		ent->velocity[2] *= newspeed;
	}

	// friction for flying monsters that have been given vertical velocity
	if ((ent->flags & FL_SWIM) && (ent->velocity[2] != 0) && !(ent->monsterinfo.aiflags & AI_ALTERNATE_FLY))
	{
		speed = fabsf(ent->velocity[2]);
		control = speed < sv_stopspeed->value ? sv_stopspeed->value : speed;
		newspeed = speed - (gi.frame_time_s * control * sv_waterfriction * (float) ent->waterlevel);
		if (newspeed < 0)
			newspeed = 0;
		newspeed /= speed;
		ent->velocity[2] *= newspeed;
	}

	if (ent->velocity[2] || ent->velocity[1] || ent->velocity[0])
	{
		// apply friction
		if ((wasonground || (ent->flags & (FL_SWIM | FL_FLY))) && !(ent->monsterinfo.aiflags & AI_ALTERNATE_FLY))
		{
			vel = &ent->velocity.x;
			speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
			if (speed)
			{
				friction = sv_friction;

				// Paril: lower friction for dead monsters
				if (ent->deadflag)
					friction *= 0.5f;

				control = speed < sv_stopspeed->value ? sv_stopspeed->value : speed;
				newspeed = speed - gi.frame_time_s * control * friction;

				if (newspeed < 0)
					newspeed = 0;
				newspeed /= speed;

				vel[0] *= newspeed;
				vel[1] *= newspeed;
			}
		}

		vec3_t old_origin = ent->s.origin;

		SV_FlyMove(ent, gi.frame_time_s, mask);

		G_TouchProjectiles(ent, old_origin);

		M_CheckGround(ent, mask);

		gi.linkentity(ent);

		// ========
		// PGM - reset this every time they move.
		//       G_touchtriggers will set it back if appropriate
		ent->gravity = 1.0;
		// ========

		// [Paril-KEX] this is something N64 does to avoid doors opening
		// at the start of a level, which triggers some monsters to spawn.
		if (!level.is_n64 || level.time > FRAME_TIME_S)
			G_TouchTriggers(ent);

		if (!ent->inuse)
			return;

		if (ent->groundentity)
			if (!wasonground)
				if (hitsound)
					ent->s.event = EV_FOOTSTEP;
	}

	if (!ent->inuse) // PGM g_touchtrigger free problem
		return;
	
	if (ent->svflags & SVF_MONSTER)
	{
		M_CatagorizePosition(ent, ent->s.origin, ent->waterlevel, ent->watertype);
		M_WorldEffects(ent);

		// [Paril-KEX] last minute hack to fix Stalker upside down gravity
		if (wasonground != !!ent->groundentity)
		{
			if (ent->monsterinfo.physics_change)
				ent->monsterinfo.physics_change(ent);
		}
	}

	// regular thinking
	SV_RunThink(ent);
}

// [Paril-KEX]
inline void G_RunBmodelAnimation(edict_t *ent)
{
	auto &anim = ent->bmodel_anim;

	if (anim.currently_alternate != anim.alternate)
	{
		anim.currently_alternate = anim.alternate;
		anim.next_tick = 0_ms;
	}

	if (level.time < anim.next_tick)
		return;

	const auto &speed = anim.alternate ? anim.alt_speed : anim.speed;

	anim.next_tick = level.time + gtime_t::from_ms(speed);

	const auto &style = anim.alternate ? anim.alt_style : anim.style;
	
	const auto &start = anim.alternate ? anim.alt_start : anim.start;
	const auto &end = anim.alternate ? anim.alt_end : anim.end;

	switch (style)
	{
	case BMODEL_ANIM_FORWARDS:
		if (end >= start)
			ent->s.frame++;
		else
			ent->s.frame--;
		break;
	case BMODEL_ANIM_BACKWARDS:
		if (end >= start)
			ent->s.frame--;
		else
			ent->s.frame++;
		break;
	case BMODEL_ANIM_RANDOM:
		ent->s.frame = irandom(start, end + 1);
		break;
	}

	const auto &nowrap = anim.alternate ? anim.alt_nowrap : anim.nowrap;

	if (nowrap)
	{
		if (end >= start)
			ent->s.frame = clamp(ent->s.frame, start, end);
		else
			ent->s.frame = clamp(ent->s.frame, end, start);
	}
	else
	{
		if (ent->s.frame < start)
			ent->s.frame = end;
		else if (ent->s.frame > end)
			ent->s.frame = start;
	}
}

//============================================================================
// Vehicle Physics
//============================================================================

/*
============
SV_VehicleMove

Move a vehicle entity with rotating bounding box
============
*/
static int SV_VehicleMove(edict_t *ent, float time, contents_t mask)
{
	constexpr int MAX_CLIP_PLANES = 5;

	vec3_t primal_velocity = ent->velocity;
	vec3_t original_velocity = ent->velocity;
	int numplanes = 0;
	vec3_t planes[MAX_CLIP_PLANES];
	int blocked = 0;

	vec3_t xy_velocity = ent->velocity;
	xy_velocity[2] = 0;
	float xy_speed = xy_velocity.length();

	float time_left = time;

	// Compute bounding box centered at origin
	vec3_t origin = ent->s.origin;
	vec3_t mins = ent->size * -0.5f;
	vec3_t maxs = ent->size * 0.5f;
	mins[2] += 1;

	ent->groundentity = nullptr;

	edict_t *ignore = ent;
	vec3_t start = origin;

	constexpr int numbumps = 4;

	for (int bumpcount = 0; bumpcount < numbumps; bumpcount++)
	{
		vec3_t end = origin + ent->velocity * time_left;

		trace_t trace = gi.trace(start, mins, maxs, end, ignore, mask);

		if (trace.allsolid)
		{
			// Entity is trapped in another solid
			if (trace.ent && (trace.ent->svflags & SVF_MONSTER))
			{
				// Monster stuck in vehicle - push them out
				vec3_t dir = trace.ent->s.origin - ent->s.origin;
				dir[2] = 0;
				dir.normalize();
				dir[2] = 0.2f;

				vec3_t new_velocity = trace.ent->velocity + dir * 32.f;
				vec3_t new_origin = trace.ent->s.origin + new_velocity * gi.frame_time_s;

				trace_t tr = gi.trace(trace.ent->s.origin, trace.ent->mins, trace.ent->maxs, new_origin, trace.ent, MASK_MONSTERSOLID);
				if (tr.fraction == 1.f)
				{
					trace.ent->s.origin = new_origin;
					trace.ent->velocity = new_velocity;
					gi.linkentity(trace.ent);
				}
			}
			else if (trace.ent && trace.ent->client && xy_speed > 0)
			{
				// Player may have just dismounted - move them along
				vec3_t forward, left;
				AngleVectors(ent->s.angles, forward, left, nullptr);

				vec3_t drive = ent->s.origin + forward * ent->move_origin[0] - left * ent->move_origin[1];
				vec3_t offset = drive - trace.ent->s.origin;

				if (fabsf(offset[2]) < 64)
					offset[2] = 0;

				if (offset.length() < 16)
				{
					trace.ent->s.origin += end - origin;
					gi.linkentity(trace.ent);
					goto not_allsolid;
				}
			}

			ent->velocity = {};
			ent->avelocity = {};
			return 3;
		}

	not_allsolid:
		if (trace.fraction > 0)
		{
			// Actually covered some distance
			origin = trace.endpos;
			ent->s.origin = origin;
			original_velocity = ent->velocity;
			numplanes = 0;
		}

		if (trace.fraction == 1.f)
			break; // Moved the entire distance

		edict_t *hit = trace.ent;

		if (trace.plane.normal[2] > 0.7f)
		{
			blocked |= 1; // floor
			if (hit->solid == SOLID_BSP)
			{
				ent->groundentity = hit;
				ent->groundentity_linkcount = hit->linkcount;
			}
		}
		if (trace.plane.normal[0] > 0 || trace.plane.normal[1] > 0)
			blocked |= 1;
		if (!trace.plane.normal[2])
			blocked |= 2; // step

		// Run impact function
		G_Impact(ent, trace);
		if (!ent->inuse)
			break; // Vehicle destroyed
		if (!trace.ent || !trace.ent->inuse)
		{
			blocked = 0;
			break;
		}

		// Handle collisions with monsters/players (handled in vehicle_touch)
		if (ent->owner && (trace.ent->svflags & (SVF_MONSTER | SVF_DEADMONSTER)))
			continue;

		// Match speeds with what we hit
		ent->velocity = trace.ent->velocity;

		time_left -= time_left * trace.fraction;

		// Clipped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{
			ent->velocity = {};
			ent->avelocity = {};
			return 3;
		}

		// Players and monsters don't block us (already handled above)
		if (trace.ent->client || (trace.ent->svflags & SVF_MONSTER))
		{
			blocked = 0;
			continue;
		}

		planes[numplanes] = trace.plane.normal;
		numplanes++;

		// Modify velocity to not go into any of the clip planes
		for (int i = 0; i < numplanes; i++)
		{
			ent->velocity = SlideClipVelocity(original_velocity, planes[i], 1.f);
			int j;
			for (j = 0; j < numplanes; j++)
			{
				if (j != i)
				{
					if (ent->velocity.dot(planes[j]) < 0)
						break;
				}
			}
			if (j == numplanes)
				break;
		}

		if (numplanes != 2)
		{
			ent->velocity = {};
			ent->avelocity = {};
			blocked = 4;
			break;
		}

		vec3_t dir = planes[0].cross(planes[1]);
		float d = dir.dot(ent->velocity);
		ent->velocity = dir * d;

		if (ent->velocity.dot(primal_velocity) <= 0)
		{
			ent->velocity = {};
			ent->avelocity = {};
			return blocked;
		}
	}

	return blocked;
}

/*
============
SV_Physics_Vehicle

Vehicle physics runner
============
*/
void SV_Physics_Vehicle(edict_t *ent)
{
	edict_t *part;
	vec3_t move, amove;

	// if not a team captain, so movement will be handled elsewhere
	if (ent->flags & FL_TEAMSLAVE)
		return;

	// make sure all team slaves can move before committing
	// any moves or calling any think functions
	// if the move is blocked, all moved objects will be backed out
retry:
	pushed_p = pushed;
	for (part = ent; part; part = part->teamchain)
	{
		// Check ground
		if (!part->groundentity)
			M_CheckGround(part, G_GetClipMask(part));

		SV_CheckVelocity(part);

		// Save old origin and angles for entity movement calculation
		vec3_t old_origin = part->s.origin;
		vec3_t old_angles = part->s.angles;

		// Calculate movement and rotation
		move = part->velocity * gi.frame_time_s;
		amove = part->avelocity * gi.frame_time_s;

		// Only process if actually moving
		if (move[0] || move[1] || move[2] || amove[0] || amove[1] || amove[2])
		{
			// Move angles first
			part->s.angles += amove;

			if (part->velocity[0] || part->velocity[1] || part->velocity[2])
			{
				// Adjust bounding box for yaw rotation if we have an org_size
				if (part->org_size[0])
				{
					float yaw = part->s.angles[YAW] * float(M_PI) / 180.f;
					float ca = cosf(yaw);
					float sa = sinf(yaw);

					vec3_t s2 = part->org_size * 0.5f;

					vec3_t p[4];
					p[0] = {-s2[0] * ca + s2[1] * sa, -s2[1] * ca - s2[0] * sa, 0};
					p[1] = {s2[0] * ca + s2[1] * sa, -s2[1] * ca + s2[0] * sa, 0};
					p[2] = {-s2[0] * ca - s2[1] * sa, s2[1] * ca - s2[0] * sa, 0};
					p[3] = {s2[0] * ca - s2[1] * sa, s2[1] * ca + s2[0] * sa, 0};

					vec3_t mins, maxs;
					mins[0] = min(min(p[0][0], p[1][0]), min(p[2][0], p[3][0]));
					mins[1] = min(min(p[0][1], p[1][1]), min(p[2][1], p[3][1]));
					maxs[0] = max(max(p[0][0], p[1][0]), max(p[2][0], p[3][0]));
					maxs[1] = max(max(p[0][1], p[1][1]), max(p[2][1], p[3][1]));

					part->size[0] = maxs[0] - mins[0];
					part->size[1] = maxs[1] - mins[1];
					part->mins[0] = -part->size[0] / 2;
					part->mins[1] = -part->size[1] / 2;
					part->maxs[0] = part->size[0] / 2;
					part->maxs[1] = part->size[1] / 2;
					gi.linkentity(part);
				}

				// Use custom vehicle movement with rotating bbox
				// Note: SV_VehicleMove handles collisions internally and may return blocked flags,
				// but we handle entity movement blocking separately below
				SV_VehicleMove(part, gi.frame_time_s, MASK_ALL);

				gi.linkentity(part);
				G_TouchTriggers(part);

				if (!part->inuse)
					return;

				// Calculate movement delta for entity movement
				vec3_t move_delta = part->s.origin - old_origin;
				vec3_t angle_delta = part->s.angles - old_angles;

				// Move entities that are on or inside this vehicle part
				// Use the proven SV_PushEntities logic
				if (move_delta[0] || move_delta[1] || move_delta[2] || angle_delta[YAW])
				{
					// Save the vehicle part's position in pushed array (for rollback if blocked)
					pushed_p->ent = part;
					pushed_p->origin = old_origin;
					pushed_p->angles = old_angles;
					pushed_p->rotated = false;
					pushed_p++;

					// Use SV_PushEntities to move entities on/in the vehicle
					// It will automatically skip the driver (part->owner) and the vehicle itself
					if (!SV_PushEntities(part, move_delta, angle_delta))
					{
						// Movement was blocked - handle it
						if (part->moveinfo.blocked)
							part->moveinfo.blocked(part, obstacle);

						if (!obstacle || !obstacle->inuse)
							goto retry;

						// Back out all moves (including the vehicle part)
						for (pushed_t *p = pushed_p - 1; p >= pushed; p--)
						{
							p->ent->s.origin = p->origin;
							p->ent->s.angles = p->angles;
							if (p->rotated)
							{
								if (p->ent->client)
									p->ent->client->ps.pmove.delta_angles[YAW] = p->yaw;
								else
									p->ent->s.angles[YAW] = p->yaw;
							}
							gi.linkentity(p->ent);
						}
						return;
					}
				}
			}
		}
	}

	if (pushed_p > &pushed[MAX_EDICTS])
		gi.Com_Error("pushed_p > &pushed[MAX_EDICTS], memory corrupted");

	// the move succeeded, so call all think functions
	for (part = ent; part; part = part->teamchain)
	{
		// prevent entities that are on vehicles that have gone away from thinking!
		if (part->inuse)
		{
			// Regular thinking
			SV_RunThink(part);
			part->oldvelocity = part->velocity;
		}
	}

	// FIXME: is there a better way to handle this?
	//  see if anything we moved has touched a trigger
	for (pushed_t *p = pushed_p - 1; p >= pushed; p--)
		G_TouchTriggers(p->ent);
}

//============================================================================

/*
================
G_RunEntity

================
*/
void G_RunEntity(edict_t *ent)
{
	// PGM
	trace_t trace;
	vec3_t	previous_origin;
	bool	has_previous_origin = false;

	if (ent->movetype == MOVETYPE_STEP)
	{
		previous_origin = ent->s.origin;
		has_previous_origin = true;
	}
	// PGM

	if (ent->prethink)
		ent->prethink(ent);

	// bmodel animation stuff runs first, so custom entities
	// can override them
	if (ent->bmodel_anim.enabled)
		G_RunBmodelAnimation(ent);

	switch ((int) ent->movetype)
	{
	case MOVETYPE_PUSH:
	case MOVETYPE_STOP:
		SV_Physics_Pusher(ent);
		break;
	case MOVETYPE_NONE:
		SV_Physics_None(ent);
		break;
	case MOVETYPE_NOCLIP:
		SV_Physics_Noclip(ent);
		break;
	case MOVETYPE_STEP:
		SV_Physics_Step(ent);
		break;
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
	// RAFAEL
	case MOVETYPE_WALLBOUNCE:
		// RAFAEL
		SV_Physics_Toss(ent);
		break;
	// ROGUE
	case MOVETYPE_NEWTOSS:
		SV_Physics_NewToss(ent);
		break;
	// ROGUE
	case MOVETYPE_VEHICLE:
		SV_Physics_Vehicle(ent);
		break;
	default:
		gi.Com_ErrorFmt("SV_Physics: bad movetype {}", (int32_t)ent->movetype);
	}

	// PGM
	if (has_previous_origin && ent->movetype == MOVETYPE_STEP)
	{
		// if we moved, check and fix origin if needed
		if (ent->s.origin != previous_origin)
		{
			trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, previous_origin, ent, G_GetClipMask(ent));
			if (trace.allsolid || trace.startsolid)
				ent->s.origin = previous_origin;
		}
	}
	// PGM

#if 0
	// disintegrator stuff; only for non-players
	if (ent->disintegrator_time)
	{
		if (ent->disintegrator_time > 100_sec)
		{
			gi.WriteByte(svc_temp_entity);
			gi.WriteByte(TE_BOSSTPORT);
			gi.WritePosition(ent->s.origin);
			gi.multicast(ent->s.origin, MULTICAST_PHS, false);

			Killed(ent, ent->disintegrator, ent->disintegrator, 999999, vec3_origin, MOD_NUKE);
			G_FreeEdict(ent);
		}
		
		ent->disintegrator_time = max(0_ms, ent->disintegrator_time - (15000_ms / gi.tick_rate));

		if (ent->disintegrator_time)
			ent->s.alpha = max(1 / 255.f, 1.f - (ent->disintegrator_time.seconds() / 100.f));
		else
			ent->s.alpha = 1;
	}
#endif

	if (ent->postthink)
		ent->postthink(ent);
}

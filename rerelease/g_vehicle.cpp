// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.
// Vehicle support ported from Lazarus/KMQuake2

#include "g_local.h"

// Vehicle speed states (stored in moveinfo.distance as a speed multiplier state)
// Range from RFAST (-3, full reverse) through STOP (0) to FAST (3, full forward)
constexpr int VEHICLE_RFAST = -3;
constexpr int VEHICLE_STOP = 0;
constexpr int VEHICLE_FAST = 3;

// Spawnflags
constexpr spawnflags_t SPAWNFLAG_VEHICLE_BLOCK_STOPS = 4_spawnflag;

// Forward declarations
static void vehicle_think(edict_t *self);
static void vehicle_disengage(edict_t *vehicle);

// Helper to get/set vehicle speed state (stored in moveinfo.distance)
inline int vehicle_get_state(edict_t *self) { return (int)self->moveinfo.distance; }
inline void vehicle_set_state(edict_t *self, int state) { self->moveinfo.distance = (float)state; }

// ============================================================================
// Vehicle explosion/death
// ============================================================================

DIE(func_vehicle_die)(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, const vec3_t &point, const mod_t &mod)->void
{
    // Fire death target
    if (self->deathtarget)
    {
        self->target = self->deathtarget;
        G_UseTargets(self, attacker);
    }

    // bmodel origins are (0 0 0), we need to adjust that here
    vec3_t size = self->size * 0.5f;
    vec3_t origin = self->absmin + size;
    self->s.origin = origin;

    self->takedamage = false;

    if (self->dmg)
        T_RadiusDamage(self, attacker, (float)self->dmg, nullptr, (float)(self->dmg + 40), DAMAGE_NONE, MOD_EXPLOSIVE);

    self->velocity = origin - inflictor->s.origin;
    self->velocity.normalize();
    self->velocity *= 150.f;

    int mass = self->mass;
    if (!mass)
        mass = 75;

    // big chunks
    if (mass >= 100)
    {
        size_t count = mass / 100;
        if (count > 8)
            count = 8;
        ThrowGibs(self, 1, {{count, "models/objects/debris1/tris.md2", GIB_METALLIC | GIB_DEBRIS}});
    }

    // small chunks
    size_t count = mass / 25;
    if (count > 16)
        count = 16;
    ThrowGibs(self, 2, {{count, "models/objects/debris2/tris.md2", GIB_METALLIC | GIB_DEBRIS}});

    if (self->dmg)
        BecomeExplosion1(self);
    else
        G_FreeEdict(self);
}

// ============================================================================
// Vehicle blocked callback
// ============================================================================

MOVEINFO_BLOCKED(vehicle_blocked)(edict_t *self, edict_t *other)->void
{
    if (self->spawnflags.has(SPAWNFLAG_VEHICLE_BLOCK_STOPS) || other == world)
    {
        self->velocity = {};
        self->avelocity = {};
        self->moveinfo.current_speed = 0;
        gi.linkentity(self);
        return;
    }

    if (other->takedamage)
    {
        edict_t *attacker;
        if (self->teammaster && self->teammaster->owner)
            attacker = self->teammaster->owner;
        else
            attacker = self->owner;
        T_Damage(other, self, attacker, vec3_origin, other->s.origin, vec3_origin, self->teammaster ? self->teammaster->dmg : self->dmg, 10, DAMAGE_NONE, MOD_CRUSH);
    }
    else
    {
        self->velocity = {};
        self->avelocity = {};
        self->moveinfo.current_speed = 0;
        vehicle_set_state(self, VEHICLE_STOP);
        gi.linkentity(self);
    }

    if (!(other->svflags & SVF_MONSTER) && !other->client)
    {
        T_Damage(other, self, self, vec3_origin, other->s.origin, vec3_origin, 100000, 1, DAMAGE_NONE, MOD_CRUSH);
        if (other && other->inuse)
            BecomeExplosion1(other);
    }
}

// ============================================================================
// Vehicle touch - handles collision damage
// ============================================================================

TOUCH(vehicle_touch)(edict_t *self, edict_t *other, const trace_t &tr, bool other_touching_self)->void
{
    if (other == world || self->spawnflags.has(SPAWNFLAG_VEHICLE_BLOCK_STOPS))
    {
        self->velocity = {};
        self->avelocity = {};
        self->moveinfo.current_speed = 0;
        gi.linkentity(self);
    }

    // Check if a player wants to mount the vehicle
    if (!self->owner && other->client && other->movetype != MOVETYPE_NOCLIP)
    {
        if (other->client->cmd.buttons & BUTTON_USE)
        {
            if (level.time - other->client->vehicle_framenum > FRAME_TIME_S * 2)
            {
                // Get driving position
                vec3_t forward, left;
                AngleVectors(self->s.angles, forward, left, nullptr);
                vec3_t drive = self->s.origin + forward * self->move_origin[0] - left * self->move_origin[1];
                drive[2] += self->move_origin[2];

                // Determine distance from vehicle "move_origin"
                vec3_t dir = drive - other->s.origin;
                if (fabsf(dir[2]) < 64)
                    dir[2] = 0;

                if (dir.length() < 16)
                {
                    other->client->vehicle_framenum = level.time;
                    // Player has taken control of vehicle
                    // Move vehicle up slightly to avoid roundoff collisions
                    self->s.origin[2] += 1;
                    gi.linkentity(self);

                    if (self->message)
                        gi.LocClient_Print(other, PRINT_CENTER, self->message);

                    self->owner = other;
                    other->movetype = MOVETYPE_PUSH;
                    other->gravity = 0;
                    other->vehicle = self;
                    // Turn off client side prediction for this player
                    other->client->ps.pmove.pm_flags |= (PMF_NO_POSITIONAL_PREDICTION | PMF_NO_ANGULAR_PREDICTION);
                    // Force a good driving position
                    other->s.origin = drive;
                    gi.linkentity(other);
                    // Vehicle idle noise
                    self->s.sound = self->noise_index2;
                    // Reset wait time so we can start accelerating
                    self->moveinfo.wait = 0;
                    return;
                }
            }
        }
    }

    if (!self->owner)
        return; // if vehicle isn't being driven, it can't hurt anybody
    if (other == self->owner)
        return; // can't hurt the driver
    if (!other->takedamage)
        return;
    // we damage func_explosives elsewhere. About all that's left to hurt are players and monsters
    if (!other->client && !(other->svflags & SVF_MONSTER))
        return;

    float vspeed = self->velocity.length();
    if (!vspeed)
        return;

    vec3_t dir = other->s.origin - self->s.origin;
    dir[2] = 0;
    dir.normalize();

    vec3_t v = self->velocity;
    v.normalize();

    // damage and knockback are proportional to square of velocity * mass of vehicle
    vspeed *= dir.dot(v);
    float mspeed = other->velocity.length() * dir.dot(v);
    vspeed -= mspeed;

    if (vspeed <= 0.f)
        return;

    // for speed < 200, don't do damage but move monster
    if (vspeed < 200.f)
    {
        if (other->mass > self->mass)
            vspeed *= (float)self->mass / (float)other->mass;

        vec3_t new_velocity = other->velocity + dir * vspeed;
        vec3_t new_origin = other->s.origin + new_velocity * gi.frame_time_s;
        new_origin[2] += 2;

        // if the move would place the monster in a solid, make him go splat
        vec3_t end = new_origin;
        end[2] -= 1;
        trace_t trace = gi.trace(new_origin, other->mins, other->maxs, end, self, CONTENTS_SOLID);

        if (trace.startsolid)
        {
            // splat
            T_Damage(other, self, self->owner, dir, self->s.origin, vec3_origin,
                     other->health - other->gib_health + 1, 0, DAMAGE_NONE, MOD_VEHICLE);
        }
        else
        {
            // go ahead and move the bastard
            other->velocity = new_velocity;
            other->s.origin = new_origin;
            gi.linkentity(other);
        }
        return;
    }

    if (other->damage_debounce_time > level.time)
        return;
    other->damage_debounce_time = level.time + 200_ms;

    float points = 100.f * ((float)self->mass / 2000.f * vspeed * vspeed / 160000.f);

    // knockback takes too long to take effect. If we can move him w/o throwing him
    // into a solid, do so NOW
    dir[2] = 0.2f; // make knockback slightly upward
    vec3_t new_velocity = other->velocity + dir * vspeed;
    vec3_t new_origin = other->s.origin + new_velocity * gi.frame_time_s;

    float knockback;
    if (gi.pointcontents(new_origin) & CONTENTS_SOLID)
        knockback = (160.f / 500.f) * 200.f * ((float)self->mass / 2000.f * vspeed * vspeed / 160000.f);
    else
    {
        knockback = 0;
        other->velocity = new_velocity;
        other->s.origin = new_origin;
    }

    T_Damage(other, self, self->owner, dir, self->s.origin, vec3_origin,
             (int)points, (int)knockback, DAMAGE_NONE, MOD_VEHICLE);
    gi.linkentity(other);
}

// ============================================================================
// Disengage - player exits vehicle
// ============================================================================

static void vehicle_disengage(edict_t *vehicle)
{
    edict_t *driver = vehicle->owner;
    if (!driver)
        return;

    vec3_t forward, left;
    AngleVectors(vehicle->s.angles, forward, left, nullptr);

    driver->velocity = vehicle->velocity;
    driver->s.origin = vehicle->s.origin + forward * vehicle->move_origin[0] - left * vehicle->move_origin[1];
    driver->s.origin[2] += vehicle->move_origin[2];

    driver->vehicle = nullptr;
    driver->client->vehicle_framenum = level.time;
    driver->movetype = MOVETYPE_WALK;
    driver->gravity = 1;
    // turn ON client side prediction for this player
    driver->client->ps.pmove.pm_flags &= ~(PMF_NO_POSITIONAL_PREDICTION | PMF_NO_ANGULAR_PREDICTION);
    vehicle->s.sound = 0;
    gi.linkentity(driver);
    vehicle->owner = nullptr;
}

// ============================================================================
// Vehicle think - main per-frame logic
// ============================================================================

THINK(vehicle_think)(edict_t *self)->void
{
    self->nextthink = level.time + FRAME_TIME_S;

    vec3_t v = self->oldvelocity;
    v[2] = 0;
    float speed = v.length();

    if (speed > 0)
        self->s.effects |= EF_ANIM_ALL;
    else
        self->s.effects &= ~EF_ANIM_ALL;

    vec3_t forward, left;
    AngleVectors(self->s.angles, forward, left, nullptr);

    if (forward.dot(self->oldvelocity) < 0)
        speed = -speed;
    self->moveinfo.current_speed = speed;

    int vehicle_state = vehicle_get_state(self);

    if (self->owner)
    {
        // We have a driver
        if (self->owner->health <= 0)
        {
            vehicle_disengage(self);
            return;
        }

        // Check if driver wants to exit (use key)
        if (self->owner->client->cmd.buttons & BUTTON_USE)
        {
            // if he's pressing the use key, and he didn't just get on or off, disengage
            if (level.time - self->owner->client->vehicle_framenum > FRAME_TIME_S * 2)
            {
                self->oldvelocity = self->velocity;
                vehicle_disengage(self);
                return;
            }
        }

        // Handle acceleration/deceleration (use timestamp for wait timing)
        if (self->owner->client->cmd.forwardmove != 0 && level.time > self->timestamp)
        {
            if (self->owner->client->cmd.forwardmove > 0)
            {
                if (vehicle_state < VEHICLE_FAST)
                {
                    vehicle_state++;
                    vehicle_set_state(self, vehicle_state);
                    self->moveinfo.next_speed = vehicle_state * self->speed / 3.f;
                    self->timestamp = level.time + FRAME_TIME_S;
                }
            }
            else
            {
                if (vehicle_state > VEHICLE_RFAST)
                {
                    vehicle_state--;
                    vehicle_set_state(self, vehicle_state);
                    self->moveinfo.next_speed = vehicle_state * self->speed / 3.f;
                    self->timestamp = level.time + FRAME_TIME_S;
                }
            }
        }

        // Smooth speed transitions
        if (self->moveinfo.current_speed < self->moveinfo.next_speed)
        {
            speed = self->moveinfo.current_speed + self->accel / 10.f;
            if (speed > self->moveinfo.next_speed)
                speed = self->moveinfo.next_speed;
        }
        else if (self->moveinfo.current_speed > self->moveinfo.next_speed)
        {
            speed = self->moveinfo.current_speed - self->decel / 10.f;
            if (speed < self->moveinfo.next_speed)
                speed = self->moveinfo.next_speed;
        }

        self->velocity = forward * speed;

        // Handle steering - allow turning even when stationary
        if (self->owner->client->cmd.sidemove != 0)
        {
            if (speed != 0)
            {
                // When moving, turn speed is proportional to forward speed
                float aspeed = 180.f * speed / (float(M_PI) * self->radius);
                if (self->owner->client->cmd.sidemove > 0)
                    aspeed = -aspeed;
                self->avelocity[1] = aspeed;
            }
            else
            {
                // When stationary, use a fixed turn rate (90 degrees per second)
                float aspeed = 90.f;
                if (self->owner->client->cmd.sidemove > 0)
                    aspeed = -aspeed;
                self->avelocity[1] = aspeed;
            }
        }
        else
            self->avelocity[1] = 0;

        // Sound effects
        if (speed != 0)
            self->s.sound = self->noise_index;
        else
            self->s.sound = self->noise_index2;

        gi.linkentity(self);

        // Copy velocities and set position of driver
        self->owner->velocity = self->velocity;
        self->owner->s.origin = self->s.origin + forward * self->move_origin[0] - left * self->move_origin[1];
        self->owner->s.origin[2] += self->move_origin[2];

        // Turn driver (even when stationary, so they can pivot)
        if (self->avelocity[1] != 0)
        {
            float yaw = self->avelocity[1] * gi.frame_time_s;
            self->owner->s.angles[YAW] += yaw;
            self->owner->client->ps.pmove.delta_angles[YAW] += yaw;
            // Don't freeze player view - let them look around while driving
        }

        self->oldvelocity = self->velocity;
        gi.linkentity(self->owner);
    }
    else
    {
        // No driver
        // if vehicle has stopped, drop it to ground
        // otherwise slow it down
        if (speed == 0)
        {
            if (!self->groundentity)
                SV_AddGravity(self);
        }
        else
        {
            // no driver... slow to an eventual stop in no more than 5 sec
            self->moveinfo.next_speed = 0;
            vehicle_set_state(self, VEHICLE_STOP);

            float newspeed;
            if (speed > 0)
                newspeed = max(0.f, speed - self->speed / 50.f);
            else
                newspeed = min(0.f, speed + self->speed / 50.f);

            self->velocity = forward * newspeed;
            self->avelocity *= newspeed / speed;
            self->oldvelocity = self->velocity;
            gi.linkentity(self);
        }

        // Check if a player wants to mount the vehicle (only when no driver)
        // First get driving position
        vec3_t drive = self->s.origin + forward * self->move_origin[0] - left * self->move_origin[1];
        drive[2] += self->move_origin[2];

        // Find a player
        for (auto player : active_players())
        {
            if (player->movetype == MOVETYPE_NOCLIP)
                continue;
            if (!(player->client->cmd.buttons & BUTTON_USE))
                continue;
            if (level.time - player->client->vehicle_framenum <= FRAME_TIME_S * 2)
                continue;

            // Determine distance from vehicle "move_origin"
            vec3_t dir = drive - player->s.origin;
            if (fabsf(dir[2]) < 64)
                dir[2] = 0;

            if (dir.length() < 16)
            {
                player->client->vehicle_framenum = level.time;
                // Player has taken control of vehicle
                // Move vehicle up slightly to avoid roundoff collisions
                self->s.origin[2] += 1;
                gi.linkentity(self);

                if (self->message)
                    gi.LocClient_Print(player, PRINT_CENTER, self->message);

                self->owner = player;
                player->movetype = MOVETYPE_PUSH;
                player->gravity = 0;
                player->vehicle = self;
                // Turn off client side prediction for this player
                player->client->ps.pmove.pm_flags |= (PMF_NO_POSITIONAL_PREDICTION | PMF_NO_ANGULAR_PREDICTION);
                // Force a good driving position
                player->s.origin = drive;
                gi.linkentity(player);
                // Vehicle idle noise
                self->s.sound = self->noise_index2;
                // Reset wait time so we can start accelerating
                self->moveinfo.wait = 0;
                break;
            }
        }
    }
}

// ============================================================================
// Pre-think to set initial yaw
// ============================================================================

PRETHINK(turn_vehicle)(edict_t *self)->void
{
    self->s.angles[YAW] = self->ideal_yaw;
    gi.linkentity(self);
    self->prethink = nullptr;
}

// ============================================================================
// Spawn function
// ============================================================================

void SP_func_vehicle(edict_t *self)
{
    // Pre-cache debris models
    gi.modelindex("models/objects/debris1/tris.md2");
    gi.modelindex("models/objects/debris2/tris.md2");

    self->ideal_yaw = self->s.angles[YAW];
    self->s.angles = {};
    self->solid = SOLID_BSP;
    gi.setmodel(self, self->model);

    self->movetype = MOVETYPE_VEHICLE;

    if (!self->speed)
        self->speed = 200;
    if (!self->accel)
        self->accel = self->speed; // accelerates to full speed in 1 second (approximate)
    if (!self->decel)
        self->decel = self->accel;
    if (!self->mass)
        self->mass = 2000;
    if (!self->radius)
        self->radius = 256;

    self->moveinfo.blocked = vehicle_blocked;
    self->touch = vehicle_touch;
    self->think = vehicle_think;
    self->nextthink = level.time + FRAME_TIME_S;
    self->noise_index = gi.soundindex("world/land.wav");  // placeholder for engine sound
    self->noise_index2 = gi.soundindex("world/land.wav"); // placeholder for idle sound
    self->velocity = {};
    self->avelocity = {};
    self->moveinfo.current_speed = 0;
    vehicle_set_state(self, VEHICLE_STOP);
    gi.linkentity(self);
    self->org_size = self->size;

    if (self->ideal_yaw != 0)
        self->prethink = turn_vehicle;

    if (self->health)
    {
        self->die = func_vehicle_die;
        self->takedamage = true;
    }
    else
        self->takedamage = false;
}

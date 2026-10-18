/* halo_skate.h: the skate engine's C interface (port/skate/halo-skate, a
Rust static library). Positions are world units, Z up; see port/skate/README.md. */

#ifndef HALO_SKATE_H
#define HALO_SKATE_H

/* an Xbox 360 pad as the engine reads it: XINPUT_GAMEPAD's buttons (A 0x1000,
B 0x2000, X 0x4000, Y 0x8000, shoulders 0x100 and 0x200, stick clicks 0x40 and
0x80, Start 0x10, Back 0x20, the D-pad 0x1 to 0x8), triggers 0 to 255, and
sticks with +Y up */
struct halo_skate_pad
{
	unsigned short buttons;
	unsigned char triggers[2];
	short left[2];
	short right[2];
};

struct halo_skate_frame
{
	float position[3];
	float forward[3];
	float up[3];
	float velocity[3]; /* the board's, world units a second */
	float skater_velocity[3]; /* the skater's own off the board, world units a second */
	float facing[3]; /* the way the skater faces, a unit vector */
	int off_board; /* 1 while the skater is off the board (walking, or jumping off it) */
	float camera_position[3];
	float camera_forward[3];
	float camera_up[3];
	float camera_field_of_view; /* degrees */
	int has_camera;
	unsigned int tick;
	char state[32];
};

/* sends the engine's log lines (load timings, failures) to log, one line a
call without its line end, from the engine's thread too; NULL: back to stderr */
void halo_skate_set_log(void (*log)(const char *line));
/* makes the skate session (the skater, its animations, graphs and physics:
the slow part) in the background before any map; 0, or -1 when the assets
folder is missing or the engine could not start. Never waits */
int halo_skate_preload(const char *assets);
/* 1 while the preload is under way, else 0 */
int halo_skate_preloading(void);
/* builds a map's collision (triangles of 9 floats) in the background, after
any preload; a map sent before the last was built replaces it. 0, or -1 */
int halo_skate_load(const char *assets, const float *triangles, int count);
/* builds the loaded level's collision again (scenery made or gone) aside while
the skater rides the old, and swaps it in between steps, never on a rail or in
the air; the state stays ready (2). As halo_skate_load with no map loaded from
assets yet. 0, or -1 */
int halo_skate_reload(const char *assets, const float *triangles, int count);
/* the map's collision: 0 none, 1 loading, 2 ready, -1 failed (halo_skate_error) */
int halo_skate_state(void);
const char *halo_skate_error(void);
int halo_skate_activate(const float *position, float yaw, struct halo_skate_frame *out);
/* a teleporter took the skater: on the board at position facing yaw, as
halo_skate_activate, and moving at velocity (3 floats, world units a second)
from the start. 0, or -1 */
int halo_skate_teleport(const float *position, float yaw, const float *velocity, struct halo_skate_frame *out);
/* a blast pushes the skater, as Halo's pushes a player on foot: x, y, z world
units a second added before the next step (the board's too while riding; past
the obstacle bail speed, a bail). 0, or -1 */
int halo_skate_apply_impulse(float x, float y, float z);
/* 0: the new pose; 1: the engine did not answer in time (the last pose); 2: a
step failed and the skater was put back on the board where it last was; -1:
failed (halo_skate_error) */
int halo_skate_step(const struct halo_skate_pad *pad, float dt, struct halo_skate_frame *out);
/* after halo_skate_step returned 1, waits up to wait_ms for that step's pose
(the tests; the game moves on with the last pose): codes as halo_skate_step */
int halo_skate_await_step(unsigned int wait_ms, struct halo_skate_frame *out);
/* starts a step as halo_skate_step does and returns at once (0, or -1):
halo_skate_await_step takes its pose. Each bot's skater's step is started so,
then their poses taken, so that their sessions step side by side */
int halo_skate_step_begin(const struct halo_skate_pad *pad, float dt);
void halo_skate_suspend(void);
int halo_skate_set_skeleton(int count, const char *names, const short *parents, const float *inverse_defaults);
/* the biped's nodes posed as the skater; riding, its soles rest on the deck
(halo_skate_set_feet) */
int halo_skate_pose_nodes(float *out, int capacity);
/* how the riding biped stands on the deck: its foot nodes ankle_height world
units above its soles (0 for unknown: its ankles stay on the skater's), and
raised offset world units more along the board's up */
void halo_skate_set_feet(float ankle_height, float offset);
/* the heading the following camera keeps (camera.rs) */
struct halo_skate_follow
{
	float heading; /* radians, 0 to 2 pi */
	float still; /* seconds off the board slower than minimum_speed */
};
/* the following camera's next heading, dt seconds on, radians within 0 to 2
pi: on the board, follow turned fraction of the way toward the way the board
travels (frame->velocity), off it toward the way the skater walks
(frame->skater_velocity), the shorter way round, when that is faster than
minimum_speed (world units a second); off it and still a second, slowly round
behind the way it faces; else held. Updates follow and returns its heading */
float halo_skate_follow(struct halo_skate_follow *follow, const struct halo_skate_frame *frame,
	float minimum_speed, float fraction, float dt);

/* the skateboard (board.rs): the board meshes of the converted skater model,
skinned to the skater, which the game draws itself */
#define HALO_SKATE_BOARD_VERTEX_FLOATS 8 /* position, normal, texture coordinate */
#define HALO_SKATE_BOARD_TEXTURES 8 /* the most the game draws a board with */

struct halo_skate_board_info
{
	unsigned int generation; /* changes when another board is loaded */
	int vertex_count;
	int index_count;
	int surface_count;
	int texture_count;
};

/* a run of triangles drawn with one texture */
struct halo_skate_board_surface
{
	int first_index;
	int index_count;
	int texture;
};

int halo_skate_board_info(struct halo_skate_board_info *out);
int halo_skate_board_mesh(unsigned int *indices, int index_capacity, struct halo_skate_board_surface *surfaces,
	int surface_capacity);
int halo_skate_board_texture(int index, int *width, int *height, unsigned char *rgba, int capacity);
/* the board skinned to the skater, grown halo_skate_set_board_scale times */
int halo_skate_board_vertices(float *out, int capacity);
/* grows the drawn board scale times about its middle (1.05 at first: Master
Chief is larger than the skater it was made for); returns the scale kept */
float halo_skate_set_board_scale(float scale);

/* the game's moving objects the board and the skater collide with, as a wall:
other bipeds as capsules, vehicles, devices and scenery made later as their
own collision, triangles the game places each tick (obstacles.rs) */
#define HALO_SKATE_OBSTACLE_BIPED 0
#define HALO_SKATE_OBSTACLE_VEHICLE 1
#define HALO_SKATE_OBSTACLE_SCENERY 2
#define HALO_SKATE_OBSTACLE_SKATER 3
#define HALO_SKATE_OBSTACLE_BOX 0
#define HALO_SKATE_OBSTACLE_CAPSULE 1
#define HALO_SKATE_OBSTACLE_MESH 2
#define HALO_SKATE_MAXIMUM_OBSTACLES 32
/* the most triangles a call's meshes take between them */
#define HALO_SKATE_MAXIMUM_OBSTACLE_TRIANGLES 4096

struct halo_skate_obstacle
{
	int kind; /* HALO_SKATE_OBSTACLE_BIPED, _VEHICLE, _SCENERY or _SKATER (another
		skater's body or board) */
	int shape; /* HALO_SKATE_OBSTACLE_BOX, _CAPSULE along up, or _MESH */
	float center[3]; /* its middle, world units (a mesh's: what it turns about) */
	float forward[3]; /* a box's axes, unit vectors (a capsule's is up) */
	float left[3];
	float up[3];
	/* a box's half sizes along forward, left and up; a capsule's radius,
	0, and half its height with its caps, world units */
	float half_extents[3];
	float velocity[3]; /* world units a second */
	float angular_velocity[3]; /* radians a second about center, world axes */
	/* a mesh's triangles among those given with it (halo_skate_set_obstacles):
	the first and how many, each wound counterclockwise seen from outside */
	int first_triangle;
	int triangle_count;
	int tag; /* the game's name for it (its object), told back with a held hit */
	int held; /* non-zero: a hit on it is the game host's to decide, not the
		engine's (a network game's other players, their vehicles, other
		skaters): the engine holds any bail it would have had by it, and tells
		of the hit (halo_skate_take_held_hit) */
	int run_over; /* non-zero (held ones): a person on foot the riding skater runs
		over, as a vehicle does in Halo: a contact closing at the run-over speed
		(halo_skate_set_run_over) or faster goes through them, slowed, and is told
		as run over; slower, it is held as any */
};

/* the obstacles the board and the skater collide with from the next step,
where they are now (nearest first): they carry on at their velocities until
the next call. A mesh's triangles are among triangles (triangle_count of 9
floats, world units, placed where the object is now; NULL with none). Unknown
or invalid ones are left out, and no more than HALO_SKATE_MAXIMUM_OBSTACLES
are taken, nor more than HALO_SKATE_MAXIMUM_OBSTACLE_TRIANGLES triangles; how
many were, or -1 */
int halo_skate_set_obstacles(const struct halo_skate_obstacle *obstacles, int count, const float *triangles,
	int triangle_count);
/* how fast (world units a second, along the contact) running into a person or
a vehicle knocks the skater over: a bail (6 m/s at first); slower is a bump.
Returns the speed kept */
float halo_skate_set_obstacle_bail_speed(float speed);

/* running over people on foot (halo_skate_obstacle's run_over): a contact
closing at speed world units a second or faster goes through them, the board
and the skater keeping keep (0 to 1) of their speed; below 0 (at first), nobody
is run over. Returns the speed kept */
float halo_skate_set_run_over(float speed, float keep);

/* hits on held obstacles (halo_skate_obstacle's held), for the game's host to
decide (../linux/game/skate_authority.c) */
struct halo_skate_held_hit
{
	int tag; /* the obstacle's (its object) */
	int kind; /* HALO_SKATE_OBSTACLE_BIPED, _VEHICLE, _SCENERY or _SKATER */
	float closing; /* world units a second along the contact */
	float normal[3]; /* the contact's, from the obstacle toward the skater */
	int touched; /* the above are of a contact since the last call */
	int bail_held; /* the engine held a bail it would have had since the last call */
	int run_over; /* the contact above went through a person on foot
		(halo_skate_obstacle's run_over) rather than stopping at them */
};
/* what the steps since the last call had to tell of hits on held obstacles:
the fastest contact with one, and whether a bail was held. 1 with out filled,
0 with nothing to tell, -1 on failure */
int halo_skate_take_held_hit(struct halo_skate_held_hit *out);
/* the game's host decided the skater bails: knocked over on the next step
they ride, as running into a person at closing world units a second is (given
up after half a second of not riding). 0, or -1 */
int halo_skate_force_bail(float closing);

/* multiplayer (../linux/game/skate_network.c): where the skater's last pose
has the board, 7 floats (a unit quaternion x, y, z, w, and a position in world
units), 0 or -1 with no board or pose; and the board's vertices placed whole
there (another machine's skater's board), as halo_skate_board_vertices writes
them: how many, or 0 */
int halo_skate_board_placement(float *out);
int halo_skate_board_vertices_at(const float *placement, float *out, int capacity);
/* another machine's skater's board at placement (7 floats, as above) as an
obstacle (skate.c): a box of kind HALO_SKATE_OBSTACLE_SKATER, turned as the
board is, around it as drawn (grown halo_skate_set_board_scale times), its
velocity 0 for the game to set. 0, or -1 with no board or a placement that is
not one */
int halo_skate_board_obstacle(const float *placement, struct halo_skate_obstacle *out);

/* ---------- several skaters (lib.rs): a session for each, this machine's player's
(0, which the calls above drive) and the host's bots' (1 on). Each _in call is
the call of its name for that session, which has its own worker, collision,
skater, pose and hits; the animation banks, the board and the settings above
are shared. A number that is no session fails as the call does with nothing
loaded. */

/* whether assets is a folder the converter wrote (1, else 0), loading
nothing: a dedicated server makes no skater of its own, only its bots' */
int halo_skate_assets_present(const char *assets);
/* how many sessions there may be at once, 0 with them */
int halo_skate_sessions(void);
/* the session ended: its worker, skater and memory gone (made again by
halo_skate_preload_in) */
void halo_skate_close_in(int session);
int halo_skate_preload_in(int session, const char *assets);
int halo_skate_preloading_in(int session);
int halo_skate_load_in(int session, const char *assets, const float *triangles, int count);
int halo_skate_reload_in(int session, const char *assets, const float *triangles, int count);
int halo_skate_state_in(int session);
const char *halo_skate_error_in(int session);
int halo_skate_activate_in(int session, const float *position, float yaw, struct halo_skate_frame *out);
int halo_skate_teleport_in(int session, const float *position, float yaw, const float *velocity,
	struct halo_skate_frame *out);
int halo_skate_apply_impulse_in(int session, float x, float y, float z);
int halo_skate_step_in(int session, const struct halo_skate_pad *pad, float dt, struct halo_skate_frame *out);
int halo_skate_step_begin_in(int session, const struct halo_skate_pad *pad, float dt);
int halo_skate_await_step_in(int session, unsigned int wait_ms, struct halo_skate_frame *out);
void halo_skate_suspend_in(int session);
int halo_skate_set_obstacles_in(int session, const struct halo_skate_obstacle *obstacles, int count,
	const float *triangles, int triangle_count);
int halo_skate_set_skeleton_in(int session, int count, const char *names, const short *parents,
	const float *inverse_defaults);
int halo_skate_pose_nodes_in(int session, float *out, int capacity);
int halo_skate_take_held_hit_in(int session, struct halo_skate_held_hit *out);
int halo_skate_force_bail_in(int session, float closing);
int halo_skate_board_placement_in(int session, float *out);

#endif

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
	float velocity[3]; /* world units a second */
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
/* the map's collision: 0 none, 1 loading, 2 ready, -1 failed (halo_skate_error) */
int halo_skate_state(void);
const char *halo_skate_error(void);
int halo_skate_activate(const float *position, float yaw, struct halo_skate_frame *out);
int halo_skate_step(const struct halo_skate_pad *pad, float dt, struct halo_skate_frame *out);
void halo_skate_suspend(void);
int halo_skate_set_skeleton(int count, const char *names, const short *parents, const float *inverse_defaults);
int halo_skate_pose_nodes(float *out, int capacity);

#endif

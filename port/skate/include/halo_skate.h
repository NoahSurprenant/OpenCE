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

int halo_skate_load(const char *assets, const float *triangles, int count);
int halo_skate_state(void);
const char *halo_skate_error(void);
int halo_skate_activate(const float *position, float yaw, struct halo_skate_frame *out);
int halo_skate_step(const struct halo_skate_pad *pad, float dt, struct halo_skate_frame *out);
void halo_skate_suspend(void);
int halo_skate_set_skeleton(int count, const char *names, const short *parents, const float *inverse_defaults);
int halo_skate_pose_nodes(float *out, int capacity);

#endif

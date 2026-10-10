//! Original82D7B7A0, called with real GroundEnter frame/velocity/body position.
use super::state::ZERO;
use super::{Frame, State, Vector};
#[derive(Clone, Copy, Debug)]
pub struct PlacementInput {
    pub frame: Frame,          //entry0..48
    pub velocity: Vector,      //entry64
    pub body_position: Vector, //entry80, actual skeleton body position
    pub current_state: u32,    //processed2508
    pub previous_state: u32,   //processed2504
    pub previous_frame: Frame, //processed192..240
}
impl State {
    pub fn place(&mut self, i: PlacementInput) {
        // halo-skate: the Biped keeps geometric vectors, w 0, as the host's
        // frames are (IDENTITY, the Rodrigues rotations). The entry frame comes
        // from the skeleton, whose native fourth lanes are kept and need not be
        // 0; carried into frame0 and the frame output, a w that was not 0 grew
        // without bound (ground_motion::support) until a launch packet was NaN.
        let i = PlacementInput {
            frame: i.frame.map(geometric),
            velocity: geometric(i.velocity),
            body_position: geometric(i.body_position),
            previous_frame: i.previous_frame.map(geometric),
            ..i
        };
        if i.current_state == 500 {
            self.reset();
            let delta = sub(self.correction_target_592, i.previous_frame[3]);
            let eligible = i.previous_state == 501
                && !(dot(i.frame[2], delta) <= 0.0)
                && !(dot(i.frame[2], i.previous_frame[2]) <= f32::from_bits(0x3f66_6666));
            if eligible {
                let difference = sub(i.previous_frame[3], self.correction_target_592);
                let amount = dot(i.previous_frame[2], difference);
                self.motion.correction_576 = i.previous_frame[2].map(|v| v * amount);
                self.motion.correction_enabled_711 = true;
            } else {
                self.clear_entry_correction();
            }
        } else if i.current_state == 501 {
            self.clear_entry_correction();
        }
        self.motion.frame_0 = i.frame;
        self.motion.published_frame_64 = i.frame;
        self.frame_output.frame = i.frame;
        self.position_368 = i.body_position;
        self.motion.velocity_480 = i.velocity;
        self.motion.speed_704 = length(i.velocity);
        self.surface.spring_normal = i.frame[1];
        self.surface.spring_delta = ZERO;
    }
    fn clear_entry_correction(&mut self) {
        self.motion.correction_576 = ZERO;
        self.correction_target_592 = ZERO;
        self.motion.correction_enabled_711 = false;
    }
}
fn dot(a: Vector, b: Vector) -> f32 {
    (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]
}
fn sub(a: Vector, b: Vector) -> Vector {
    std::array::from_fn(|i| a[i] - b[i])
}
fn length(v: Vector) -> f32 {
    let q = dot(v, v);
    let mut r = 1.0 / q.sqrt();
    for _ in 0..2 {
        r = (r * 0.5).mul_add((-q).mul_add(r * r, 1.0), r);
    }
    let n = q * r;
    if q == 0.0 { 0.0 } else { n }
}
/// The vector with its fourth (w) lane 0.
fn geometric(mut v: Vector) -> Vector {
    v[3] = 0.0;
    v
}

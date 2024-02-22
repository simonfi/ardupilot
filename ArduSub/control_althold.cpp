#include "Sub.h"


/*
 * control_althold.pde - init and run calls for althold, flight mode
 */

// althold_init - initialise althold controller
bool Sub::althold_init()
{
    if(!control_check_barometer()) {
        return false;
    }

    // initialize vertical maximum speeds and acceleration
    // sets the maximum speed up and down returned by position controller
    attitude_control.set_throttle_out(0.75, true, 100.0);
    pos_control.init_z_controller();
    pos_control.set_max_speed_accel_z(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);
    pos_control.set_correction_speed_accel_z(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);
    attitude_control.relax_attitude_controllers();
    // initialise position and desired velocity
    float pos = stopping_distance();
    float zero = 0;
    pos_control.input_pos_vel_accel_z(pos, zero, zero);

    if(prev_control_mode != control_mode_t::STABILIZE) {
        last_roll = 0;
        last_pitch = 0;
    }
    last_pilot_heading = ahrs.yaw_sensor;
    last_input_ms = AP_HAL::millis();

    return true;
}


float Sub::stopping_distance() {
    const float curr_pos_z = inertial_nav.get_position().z;
    float curr_vel_z = inertial_nav.get_velocity().z;
    float distance = - (curr_vel_z * curr_vel_z) / (2 * g.pilot_accel_z);
    return curr_pos_z  + distance;
}

void Sub::handle_mavlink_attitude_target(){
    uint32_t tnow = AP_HAL::millis();
    float target_roll, target_pitch, target_yaw;
    // Check if set_attitude_target_no_gps is valid
    if (tnow - sub.set_attitude_target_no_gps.last_message_ms < 5000) {
        Quaternion(
            set_attitude_target_no_gps.packet.q
        ).to_euler(
            target_roll,
            target_pitch,
            target_yaw
        );
        target_roll = 100 * degrees(target_roll);
        target_pitch = 100 * degrees(target_pitch);
        target_yaw = 100 * degrees(target_yaw);
        last_roll = target_roll;
        last_pitch = target_pitch;
        last_pilot_heading = target_yaw;
        attitude_control.input_euler_angle_roll_pitch_yaw(target_roll, target_pitch, target_yaw, true);
        return;
    }
}

// althold_run - runs the althold controller
// should be called at 100hz or more
void Sub::althold_run()
{
    // When unarmed, disable motors and stabilization
    if (!motors.armed()) {
        althold_init();
        return;
    }

    handle_attitude();

    control_depth();
}

void Sub::control_depth() {
    // We rotate the RC inputs to the earth frame to check if the user is giving an input that would change the depth.
    // Output the Z controller + pilot input to all motors.
    Vector3f earth_frame_rc_inputs = ahrs.get_rotation_body_to_ned() * Vector3f(-channel_forward->norm_input(), -channel_lateral->norm_input(), (2.0f*(-0.5f+channel_throttle->norm_input())));
    float target_climb_rate_cm_s = get_pilot_desired_climb_rate(500 + g.pilot_speed_up * earth_frame_rc_inputs.z);

    bool surfacing = ap.at_surface || pos_control.get_pos_target_z_cm() > g.surface_depth;
    float upper_speed_limit = surfacing ? 0 : g.pilot_speed_up;
    float lower_speed_limit = ap.at_bottom ? 0 : -get_pilot_speed_dn();
    target_climb_rate_cm_s = constrain_float(target_climb_rate_cm_s, lower_speed_limit, upper_speed_limit);
    pos_control.set_pos_target_z_from_climb_rate_cm(target_climb_rate_cm_s);

    if (surfacing) {
        pos_control.set_alt_target_with_slew(MIN(pos_control.get_pos_target_z_cm(), g.surface_depth - 5.0f)); // set target to 5 cm below surface level
    } else if (ap.at_bottom) {
        pos_control.set_alt_target_with_slew(MAX(inertial_nav.get_altitude() + 10.0f, pos_control.get_pos_target_z_cm())); // set target to 10 cm above bottom
    }
    pos_control.update_z_controller();
    // Read the output of the z controller and rotate it so it always points up
    Vector3f throttle_vehicle_frame = ahrs.get_rotation_body_to_ned().transposed() * Vector3f(0, 0, motors.get_throttle_in_bidirectional());
    //TODO: scale throttle with the ammount of thrusters in the given direction
    float raw_throttle_factor = (ahrs.get_rotation_body_to_ned() * Vector3f(0, 0, 1.0)).xy().length();
    motors.set_throttle(throttle_vehicle_frame.z + raw_throttle_factor * channel_throttle->norm_input());
    motors.set_forward(-throttle_vehicle_frame.x + channel_forward->norm_input());
    motors.set_lateral(-throttle_vehicle_frame.y + channel_lateral->norm_input());
}
// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 *       AP_MotorsMatrix.cpp - ArduCopter motors library
 *       Code by RandyMackay. DIYDrones.com
 *
 *       This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 */
#include <AP_HAL.h>
#include "AP_MotorsMatrix.h"

extern const AP_HAL::HAL& hal;

// Init
void AP_MotorsMatrix::Init()
{
    // call parent Init function to set-up throttle curve
    AP_Motors::Init();

    // setup the motors
    setup_motors();

    // enable fast channels or instant pwm
    set_update_rate(_speed_hz);
}

// set update rate to motors - a value in hertz
void AP_MotorsMatrix::set_update_rate( uint16_t speed_hz )
{
    int8_t i;

    // record requested speed
    _speed_hz = speed_hz;

    // check each enabled motor
    uint32_t mask = 0;
    for( i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++ ) {
        if( motor_enabled[i] ) {
		mask |= 1U << _motor_to_channel_map[i];
        }
    }
    hal.rcout->set_freq( mask, _speed_hz );
}

// set frame orientation (normally + or X)
void AP_MotorsMatrix::set_frame_orientation( uint8_t new_orientation )
{
    // return if nothing has changed
    if( new_orientation == _frame_orientation ) {
        return;
    }

    // call parent
    AP_Motors::set_frame_orientation( new_orientation );

    // setup the motors
    setup_motors();

    // enable fast channels or instant pwm
    set_update_rate(_speed_hz);
}

// enable - starts allowing signals to be sent to motors
void AP_MotorsMatrix::enable()
{
    int8_t i;

    // enable output channels
    for( i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++ ) {
        if( motor_enabled[i] ) {
            hal.rcout->enable_ch(_motor_to_channel_map[i]);
        }
    }
}

// output_min - sends minimum values out to the motors
void AP_MotorsMatrix::output_min()
{
    int8_t i;

    // fill the motor_out[] array for HIL use and send minimum value to each motor
    for( i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++ ) {
        if( motor_enabled[i] ) {
            motor_out[i] = _rc_throttle->radio_min;
            hal.rcout->write(_motor_to_channel_map[i], motor_out[i]);
        }
    }
}

// output_armed - sends commands to the motors
// includes new scaling stability patch
void AP_MotorsMatrix::output_armed()
{
    int8_t i;
    int16_t out_min = _rc_throttle->radio_min + _min_throttle;
    int16_t out_max = _rc_throttle->radio_max;
    int16_t out_mid = (out_min+out_max)/2;
    int16_t out_max_range; // the is the allowable throttle out setting that allowes maximum roll, pitch and yaw range
    float rpy_scale = 1.0; // this is used to scale the roll, pitch and yaw to fit within the motor limits

    int16_t rpy_out[AP_MOTORS_MAX_NUM_MOTORS]; // buffer so we don't have to multiply coefficients multiple times.

    int16_t rpy_low = 0;    // lowest motor value
    int16_t rpy_high = 0;   // highest motor value
    int16_t yaw_allowed;    // amount of yaw we can fit in
    int16_t thr_adj;        // how far we move the throttle point from out_max_range

    // initialize limits flag
    limit.roll_pitch = false;
    limit.yaw = false;
    limit.throttle = false;

    // Throttle is 0 to 1000 only
    // To-Do: we should not really be limiting this here because we don't "own" this _rc_throttle object
    _rc_throttle->servo_out = constrain_int16(_rc_throttle->servo_out, 0, _max_throttle);

    // capture desired roll, pitch, yaw and throttle from receiver
    _rc_roll->calc_pwm();
    _rc_pitch->calc_pwm();
    _rc_throttle->calc_pwm();
    _rc_yaw->calc_pwm();

    // if we are not sending a throttle output, we cut the motors
    if (_rc_throttle->servo_out == 0) {
        // range check spin_when_armed
        if (_spin_when_armed < 0) {
             _spin_when_armed = 0;
        }
        if (_spin_when_armed > _min_throttle) {
            _spin_when_armed = _min_throttle;
        }
        for (i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++) {
            // spin motors at minimum
            if (motor_enabled[i]) {
                motor_out[i] = _rc_throttle->radio_min + _spin_when_armed;
            }
        }

        // Every thing is limited
        limit.roll_pitch = true;
        limit.yaw = true;
        limit.throttle = true;

    } else {

        // check if throttle is below limit
        if (_rc_throttle->radio_out < out_min) {
            limit.throttle = true;
        }

        // calculate roll and pitch for each motor
        for (i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++) {
            if (motor_enabled[i]) {
                rpy_out[i] = _rc_roll->pwm_out * _roll_factor[i] +
                             _rc_pitch->pwm_out * _pitch_factor[i];

                // record lowest roll pitch command
                if (rpy_out[i] < rpy_low) {
                    rpy_low = rpy_out[i];
                }
                // record highest roll pich command
                if (rpy_out[i] > rpy_high) {
                    rpy_high = rpy_out[i];
                }
            }
        }

        // calculate throttle that gives most possible room for yaw (range 1000 ~ 2000)
        // this value is either:
        //      mid throttle - average of highest and lowest motor
        //      the higher of the pilot's throttle input or hover-throttle -- this ensure we never increase the throttle above hover throttle unless the pilot has commanded that
        int16_t motor_mid = (rpy_low+rpy_high)/2;
        out_max_range = min(out_mid - motor_mid, max(_rc_throttle->radio_out, (_rc_throttle->radio_out+_hover_out)/2));

        // calculate amount of yaw we can fit into the throttle range
        // this is always equal to or less than the requested yaw from the pilot or rate controller
        yaw_allowed = min(out_max - out_max_range, out_max_range - out_min) - (rpy_high-rpy_low)/2;
        yaw_allowed = max(yaw_allowed, AP_MOTORS_MATRIX_YAW_LOWER_LIMIT_PWM);

        if (_rc_yaw->pwm_out >= 0) {
            // if yawing right
            if (yaw_allowed > _rc_yaw->pwm_out) {
                yaw_allowed = _rc_yaw->pwm_out; // to-do: this is bad form for yaw_allows to change meaning to become the amount that we are going to output
            }else{
                limit.yaw = true;
            }
        }else{
            // if yawing left
            yaw_allowed = -yaw_allowed;
            if( yaw_allowed < _rc_yaw->pwm_out ) {
                yaw_allowed = _rc_yaw->pwm_out; // to-do: this is bad form for yaw_allows to change meaning to become the amount that we are going to output
            }else{
                limit.yaw = true;
            }
        }

        // add yaw to intermediate numbers for each motor
        for (i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++) {
            if (motor_enabled[i]) {
                rpy_out[i] =    rpy_out[i] +
                                yaw_allowed * _yaw_factor[i];

                // record lowest roll+pitch+yaw command
                if( rpy_out[i] < rpy_low ) {
                    rpy_low = rpy_out[i];
                }
                // record highest roll+pitch+yaw command
                if( rpy_out[i] > rpy_high) {
                    rpy_high = rpy_out[i];
                }
            }
        }

        // check everything fits
        thr_adj = _rc_throttle->radio_out - out_max_range;

        if (thr_adj > 0) {
            // increase throttle as close as possible to requested throttle
            // without going over out_max
            if (thr_adj > out_max-(rpy_high+out_max_range)){
                thr_adj = out_max-(rpy_high+out_max_range);
                // we haven't even been able to apply full throttle command
                limit.throttle = true;
            }
        }else if(thr_adj < 0){
            // decrease throttle as close as possible to requested throttle
            // without going under out_min or over out_max
            // earlier code ensures we can't break both boundaryies
            thr_adj = max(min(thr_adj,out_max-(rpy_high+out_max_range)), min(out_min-(rpy_low+out_max_range),0));
        }

        // do we need to reduce roll, pitch, yaw command
        // earlier code does not allow both limit's to be passed simultainiously with abs(_yaw_factor)<1
        if ((rpy_low+out_max_range)+thr_adj < out_min){
            rpy_scale = (float)(out_min-thr_adj-out_max_range)/rpy_low;
            // we haven't even been able to apply full roll, pitch and minimal yaw without scaling
            limit.roll_pitch = true;
            limit.yaw = true;
        }else if((rpy_high+out_max_range)+thr_adj > out_max){
            rpy_scale = (float)(out_max-thr_adj-out_max_range)/rpy_high;
            // we haven't even been able to apply full roll, pitch and minimal yaw without scaling
            limit.roll_pitch = true;
            limit.yaw = true;
        }

        // add scaled roll, pitch, constrained yaw and throttle for each motor
        for (i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++) {
            if (motor_enabled[i]) {
                motor_out[i] = out_max_range+thr_adj +
                               rpy_scale*rpy_out[i];
            }
        }

        // adjust for throttle curve
        if (_throttle_curve_enabled) {
            for (i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++) {
                if (motor_enabled[i]) {
                    motor_out[i] = _throttle_curve.get_y(motor_out[i]);
                }
            }
        }
        // clip motor output if required (shouldn't be)
        for (i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++) {
            if (motor_enabled[i]) {
                motor_out[i] = constrain_int16(motor_out[i], out_min, out_max);
            }
        }
    }

    // send output to each motor
    for( i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++ ) {
        if( motor_enabled[i] ) {
            hal.rcout->write(_motor_to_channel_map[i], motor_out[i]);
        }
    }
}

// output_disarmed - sends commands to the motors
void AP_MotorsMatrix::output_disarmed()
{
    // Send minimum values to all motors
    output_min();
}

// output_disarmed - sends commands to the motors
void AP_MotorsMatrix::output_test()
{
    uint8_t min_order, max_order;
    uint8_t i,j;

    // find min and max orders
    min_order = _test_order[0];
    max_order = _test_order[0];
    for(i=1; i<AP_MOTORS_MAX_NUM_MOTORS; i++ ) {
        if( _test_order[i] < min_order )
            min_order = _test_order[i];
        if( _test_order[i] > max_order )
            max_order = _test_order[i];
    }

    // shut down all motors
    output_min();

    // first delay is longer
    hal.scheduler->delay(4000);

    // loop through all the possible orders spinning any motors that match that description
    for( i=min_order; i<=max_order; i++ ) {
        for( j=0; j<AP_MOTORS_MAX_NUM_MOTORS; j++ ) {
            if( motor_enabled[j] && _test_order[j] == i ) {
                // turn on this motor and wait 1/3rd of a second
                hal.rcout->write(_motor_to_channel_map[j], _rc_throttle->radio_min + _min_throttle);
                hal.scheduler->delay(300);
                hal.rcout->write(_motor_to_channel_map[j], _rc_throttle->radio_min);
                hal.scheduler->delay(2000);
            }
        }
    }

    // shut down all motors
    output_min();
}

// add_motor
void AP_MotorsMatrix::add_motor_raw(int8_t motor_num, float roll_fac, float pitch_fac, float yaw_fac, uint8_t testing_order)
{
    // ensure valid motor number is provided
    if( motor_num >= 0 && motor_num < AP_MOTORS_MAX_NUM_MOTORS ) {

        // increment number of motors if this motor is being newly motor_enabled
        if( !motor_enabled[motor_num] ) {
            motor_enabled[motor_num] = true;
            _num_motors++;
        }

        // set roll, pitch, thottle factors and opposite motor (for stability patch)
        _roll_factor[motor_num] = roll_fac;
        _pitch_factor[motor_num] = pitch_fac;
        _yaw_factor[motor_num] = yaw_fac;

        // set order that motor appears in test
        _test_order[motor_num] = testing_order;
    }
}

// add_motor using just position and prop direction
void AP_MotorsMatrix::add_motor(int8_t motor_num, float angle_degrees, float yaw_factor, uint8_t testing_order)
{
    // call raw motor set-up method
    add_motor_raw(
        motor_num,
        cosf(radians(angle_degrees + 90)),               // roll factor
        cosf(radians(angle_degrees)),                    // pitch factor
        yaw_factor,                                      // yaw factor
        testing_order);

}

// remove_motor - disabled motor and clears all roll, pitch, throttle factors for this motor
void AP_MotorsMatrix::remove_motor(int8_t motor_num)
{
    // ensure valid motor number is provided
    if( motor_num >= 0 && motor_num < AP_MOTORS_MAX_NUM_MOTORS ) {

        // if the motor was enabled decrement the number of motors
        if( motor_enabled[motor_num] )
            _num_motors--;

        // disable the motor, set all factors to zero
        motor_enabled[motor_num] = false;
        _roll_factor[motor_num] = 0;
        _pitch_factor[motor_num] = 0;
        _yaw_factor[motor_num] = 0;
    }
}

// remove_all_motors - removes all motor definitions
void AP_MotorsMatrix::remove_all_motors()
{
    for( int8_t i=0; i<AP_MOTORS_MAX_NUM_MOTORS; i++ ) {
        remove_motor(i);
    }
    _num_motors = 0;
}

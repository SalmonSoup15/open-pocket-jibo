#pragma once

bool  imu_init();
void  imu_deinit();
void  imu_poll();
float imu_get_yaw_rate();
bool  imu_is_ready();

void  imu_get_orientation(float &pitch, float &roll, float &yaw);
void  imu_get_accel_raw(float &ax, float &ay, float &az);
void  imu_get_gyro_raw(float &gx, float &gy, float &gz);

void  imu_debug_create_ui();
void  imu_debug_render();
void  imu_debug_destroy_ui();

bool  imu_is_calibrated();
void  imu_cal_start();
bool  imu_cal_tick();   // returns true when done
void  imu_cal_abort();


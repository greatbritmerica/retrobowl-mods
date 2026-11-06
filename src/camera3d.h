#pragma once
#include "rb.h"
extern int rb_cam3d_on;
void rb_cam3d_before(CInstance *self);   // before s_update_camera
void rb_cam3d_after(CInstance *self);    // after s_update_camera
int  rb_cam3d_ground(double gui_x, double gui_y, double gui_w, double gui_h, double *rx, double *ry);

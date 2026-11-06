#pragma once
#include "rb.h"
extern int rb_cam3d_on;
void rb_cam3d_before(CInstance *self);   // before s_update_camera
void rb_cam3d_after(CInstance *self);    // after s_update_camera
int  rb_cam3d_ground(double gui_x, double gui_y, double gui_w, double gui_h, double *rx, double *ry);
extern int rb_cam3d_in_gui_pass;
int  rb_cam3d_active(void);
void rb_cam3d_gui_pass(CInstance *self, void (*draw_player)(CInstance *), void (*draw_ball)(CInstance *), void (*draw_text)(CInstance *));
int  rb_cam3d_hit_player(double gx, double gy, double *rx, double *ry);
double rb_cam3d_fwd(void);
void rb_cam3d_rotate_drag(double dx, double dy, double *ox, double *oy);
int  rb_cam3d_stadium(CInstance *self, long sprite, int colour, float sub, float x, float y, float xs, float ys, float alpha);
int  rb_cam3d_handoff_target(double gx, double gy, double *rx, double *ry);

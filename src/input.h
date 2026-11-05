#pragma once
extern int rb_input_log;      // log every change of an input value (diagnostics)
extern unsigned rb_frame;     // ball-update counter
int rb_input_install(void);   // swap the input built-ins; returns 0 if the runtime isn't ready yet

// Virtual finger. While active, every touch/mouse built-in reports this instead of the real screen.
typedef struct {
    int active;               // override real input
    int down, prev_down;      // finger state this frame / last frame (edges for _pressed/_released)
    double gui_x, gui_y;      // device_mouse_x_to_gui / _y_to_gui
    double room_x, room_y;    // mouse_x / mouse_y / device_mouse_x / _y
} VirtualFinger;
extern VirtualFinger rb_vf;
void rb_vf_frame(void);       // call once per game step before the game reads input

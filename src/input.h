#pragma once
extern int rb_input_log;      // log every change of an input value (diagnostics)
extern unsigned rb_frame;     // ball-update counter
int rb_input_install(void);   // swap the input built-ins; returns 0 if the runtime isn't ready yet

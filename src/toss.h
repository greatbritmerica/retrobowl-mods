// toss.h - call the opening coin toss (src/toss.c)
#pragma once
void rb_toss_header(const char *stage_name);     // a commentary stage starts
int  rb_toss_step(void);                         // each outermost commentary step: 1 = hold it
void rb_toss_after_commentary(void);             // after the outermost commentary step

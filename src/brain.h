#pragma once
typedef struct {
    double safe_open, hdef_margin, run_yds;
    int runs;
    double pass_yds;
    int passes, ints, comps, games;
    double def_close;               // how fast defenders close on a thrown ball, px/frame (measured every pass)
    int closes;
    double bias;                    // how much roomier throws turn out than predicted (measured gap - predicted)
    int throwaways, sacks;
} Brain;
extern Brain rb_brain;
enum { BR_COMPLETE, BR_INCOMPLETE, BR_INT };
void rb_brain_load(void);
void rb_brain_pass(int result, double yards, double open, double hdef_dist, int human_jumped);
void rb_brain_run(double yards);
double rb_brain_run_rate(void);
void rb_brain_closing(double rate, double gap, int result);
void rb_brain_calibrate(double predicted, double gap);
void rb_brain_gave_up(int sack);

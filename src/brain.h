#pragma once
typedef struct {
    double safe_open, hdef_margin, run_yds;
    int runs;
    double pass_yds;
    int passes, ints, comps, games;
} Brain;
extern Brain rb_brain;
enum { BR_COMPLETE, BR_INCOMPLETE, BR_INT };
void rb_brain_load(void);
void rb_brain_pass(int result, double yards, double open, double hdef_dist, int human_jumped);
void rb_brain_run(double yards);
double rb_brain_run_rate(void);

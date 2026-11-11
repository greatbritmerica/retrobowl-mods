// rules.h - settings, rules and the play report the player controls from the website (see rules.c)
#pragma once

// What a rule's conditions can look at. The modules that know each value fill it in before firing.
typedef struct {
    double down, togo;          // this play's down and yards to go
    double legs;                // the back's stamina, 0-100
    double run_share;           // how often you've been running lately, 0-100
    double pass_dial, run_dial; // how tight the defense is, 0-100
    double held;                // seconds from the snap to the throw (throw and later events)
    double depth;               // how far downfield the throw was aimed, yards (throw and later)
    double yards;               // what the play gained (play_end and the result events)
    double is_run;              // 1 on a handoff or QB run
    double worked;              // 1 when the play counted as a success (play_end)
    double play_action;         // 1 when play action is on
    double protection;          // 0 normal, 1 max, 2 slide up, 3 slide down
    double scrambled;           // 1 after a scramble
    double quarter, clock;      // quarter 1-4 (5+ overtime); seconds left in it
    double score_diff;          // your score minus theirs
    double goal;                // yards to the end zone you are attacking
} RuleCtx;
extern RuleCtx rb_rctx;

void rb_rules_load(void);                              // re-read rbmod_rules.txt if it changed (once per play)
int  rb_rules_parse(const char *buf);                  // (load's parser; returns unusable lines)
int  rb_rules_count(void);
void rb_rules_new_play(void);
void rb_rules_snapped(void);                           // the play has started (clears messages carried from the last one)
int  rb_rules_fire(const char *ev, const char *arg);   // returns how many rules fired
int  rb_rules_buttons(const char **labels, int max);   // buttons rules asked for this play
int  rb_rules_button_pressed(int i);
void rb_rules_press(int i);
const char *rb_rules_msg(void);                        // text rules want shown before the snap

// effects a rule can cause; implemented by the module that owns the players
void rb_rules_do_legs(char op, double pct);
void rb_rules_do_speed(const char *group, double pct);
void rb_rules_do_play(const char *what, double v);     // "sneak" "kneel" "deep" "push" (v: push strength %)
void rb_rules_do_penalty(double yards, int replay);    // + moves you forward; replay the down or keep the result

void rb_report_line(const char *fmt, ...);             // append one line to rbmod_plays.txt

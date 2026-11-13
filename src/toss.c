// toss.c - call the opening coin toss.
//
// The game has no toss you see: it picks who kicks off before MATCH_START. Right after the MATCH_START
// commentary step the mod holds the commentary (as the Play Defense / Simulate question does) and shows a
// panel: call heads or tails, the coin flips, and the winner chooses. Win: you pick kick or receive. Lose:
// the opponent receives, so you kick. The kicking team is written into the game's possession (the team
// that kicks off at KICKOFF) before the commentary goes on. If the panel can't show, the game's pick stands.
#include "rb.h"
#include "sys.h"
#include "ui.h"
#include "tune.h"
#include "toss.h"

// ---- tunables (Workshop: Coin toss)
#define TOSS_ON           T(T_TOSS_ON)       // 1 = you call the opening toss
#define TOSS_FLIP_SECONDS T(T_TOSS_FLIP)     // seconds the coin spins
#define PANEL_WIDTH       176                // the panel's width on screen (points)

#define SLOT_KICKOFFTEAM 0x100fa6318ULL
enum { TS_IDLE, TS_WANT, TS_ASK, TS_FLIP, TS_RESULT };
static struct { int st, call, side, win; unsigned t; } S;
static volatile int g_ev;                    // panel answer (main thread): 1-based button, -1 = couldn't show
static void cb(int i) { g_ev = i ? i : -1; }
static const char *SIDE[2] = { "HEADS", "TAILS" };

void rb_toss_header(const char *name) {
    if (strcmp(name, "MATCH_START")) return;
    if (S.st != TS_IDLE) rb_ui_panel_close(RB_PANEL_TOSS);
    S.st = TS_IDLE; g_ev = 0;
    double q = rb_get_obj_real(OBJ_CONTROLLER, SLOT_QUARTER);
    if (TOSS_ON && q <= 1) S.st = TS_WANT;                     // the opening only (not a resumed match)
    else if (TOSS_ON) rb_log("toss: MATCH_START in quarter %g - no toss", q);
}

static void set_kicker(double kicker) {
    double poss = rb_get_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION), kt = rb_get_obj_real(OBJ_CONTROLLER, SLOT_KICKOFFTEAM);
    rb_set_obj_real(OBJ_CONTROLLER, SLOT_POSSESSION, kicker);
    // keep the game's record of the opening kicker in step (the second half goes to the other team)
    if (kt == poss) rb_set_obj_real(OBJ_CONTROLLER, SLOT_KICKOFFTEAM, kicker);
    else if (kt == 1 - poss) rb_set_obj_real(OBJ_CONTROLLER, SLOT_KICKOFFTEAM, 1 - kicker);
    rb_log("toss: kicking team %g (the game had picked %g; kickoffteam %g -> %g)", kicker, poss, kt,
           rb_get_obj_real(OBJ_CONTROLLER, SLOT_KICKOFFTEAM));
}

// called at every outermost commentary step; 1 = hold the commentary this frame
int rb_toss_step(void) {
    if (S.st == TS_IDLE || S.st == TS_WANT) return 0;
    int e = g_ev; g_ev = 0;
    double human = rb_get_obj_real(OBJ_MASTER, SLOT_PLAYERTEAMINDEX);
    if (e < 0 || human < 0) {                                 // no panel (or no human team): the game's toss stands
        rb_log("toss: FALLBACK - %s, the game's pick stands", e < 0 ? "the panel couldn't show" : "no human team");
        S.st = TS_IDLE; return 0;
    }
    if (S.st == TS_ASK) {
        if (!e) return 1;
        S.call = e == 2; S.side = (int)arc4random_uniform(2); S.t = 0; S.st = TS_FLIP;
        static const char *none[1];
        rb_ui_panel(RB_PANEL_TOSS, RB_SIDE_LEFT, PANEL_WIDTH, "COIN TOSS", "Flipping...", none, 0, cb);
        rb_log("toss: you called %s", SIDE[S.call]);
        return 1;
    }
    if (S.st == TS_FLIP) {
        if (++S.t < TOSS_FLIP_SECONDS * 60) return 1;
        S.win = S.side == S.call;
        char msg[120];
        if (S.win) {
            snprintf(msg, sizeof msg, "%s - you win the toss!", SIDE[S.side]);
            static const char *b[2] = { "KICK", "RECEIVE" };
            rb_ui_panel(RB_PANEL_TOSS, RB_SIDE_LEFT, PANEL_WIDTH, "COIN TOSS", msg, b, 2, cb);
        } else {
            snprintf(msg, sizeof msg, "It's %s. The opponent won the toss and will receive.", SIDE[S.side]);
            static const char *b[1] = { "OK" };
            rb_ui_panel(RB_PANEL_TOSS, RB_SIDE_LEFT, PANEL_WIDTH, "COIN TOSS", msg, b, 1, cb);
        }
        rb_log("toss: it's %s - you %s", SIDE[S.side], S.win ? "win" : "lose");
        S.st = TS_RESULT;
        return 1;
    }
    if (S.st == TS_RESULT) {
        if (!e) return 1;
        double kicker = S.win && e == 2 ? 1 - human : human;   // RECEIVE: they kick; KICK or a loss: you kick
        if (S.win) rb_log("toss: you chose to %s", e == 2 ? "receive" : "kick");
        set_kicker(kicker);
        S.st = TS_IDLE;
        return 0;
    }
    return 0;
}

// after the outermost commentary step: MATCH_START just ran, ask before KICKOFF
void rb_toss_after_commentary(void) {
    if (S.st != TS_WANT) return;
    static const char *b[2] = { "HEADS", "TAILS" };
    S.st = TS_ASK;
    rb_ui_panel(RB_PANEL_TOSS, RB_SIDE_LEFT, PANEL_WIDTH, "COIN TOSS", "Call it:", b, 2, cb);
    rb_log("toss: asking heads or tails");
}

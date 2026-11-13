// practice.c - weekly practice.
//
// After each game, as the game moves to the next week (its log says "Week=N"), a popup asks for the
// week's focus. A drill raises one rating of each starter in its group through the game's own routine,
// the one the player screen's Add buttons use (s_increase_player_attribute), so the game's caps apply.
// The players are found with the game's own lookup (s_get_player_by_position); the position codes are
// read from the game's position names once (s_get_string_position), and everything is logged.
#include "rb.h"
#include "sys.h"
#include "ui.h"
#include "tune.h"
#include "practice.h"

// ---- tunables (Workshop: Weekly practice)
#define PRACTICE_ON    T(T_PRACTICE_ON)       // 1 = the weekly popup
#define PRACTICE_BOOST T(T_PRACTICE_BOOST)    // rating points per starter in the drilled group
#define MAX_PER_POS    4                      // starters looked up per position

#define VA_GET_PLAYER_BY_POS 0x1003cb24cULL   // s_get_player_by_position(position, nth)
#define VA_STRING_POSITION   0x1003e8810ULL   // s_get_string_position(position) -> "QB" ...
#define VA_INCREASE_ATTR     0x100427930ULL   // s_increase_player_attribute(player, attr)
#define VA_PLAYER_RATING     0x1003cec8cULL   // s_get_player_rating(player): logged before/after
#define VA_GLOBAL_INSTANCE   0x1010c82d8ULL   // the global instance (a safe self for the game's routines)
enum { ATTR_SKILL = 1, ATTR_SPEED = 2, ATTR_STRENGTH = 3, ATTR_STAMINA = 4 };   // the Add buttons' codes

static const struct { const char *button, *pos[3], *rating; int attr; const char *lines[3]; } DRILL[4] = {
    { "Weight room", { "OL", "DL", 0 }, "strength", ATTR_STRENGTH,
      { "Good week in the weight room - the line looks a bit stronger.", "The big men pushed iron all week. The trenches should hold up.",
        "Coach had the linemen under the bar every morning. They're feeling it - in a good way." } },
    { "Tackling", { "DL", "LB", "DB" }, "skill", ATTR_SKILL,
      { "Form tackling all week - wrap up, drive through.", "The defense spent the week on fundamentals. Fewer missed tackles, hopefully.",
        "Pads on, tackling drills, no shortcuts. The defense looks sharper." } },
    { "Running", { "RB", 0, 0 }, "speed", ATTR_SPEED,
      { "The backs ran sprints until they couldn't. A step quicker now.", "Ball security and burst drills for the running backs this week.",
        "Hill runs all week. The backfield has a little more pop." } },
    { "Routes", { "WR", "TE", 0 }, "skill", ATTR_SKILL,
      { "The receivers ran the route tree all week. Crisper breaks.", "Extra reps on timing routes - the passing game should hum.",
        "Hands and routes, over and over. The pass catchers look sharp." } },
};

static volatile int g_pick;           // the popup's answer (main thread), 1-based
static int g_week = -1, g_year = -1, g_shown_week = -1;
static int g_codes_ok; static char g_pos_name[16][8];

static void cb_pick(int i) { g_pick = i ? i : -1; }
static void cb_ok(int i) { (void)i; }

void rb_practice_slog(const char *line) {
    if (!strncmp(line, "Year=", 5)) { sscanf(line + 5, "%d", &g_year); return; }
    if (strncmp(line, "Week=", 5) || !PRACTICE_ON) return;
    if (sscanf(line + 5, "%d", &g_week) != 1) return;
    if (g_week == g_shown_week) return;
    g_shown_week = g_week;
    static const char *b[5] = { "Weight room", "Tackling", "Running", "Routes", "Skip" };
    char title[48];
    snprintf(title, sizeof title, "WEEK %d PRACTICE", g_week);
    rb_ui_choice_n(title, "What's the focus this week?", b, 5, cb_pick);
    rb_log("practice: week %d (year %d) - asking for the focus", g_week, g_year);
}

static void call(CInstance *self, uint64_t va, int argc, RValue *a0, RValue *a1, RValue *res) {
    RValue *av[2] = { a0, a1 };
    memset(res, 0, sizeof *res); res->kind = RV_UNDEFINED;
    ((GMLScript)RB(va))(self, self, res, argc, av);
}
static void rv_free(RValue *v) { ((void (*)(RValue *))RB(VA_RV_FREE))(v); }
static RValue num(double d) { RValue r = {0}; r.kind = RV_REAL; r.real = d; return r; }

static void learn_positions(CInstance *self) {
    if (g_codes_ok) return;
    char line[300]; int n = 0;
    for (int c = 0; c < 16; c++) {
        RValue a = num(c), r; char b[48];
        call(self, VA_STRING_POSITION, 1, &a, 0, &r);
        const char *s = (r.kind & 0xffffff) == RV_STRING ? rb_rv_str(&r, b, sizeof b) : "";
        snprintf(g_pos_name[c], sizeof g_pos_name[c], "%s", s ? s : "");
        rv_free(&r);
        n += snprintf(line + n, sizeof line - n, " %d=%s", c, g_pos_name[c]);
        if (n >= (int)sizeof line - 20) break;
    }
    g_codes_ok = 1;
    rb_log("practice: position codes:%s", line);
}

static void apply(CInstance *self, int d) {
    learn_positions(self);
    void *seen[32]; int nseen = 0, raised = 0, boost = (int)PRACTICE_BOOST;
    for (int k = 0; k < 3 && DRILL[d].pos[k]; k++)
        for (int c = 0; c < 16; c++) {
            if (strcmp(g_pos_name[c], DRILL[d].pos[k])) continue;
            for (int nth = 0; nth < MAX_PER_POS; nth++) {
                RValue a = num(c), b = num(nth), p;
                call(self, VA_GET_PLAYER_BY_POS, 2, &a, &b, &p);
                unsigned kind = p.kind & 0xffffff;
                int dup = 0;
                for (int i = 0; i < nseen; i++) if (seen[i] == p.ptr) dup = 1;
                if (kind != 6 || !p.ptr || dup) {                       // 6: a struct (a player)
                    if (kind != 6 && nth > 0) { rv_free(&p); break; }   // no more at this position
                    rv_free(&p); continue;
                }
                if (nseen < 32) seen[nseen++] = p.ptr;
                RValue r0; call(self, VA_PLAYER_RATING, 1, &p, 0, &r0); double before = rb_rv_real(&r0); rv_free(&r0);
                for (int i = 0; i < boost; i++) {
                    RValue at = num(DRILL[d].attr), r;
                    call(self, VA_INCREASE_ATTR, 2, &p, &at, &r);
                    rv_free(&r);
                }
                raised++;
                RValue r1; call(self, VA_PLAYER_RATING, 1, &p, 0, &r1); double after = rb_rv_real(&r1); rv_free(&r1);
                rb_log("practice: %s %d (code %d, lookup %d) +%d %s - rating %g -> %g", DRILL[d].pos[k], raised, c, nth, boost, DRILL[d].rating, before, after);
                rv_free(&p);
            }
        }
    char msg[200];
    if (raised) snprintf(msg, sizeof msg, "%s", DRILL[d].lines[arc4random_uniform(3)]);
    else snprintf(msg, sizeof msg, "Practice went fine, but no one in that group could be found - nothing changed.");
    rb_log("practice: %s - %d players +%d %s", DRILL[d].button, raised, boost, DRILL[d].rating);
    static const char *ok[1] = { "OK" };
    rb_ui_choice_n(DRILL[d].button, msg, ok, 1, cb_ok);
}

// on the game thread (any frame): carry out the pick
void rb_practice_tick(CInstance *drawing) {
    (void)drawing;
    if (!g_pick) return;
    CInstance *self = *(CInstance **)RB(VA_GLOBAL_INSTANCE);   // not whoever is drawing
    if (!self) return;
    int p = g_pick; g_pick = 0;
    if (p < 1 || p > 4) { rb_log("practice: %s", p == 5 ? "skipped this week" : "popup failed - no practice"); return; }
    apply(self, p - 1);
}

// ocbrain_test.c - host test for src/ocbrain.c, run by the GitHub build:
//   cc -o build/ocbrain_test tests/ocbrain_test.c src/ocbrain.c -lm && build/ocbrain_test
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/ocbrain.h"

static unsigned long seed = 12345;
static double rnd(void) { seed = seed * 6364136223846793005UL + 1442695040888963407UL; return (seed >> 11) * (1.0 / 9007199254740992.0); }

static OcCtx ctx0(void) { OcCtx c; memset(&c, 0, sizeof c); c.counter = 1; return c; }
static const int NONE[7] = {0};

int main(void) {
    OcBrain b; OcCtx c = ctx0(); int why = -1;

    // situation_buckets
    assert(oc_situation(0) == 0 && oc_situation(3) == 0 && oc_situation(4) == 1);
    assert(oc_situation(7) == 1 && oc_situation(8) == 2 && oc_situation(25) == 2);

    // learned_route_wins: slant up top on medium yardage, 8 of 10
    memset(&b, 0, sizeof b);
    b.att[1][OC_TOP][1] = 10; b.ok[1][OC_TOP][1] = 8;
    assert(oc_pick(&b, OC_TOP, 0, 1, &c, NONE, rnd, &why) == 1 && why == OC_WHY_WORKS);

    // long_skips_short: a perfect slant still isn't called on 8+ to go
    memset(&b, 0, sizeof b);
    b.att[1][OC_TOP][2] = 10; b.ok[1][OC_TOP][2] = 10;
    int r = oc_pick(&b, OC_TOP, 0, 2, &c, NONE, rnd, &why);
    assert(r != 0 && r != 1);

    // unseen_beats_familiar: the defense knows every zone but the slant's (unique to it from the top)
    memset(&b, 0, sizeof b);
    for (int z = 0; z < 9; z++) c.fam[z] = 1;
    c.fam[oc_zone(1, 0)] = 0;
    assert(oc_pick(&b, OC_TOP, 0, 1, &c, NONE, rnd, &why) == 1 && why == OC_WHY_UNSEEN);
    c = ctx0();

    // why_new: no data and no defense memory -> just trying it
    memset(&b, 0, sizeof b);
    oc_pick(&b, OC_TOP, 0, 1, &c, NONE, rnd, &why);
    assert(why == OC_WHY_NEW);

    // why_follows_familiarity: a route that went 0 for 2 but is the only one the defense doesn't know
    memset(&b, 0, sizeof b);
    b.att[1][OC_TOP][1] = 2;
    for (int z = 0; z < 9; z++) c.fam[z] = 1;
    c.fam[oc_zone(1, 0)] = 0;
    assert(oc_pick(&b, OC_TOP, 0, 1, &c, NONE, rnd, &why) == 1 && why == OC_WHY_UNSEEN);
    c = ctx0();

    // surprise_rate
    c.surprise = 0.2; int n = 0;
    for (int i = 0; i < 10000; i++) { oc_pick(&b, OC_MID, 1, 1, &c, NONE, rnd, &why); n += why == OC_WHY_SURPRISE; }
    assert(n >= 1800 && n <= 2200);
    c = ctx0();

    // keep_rate
    c.keep = 0.15; n = 0;
    for (int i = 0; i < 10000; i++) n += oc_pick(&b, OC_MID, 1, 1, &c, NONE, rnd, &why) == OC_KEEP;
    assert(n >= 1300 && n <= 1700);
    c = ctx0();

    // no_duplicates: the best route is taken by another receiver
    memset(&b, 0, sizeof b);
    b.att[1][OC_TOP][1] = 10; b.ok[1][OC_TOP][1] = 8;
    int used[7] = {0}; used[1] = 1;
    assert(oc_pick(&b, OC_TOP, 0, 1, &c, used, rnd, &why) != 1);

    // full_used_still_picks
    for (int i = 0; i < 7; i++) used[i] = 1;
    r = oc_pick(&b, OC_BOT, 2, 2, &c, used, rnd, &why);
    assert(r >= 0 && r <= 6);

    // fade_scales
    memset(&b, 0, sizeof b); b.att[3][OC_MID][0] = 10;
    oc_fade(&b, 0.9);
    assert(fabs(b.att[3][OC_MID][0] - 9) < 1e-9);

    // learn counts
    memset(&b, 0, sizeof b);
    oc_learn(&b, 2, OC_BOT, 1, 1, 12.5); oc_learn(&b, 2, OC_BOT, 1, 0, -1);
    assert(b.att[2][OC_BOT][1] == 2 && b.ok[2][OC_BOT][1] == 1 && fabs(b.yds[2][OC_BOT][1] - 11.5) < 1e-9);

    // file_roundtrip
    OcBrain b2; FILE *f = tmpfile();
    b.att[5][OC_TOP][2] = 3.25; b.att[6][OC_MID][0] = 1; b.ok[6][OC_MID][0] = 0.5;
    assert(oc_save(&b, f)); rewind(f);
    assert(oc_load(&b2, f)); fclose(f);
    for (int i = 0; i < 7; i++) for (int p = 0; p < 3; p++) for (int s = 0; s < 3; s++)
        assert(fabs(b.att[i][p][s] - b2.att[i][p][s]) < 1e-6 && fabs(b.ok[i][p][s] - b2.ok[i][p][s]) < 1e-6 && fabs(b.yds[i][p][s] - b2.yds[i][p][s]) < 1e-6);

    // load_rejects_junk
    f = tmpfile(); fputs("junk\n", f); rewind(f);
    assert(!oc_load(&b2, f)); fclose(f);
    for (int i = 0; i < 7; i++) assert(b2.att[i][0][0] == 0 && b2.ok[i][2][2] == 0);
    f = tmpfile(); fputs("ocbrain 10\n", f); for (int i = 0; i < 63; i++) fputs("0 0 0\n", f); rewind(f);   // other version
    assert(!oc_load(&b2, f)); fclose(f);
    f = tmpfile(); fputs("ocbrain 1\n-2 0 0\n", f); for (int i = 1; i < 63; i++) fputs("0 0 0\n", f); rewind(f);   // negative count
    assert(!oc_load(&b2, f)); fclose(f);
    f = tmpfile(); fputs("ocbrain 1\nnan 0 0\n", f); for (int i = 1; i < 63; i++) fputs("0 0 0\n", f); rewind(f);  // not a number
    assert(!oc_load(&b2, f)); fclose(f);
    f = tmpfile(); fputs("ocbrain 1\n1 1 1\n", f); rewind(f);   // cut short
    assert(!oc_load(&b2, f)); fclose(f);
    assert(b2.att[0][0][0] == 0);

    // table_set_updates / table_find_missing
    OcTable t; int p1, p2;
    oc_table_reset(&t);
    oc_table_set(&t, &p1, 2, OC_TOP); oc_table_set(&t, &p1, 5, OC_TOP);
    assert(t.n == 1 && t.route[oc_table_find(&t, &p1)] == 5);
    assert(oc_table_find(&t, &p2) == -1 && oc_table_find(&t, 0) == -1);

    puts("ocbrain_test ok");
    return 0;
}

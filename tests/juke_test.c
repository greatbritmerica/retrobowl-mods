// juke_test.c - host test for src/juke.c, run by the GitHub build:
//   cc -o build/juke_test tests/juke_test.c src/juke.c -lm && build/juke_test
#include <assert.h>
#include <stdio.h>
#include "../src/juke.h"

int main(void) {
    assert(rb_juke_closing(0, 0, 2, 0, 40, 0, -2, 0, 51) == 1);    // head-on
    assert(rb_juke_closing(0, 0, 2, 0, 80, 0, -2, 0, 51) == 0);    // out of reach
    assert(rb_juke_closing(0, 0, 3, 0, -30, 0, 2, 0, 51) == 0);    // chasing from behind, slower: gap growing
    assert(rb_juke_closing(0, 0, 3, 0, -30, 0, 4, 0, 51) == 1);    // chasing from behind, faster
    assert(rb_juke_closing(0, 0, 2, 0, 0, 30, 2, 0, 51) == 0);     // side by side, same speed
    puts("juke_test ok");
    return 0;
}

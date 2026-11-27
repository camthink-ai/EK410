/*
 * Host test for main/button_gesture.c (MAIN+PAGE hold ladder).
 *
 * Release after ~2.5 s → power off; hold past ~6 s → factory (no release).
 * SoftAP stays on MAIN alone and is not covered here.
 *
 * Built and run by tools/run_host_tests.py.
 */
#include <stdio.h>
#include "button_gesture.h"

static int n_cases, n_failed;

static void expect(const char *what, btn_both_action_t got, btn_both_action_t want)
{
    n_cases++;
    if (got != want) {
        n_failed++;
        printf("FAIL %-48s got=%d want=%d\n", what, (int)got, (int)want);
    }
}

int main(void)
{
    /* Still holding — below factory. */
    expect("held 0 ms", btn_both_while_held(0), BTN_BOTH_NONE);
    expect("held 2499 ms", btn_both_while_held(2499), BTN_BOTH_NONE);
    expect("held 2500 ms still holding", btn_both_while_held(2500), BTN_BOTH_NONE);
    expect("held 5999 ms still holding", btn_both_while_held(5999), BTN_BOTH_NONE);
    expect("held 6000 ms → factory", btn_both_while_held(6000), BTN_BOTH_FACTORY);
    expect("held 7000 ms → factory", btn_both_while_held(7000), BTN_BOTH_FACTORY);

    /* Release before shutdown floor. */
    expect("release 0 ms", btn_both_on_release(0, 0), BTN_BOTH_NONE);
    expect("release 2499 ms", btn_both_on_release(2499, 0), BTN_BOTH_NONE);

    /* Release in the shutdown window. */
    expect("release 2500 ms → shutdown", btn_both_on_release(2500, 0), BTN_BOTH_SHUTDOWN);
    expect("release 5999 ms → shutdown", btn_both_on_release(5999, 0), BTN_BOTH_SHUTDOWN);

    /* Factory already took the gesture — release is a no-op. */
    expect("release after factory", btn_both_on_release(6000, 1), BTN_BOTH_NONE);
    expect("release 3s after factory flag", btn_both_on_release(3000, 1), BTN_BOTH_NONE);

    /* Edge: release exactly at factory ms without the while_held path having
     * fired yet still must not power-off (factory owns ≥6 s). */
    expect("release 6000 ms without factory flag", btn_both_on_release(6000, 0),
           BTN_BOTH_NONE);

    printf("RESULT: %s (%d cases)\n", n_failed ? "fail" : "pass", n_cases);
    return n_failed ? 1 : 0;
}

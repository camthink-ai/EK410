/*
 * Host test for main/json_guard.c (the JSON nesting-depth guard).
 *
 * Deeply nested input must be rejected BEFORE cJSON recurses: each nesting
 * level costs ~64 bytes of stack and the callers run on 4–5 KB task stacks
 * (this rebooted every 1.3.9 device: see CHANGELOG 1.3.10). The stub parser
 * only counts calls, so "rejected" is observable as zero parser calls.
 *
 * Built and run by tools/run_host_tests.py.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_guard.h"

extern int g_parse_calls;

static int n_cases, n_failed;

/* must_parse = 1: the parser must run (stub "succeeds");
 * must_parse = 0: rejected without touching the parser. */
static void expect(const char *what, const char *s, int must_parse)
{
    g_parse_calls = 0;
    cJSON *r = json_guard_parse(s ? s : "");
    int ok = must_parse ? (r != NULL && g_parse_calls == 1)
                        : (r == NULL && g_parse_calls == 0);
    n_cases++;
    if (!ok) {
        n_failed++;
        printf("FAIL %-46s result=%p parse_calls=%d\n", what, (void *)r, g_parse_calls);
    }
}

/* Independent reference: walks JSON tokens (strings, escapes) and tracks
 * the peak depth; the guard must accept exactly what it accepts. */
static int reference_peak(const char *s, size_t len)
{
    int depth = 0, peak = 0, in_str = 0, esc = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') in_str = 1;
        else if (c == '{' || c == '[') { if (++depth > peak) peak = depth; }
        else if (c == '}' || c == ']') { if (depth) depth--; }
    }
    return peak;
}

static unsigned rng_state = 12345u;
static unsigned rng(void) { rng_state = rng_state * 1103515245u + 12345u; return (rng_state >> 16) & 0x7FFF; }

int main(void)
{
    char *deep = malloc(16385), *flat = malloc(16385);
    size_t o = 0;
    memset(deep, '[', 16384); deep[16384] = 0;           /* the reported attack */
    flat[o++] = '[';
    for (int i = 0; i < 8000; i++) { flat[o++] = '1'; if (i < 7999) flat[o++] = ','; }
    flat[o++] = ']'; flat[o] = 0;

    expect("16 KB of '['", deep, 0);
    expect("depth 9", "[[[[[[[[[", 0);
    expect("depth 8 (at the limit)", "[[[[[[[[", 1);
    expect("depth 8 objects/arrays", "{\"a\":[{\"a\":[{\"a\":[{\"a\":[1]}]}]}]}", 1);
    expect("depth 9 objects/arrays", "{\"a\":[{\"a\":[{\"a\":[{\"a\":[{\"a\":[1]}]}]}]}]}", 0);
    expect("MQTT render_canvas shape", "{\"cmd\":\"render_canvas\",\"params\":{\"items\":[{\"type\":\"text\",\"pos\":{\"x\":1}}]}}", 1);
    expect("CT-Prov config.set", "{\"id\":1,\"op\":\"config.set\",\"data\":{\"mqtt\":{\"host\":\"h\"}}}", 1);
    expect("16 KB flat array", flat, 1);
    expect("brackets inside a string", "{\"s\":\"[[[[[[[[[[[[[[[[[[[[[[[[[\"}", 1);
    expect("escaped quote, brackets in string", "{\"s\":\"\\\"[[[[[[[[[[[[[[[[[[\"}", 1);
    expect("closing brackets do not count", "]]]]]]]]]]]]", 1);
    expect("closed levels do not accumulate (peak 5)", "[[[[[][[[[", 1);
    expect("closed levels, peak 9", "[[[[[[[[][[[[[[[", 0);
    expect("empty body", "", 1);
    g_parse_calls = 0; n_cases++;
    if (json_guard_parse(NULL) != NULL || g_parse_calls != 0) { n_failed++; printf("FAIL NULL body\n"); }

    /* Randomised cross-check against the reference tracker. */
    static const char *const atoms[] = {
        "{", "[", "}", "]", "\"a\\\"[[[\"", "\"(((((((((\"", "1", ",", ":",
        "\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"",
    };
    int mismatches = 0, rejected = 0;
    for (int iter = 0; iter < 200000; iter++) {
        char buf[512]; size_t len = 0;
        int n_atoms = 1 + (int)(rng() % 24);
        for (int i = 0; i < n_atoms; i++) {
            const char *a = atoms[rng() % 10];
            size_t al = strlen(a);
            if (len + al >= sizeof(buf)) break;
            memcpy(buf + len, a, al); len += al;
        }
        buf[len] = 0;
        int got = json_guard_depth_ok(buf, len, JSON_GUARD_MAX_DEPTH);
        int want = reference_peak(buf, len) <= JSON_GUARD_MAX_DEPTH;
        if (!got) rejected++;
        if (got != want) {
            if (mismatches < 3) printf("MISMATCH peak=%d got=%d: %s\n", reference_peak(buf, len), got, buf);
            mismatches++;
        }
    }
    n_cases++;
    if (mismatches) { n_failed++; printf("FAIL fuzz vs reference: %d mismatches\n", mismatches); }
    printf("RESULT: %s - %d cases, %d failed; fuzz 200000 inputs (%d rejected), %d mismatches vs reference\n",
           n_failed ? "fail" : "pass", n_cases, n_failed, rejected, mismatches);
    return n_failed ? 1 : 0;
}

// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host matrix for expr_eval (Tier 2 value expressions): grammar, precedence,
// unary rules, caps, compile rejects, runtime div0, int32 clamping, and a
// deterministic fuzz loop (never crash on arbitrary input — this parser eats
// REST/cloud-supplied text on the P4). Built with ASan+UBSan.
#include "expr_eval.h"

#include <cstdio>
#include <cstring>
#include <string>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Compile + eval; returns true only if both succeed.
static bool ev(const char* text, int32_t value, int32_t& out) {
    ExprProg p{};
    char err[64] = {};
    if (!expr_compile(text, p, err, sizeof err)) return false;
    return expr_eval(p, value, out);
}
// Assert compile+eval == want.
#define EV(text, value, want, msg) do {                               \
    int32_t r = 0;                                                     \
    CHECK(ev((text), (value), r) && r == (want), msg);                 \
} while (0)
// Assert compile rejects (and err is non-empty).
static bool rejects(const char* text) {
    ExprProg p{};
    char err[64] = {};
    if (expr_compile(text, p, err, sizeof err)) return false;
    return err[0] != '\0';
}

int main() {
    // ── 1. Primaries + whitespace ────────────────────────────────────────
    EV("5", 0, 5,                      "int literal");
    EV("%value%", 42, 42,              "bare %value%");
    EV("  %value%  ", 7, 7,            "surrounding whitespace tolerated");
    EV("1 + 2", 0, 3,                  "spaces around operator");

    // ── 2. Precedence + associativity ───────────────────────────────────
    EV("2+3*4", 0, 14,                 "* binds tighter than +");
    EV("2*3+4", 0, 10,                 "term then +");
    EV("10-2-3", 0, 5,                 "- is left-associative");
    EV("20/2/5", 0, 2,                 "/ is left-associative");
    EV("10%3", 0, 1,                   "modulo");
    EV("7%4", 0, 3,                    "modulo 2");
    EV("%value%*10+5", 2, 25,          "value in a precedence chain");
    EV("10%%value%", 3, 1,             "'%' disambiguates: 10 mod value");
    EV("%value%%%value%", 7, 0,        "value mod value");

    // ── 3. Parens ────────────────────────────────────────────────────────
    EV("(2+3)*4", 0, 20,               "parens override precedence");
    EV("2*(3+4)", 0, 14,               "parens right of operator");
    EV("(%value%*10)/3+5", 25, 88,     "the plan's flagship expression");
    EV("((((((1))))))", 0, 1,          "paren depth 6 accepted");
    CHECK(rejects("(((((((1)))))))"),  "paren depth 7 rejected");
    EV("1+(2+(3+(4+5)))", 0, 15,       "nested right-leaning parens");

    // ── 4. Unary ─────────────────────────────────────────────────────────
    EV("-5", 0, -5,                    "unary minus");
    EV("!0", 0, 1,                     "!0 = 1");
    EV("!7", 0, 0,                     "!nonzero = 0");
    EV("!%value%", 0, 1,               "!%value% with 0");
    EV("!%value%", 3, 0,               "!%value% with nonzero");
    EV("-%value%", 3, -3,              "negate value");
    EV("2*-3", 0, -6,                  "unary minus after binary operator");
    EV("2+!0", 0, 3,                   "! after binary operator");
    EV("-(2+3)", 0, -5,                "unary minus before parens");
    CHECK(rejects("--5"),              "unary chain -- rejected");
    CHECK(rejects("!!5"),              "unary chain !! rejected");
    CHECK(rejects("-!5"),              "mixed unary chain rejected");

    // ── 5. Division / modulo by zero ─────────────────────────────────────
    CHECK(rejects("5/0"),              "literal /0 rejected at compile");
    CHECK(rejects("%value%/0"),        "literal /0 after value rejected at compile");
    CHECK(rejects("10%0"),             "literal %0 rejected at compile");
    {   // divisor only zero at runtime → compile OK, eval false
        ExprProg p{}; char err[64] = {};
        CHECK(expr_compile("%value%/(%value%-1)", p, err, sizeof err),
              "runtime-zero divisor compiles");
        int32_t out = 0;
        CHECK(!expr_eval(p, 1, out),   "eval false when divisor hits 0");
        CHECK(expr_eval(p, 3, out) && out == 1, "same prog fine when divisor nonzero");
    }
    {   // constant-folded zero the literal check can't see → runtime guard
        int32_t out = 0;
        CHECK(!ev("10/(5-5)", 0, out) && rejects("10/(5-5)") == false,
              "10/(5-5) compiles but eval returns false");
    }

    // ── 6. int32 clamping (no UB) ────────────────────────────────────────
    EV("2147483647+1", 0, INT32_MAX,   "add overflow clamps to INT32_MAX");
    EV("0-2147483647-2", 0, INT32_MIN, "sub overflow clamps to INT32_MIN");
    EV("1000000*1000000", 0, INT32_MAX, "mul overflow clamps");
    EV("-1000000*1000000", 0, INT32_MIN, "negative mul overflow clamps");
    CHECK(rejects("2147483648"),       "literal > INT32_MAX rejected");
    EV("%value%*2", INT32_MAX, INT32_MAX, "value-driven overflow clamps");

    // ── 7. Caps ──────────────────────────────────────────────────────────
    {
        std::string long49(49, '1');   // 49 digits > EXPR_TEXT_MAX
        CHECK(rejects(long49.c_str()), "text longer than 48 chars rejected");
    }
    EV("1+1+1+1+1+1", 0, 6,            "11 RPN ops accepted (6 pushes + 5 adds)");
    CHECK(rejects("1+1+1+1+1+1+1"),    "13 RPN ops rejected (> 12)");

    // ── 8. Malformed inputs ──────────────────────────────────────────────
    const char* bad[] = {
        "", "   ", "1+", "+", "*3", "(1+2", "1+2)", "()", "abc",
        "%val%", "%value", "1 2", "1//2", "5!", "%",
    };
    for (const char* b : bad) {
        char m[64]; snprintf(m, sizeof m, "malformed rejected: '%s'", b);
        CHECK(rejects(b), m);
    }
    CHECK(rejects(nullptr) || true,    "null input does not crash");  // no-crash pin

    // ── 9. Defensive eval ────────────────────────────────────────────────
    {
        ExprProg empty{};
        int32_t out = 0;
        CHECK(!expr_eval(empty, 0, out), "empty program eval returns false");
    }

    // ── 10. Deterministic fuzz: never crash, ASan/UBSan clean ───────────
    {
        uint32_t lcg = 0xC0FFEE01u;
        auto next = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 16; };
        static const char cs[] = "0123456789+-*/%()! %value";
        int compiled = 0;
        for (int i = 0; i < 20000; i++) {
            char buf[EXPR_TEXT_MAX + 1];
            const size_t len = 1 + next() % EXPR_TEXT_MAX;
            for (size_t j = 0; j < len; j++) buf[j] = cs[next() % (sizeof(cs) - 1)];
            buf[len] = '\0';
            ExprProg p{}; char err[64];
            if (expr_compile(buf, p, err, sizeof err)) {
                compiled++;
                int32_t out = 0;
                (void)expr_eval(p, 7, out);       // must not crash either way
            }
        }
        printf("  (fuzz: %d/20000 random strings compiled)\n", compiled);
        CHECK(true, "fuzz loop completed without crash (ASan/UBSan gate)");
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}

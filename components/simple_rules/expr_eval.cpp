// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// expr_eval — integer expression compiler/evaluator for rule action values.
// Non-recursive shunting-yard → RPN at rule-add; fixed-stack walk per fire.
// See include/expr_eval.h for the grammar + caps and
// extra/docs/2026-07-08-simple-rules-expr-tier2-plan.md for the design.
//
// Security posture: this parses REST/cloud-supplied text on the P4, so it is
// deliberately non-recursive, allocation-free, and every cap is a hard reject.
// All arithmetic runs in int64 and clamps to int32 — no UB on any input.
#include "expr_eval.h"

#include <cstdio>
#include <cstring>

namespace {

void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "error");
}

int32_t clamp64(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(v);
}

// Operator-stack entries: '+' '-' '*' '/' '%' binary, 'N' unary minus,
// '!' logical not, '(' group marker.
int prec(char op) {
    switch (op) {
        case 'N': case '!':           return 3;
        case '*': case '/': case '%': return 2;
        case '+': case '-':           return 1;
        default:                      return 0;  // '('
    }
}

uint8_t op_code(char c) {
    switch (c) {
        case '+': return EXPR_ADD;
        case '-': return EXPR_SUB;
        case '*': return EXPR_MUL;
        case '/': return EXPR_DIV;
        case '%': return EXPR_MOD;
        case 'N': return EXPR_NEG;
        default:  return EXPR_NOT;   // '!'
    }
}

// RPN emitter with op-count + simulated-eval-stack-depth enforcement. A
// program that passes compile can never over/underflow the eval stack.
struct Emitter {
    ExprProg*   p;
    int         depth = 0;
    const char* fail  = nullptr;

    bool emit(uint8_t code, int32_t imm = 0) {
        if (p->count >= EXPR_OPS_MAX) {
            fail = "expression too complex (op limit)";
            return false;
        }
        if (code == EXPR_PUSH_IMM || code == EXPR_PUSH_VAL) {
            if (depth >= EXPR_STACK_MAX) { fail = "expression too deep"; return false; }
            depth++;
        } else if (code == EXPR_NEG || code == EXPR_NOT) {
            if (depth < 1) { fail = "misplaced operator"; return false; }
        } else {                              // binary
            if (depth < 2) { fail = "misplaced operator"; return false; }
            depth--;
        }
        p->ops[p->count].code = code;
        p->ops[p->count].imm  = imm;
        p->count++;
        return true;
    }
};

}  // namespace

bool expr_compile(const char* text, ExprProg& out, char* err, size_t err_cap) {
    out.count = 0;
    if (!text) { set_err(err, err_cap, "null expression"); return false; }
    if (strnlen(text, EXPR_TEXT_MAX + 1) > EXPR_TEXT_MAX) {
        set_err(err, err_cap, "expression too long (max 48)");
        return false;
    }

    Emitter em{&out};
    char opstk[EXPR_TEXT_MAX];
    int  osp   = 0;
    int  paren = 0;
    bool expect_operand = true;   // token state machine: operand vs operator
    bool any_token      = false;
    bool pending_unary  = false;  // last token was a unary op (reject chains)

    // Pop one operator off the stack into the program. Rejects '/' and '%'
    // whose right operand is the LITERAL zero just emitted (the common typo);
    // a divisor that only evaluates to zero is caught at eval time.
    auto pop_emit = [&](char op) -> bool {
        if ((op == '/' || op == '%') && out.count > 0) {
            const ExprOp& prev = out.ops[out.count - 1];
            if (prev.code == EXPR_PUSH_IMM && prev.imm == 0) {
                em.fail = "division by zero";
                return false;
            }
        }
        return em.emit(op_code(op));
    };

    for (const char* s = text; *s; ) {
        if (*s == ' ' || *s == '\t') { s++; continue; }
        any_token = true;

        if (expect_operand) {
            if (*s >= '0' && *s <= '9') {
                int64_t v = 0;
                while (*s >= '0' && *s <= '9') {
                    v = v * 10 + (*s - '0');
                    if (v > INT32_MAX) {
                        set_err(err, err_cap, "integer literal out of range");
                        return false;
                    }
                    s++;
                }
                if (!em.emit(EXPR_PUSH_IMM, static_cast<int32_t>(v))) {
                    set_err(err, err_cap, em.fail); return false;
                }
                expect_operand = false;
                pending_unary  = false;
                continue;
            }
            if (strncmp(s, "%value%", 7) == 0) {
                if (!em.emit(EXPR_PUSH_VAL)) { set_err(err, err_cap, em.fail); return false; }
                s += 7;
                expect_operand = false;
                pending_unary  = false;
                continue;
            }
            if (*s == '-' || *s == '!') {              // unary (at most one)
                if (pending_unary) {
                    set_err(err, err_cap, "chained unary operators");
                    return false;
                }
                if (osp >= static_cast<int>(sizeof opstk)) {
                    set_err(err, err_cap, "expression too complex");
                    return false;
                }
                opstk[osp++] = (*s == '-') ? 'N' : '!';
                pending_unary = true;
                s++;
                continue;                              // still expecting an operand
            }
            if (*s == '(') {
                if (++paren > EXPR_PAREN_MAX) {
                    set_err(err, err_cap, "parentheses nested too deep");
                    return false;
                }
                if (osp >= static_cast<int>(sizeof opstk)) {
                    set_err(err, err_cap, "expression too complex");
                    return false;
                }
                opstk[osp++] = '(';
                pending_unary = false;
                s++;
                continue;
            }
            set_err(err, err_cap, "expected a value");
            return false;
        }

        // Expecting a binary operator or ')'.
        if (*s == '+' || *s == '-' || *s == '*' || *s == '/' || *s == '%') {
            const char op = *s;
            while (osp > 0 && opstk[osp - 1] != '(' && prec(opstk[osp - 1]) >= prec(op)) {
                if (!pop_emit(opstk[--osp])) { set_err(err, err_cap, em.fail); return false; }
            }
            if (osp >= static_cast<int>(sizeof opstk)) {
                set_err(err, err_cap, "expression too complex");
                return false;
            }
            opstk[osp++] = op;
            expect_operand = true;
            pending_unary  = false;
            s++;
            continue;
        }
        if (*s == ')') {
            if (paren == 0) { set_err(err, err_cap, "unbalanced ')'"); return false; }
            while (osp > 0 && opstk[osp - 1] != '(') {
                if (!pop_emit(opstk[--osp])) { set_err(err, err_cap, em.fail); return false; }
            }
            if (osp == 0) { set_err(err, err_cap, "unbalanced ')'"); return false; }
            osp--;                                     // pop '('
            paren--;
            s++;
            continue;                                  // still after-operand
        }
        set_err(err, err_cap, "expected an operator");
        return false;
    }

    if (!any_token)     { set_err(err, err_cap, "empty expression"); return false; }
    if (expect_operand) { set_err(err, err_cap, "expression ends with an operator"); return false; }
    while (osp > 0) {
        const char op = opstk[--osp];
        if (op == '(') { set_err(err, err_cap, "unbalanced '('"); return false; }
        if (!pop_emit(op)) { set_err(err, err_cap, em.fail); return false; }
    }
    if (em.depth != 1 || out.count == 0) {
        set_err(err, err_cap, "malformed expression");
        return false;
    }
    return true;
}

bool expr_eval(const ExprProg& p, int32_t value, int32_t& out) {
    if (p.count == 0 || p.count > EXPR_OPS_MAX) return false;
    int64_t st[EXPR_STACK_MAX];
    int sp = 0;
    for (uint8_t i = 0; i < p.count; i++) {
        const ExprOp& op = p.ops[i];
        switch (op.code) {
            case EXPR_PUSH_IMM:
                if (sp >= EXPR_STACK_MAX) return false;
                st[sp++] = op.imm;
                break;
            case EXPR_PUSH_VAL:
                if (sp >= EXPR_STACK_MAX) return false;
                st[sp++] = value;
                break;
            case EXPR_NEG:
                if (sp < 1) return false;
                st[sp - 1] = clamp64(-st[sp - 1]);
                break;
            case EXPR_NOT:
                if (sp < 1) return false;
                st[sp - 1] = (st[sp - 1] == 0) ? 1 : 0;
                break;
            default: {
                if (sp < 2) return false;
                const int64_t b = st[--sp];
                const int64_t a = st[sp - 1];
                int64_t r;
                switch (op.code) {
                    case EXPR_ADD: r = a + b; break;
                    case EXPR_SUB: r = a - b; break;
                    case EXPR_MUL: r = a * b; break;   // clamped int32 operands: no int64 overflow
                    case EXPR_DIV: if (b == 0) return false; r = a / b; break;
                    case EXPR_MOD: if (b == 0) return false; r = a % b; break;
                    default: return false;             // corrupt program
                }
                st[sp - 1] = clamp64(r);
                break;
            }
        }
    }
    if (sp != 1) return false;
    out = clamp64(st[0]);
    return true;
}

// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// expr_eval — tiny integer expression compiler/evaluator for rule action
// values (design: extra/docs/2026-07-08-simple-rules-expr-tier2-plan.md).
//
// Grammar (int32, one variable):
//   expr    := term (('+'|'-') term)*
//   term    := unary (('*'|'/'|'%') unary)*
//   unary   := ('!'|'-')? primary          — at most ONE unary per primary
//   primary := INT | '%value%' | '(' expr ')'
//
// Compiled ONCE at rule-add (non-recursive shunting-yard → RPN bytecode);
// evaluated per fire with a fixed-depth stack — no recursion, no heap, no
// per-fire parsing. Rules arrive from REST / the cloud builder, so the
// grammar is deliberately small and every cap below is a hard parse reject.
//
// Semantics:
//   • int32 domain; every op computed in int64 then CLAMPED to int32.
//   • '!' → (v == 0) ? 1 : 0.   Unary '-' negates (clamped).
//   • '/' '%' by a LITERAL zero reject at compile; a runtime zero divisor
//     makes expr_eval return false (caller skips the action).
//   • VAL_FLOAT events reach rules as the raw ×100 int (shadow contract), so
//     `%value%/100` is the idiom for whole units.
#pragma once
#include <cstdint>
#include <cstddef>

static constexpr size_t  EXPR_TEXT_MAX  = 48;  // max expression text length
static constexpr uint8_t EXPR_OPS_MAX   = 12;  // max RPN ops per program
static constexpr uint8_t EXPR_STACK_MAX = 8;   // eval stack depth
static constexpr uint8_t EXPR_PAREN_MAX = 6;   // max paren nesting

enum ExprOpCode : uint8_t {
    EXPR_PUSH_IMM = 0,   // push ops[i].imm
    EXPR_PUSH_VAL,       // push the trigger value
    EXPR_ADD, EXPR_SUB, EXPR_MUL, EXPR_DIV, EXPR_MOD,
    EXPR_NEG, EXPR_NOT,
};

struct ExprOp {
    uint8_t code;   // ExprOpCode
    int32_t imm;    // EXPR_PUSH_IMM only
};

struct ExprProg {
    uint8_t count;                 // 0 = empty/invalid
    ExprOp  ops[EXPR_OPS_MAX];
};

// Compile `text` into `out`. Returns false on any malformed input or cap
// violation; `err` (if non-null) then holds a short human-readable reason
// (surfaced through dsl_last_error() to REST/cloud callers).
bool expr_compile(const char* text, ExprProg& out, char* err, size_t err_cap);

// Evaluate a compiled program. Returns false on runtime failure (division or
// modulo by zero, or a malformed program) — the caller must skip the action.
bool expr_eval(const ExprProg& p, int32_t value, int32_t& out);

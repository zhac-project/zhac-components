# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# ESP-IDF component wrapper for openresty/lua-cjson 2.1.0.14.
#
# Upstream: https://github.com/openresty/lua-cjson
# Vendored under src/ (LICENSE is MIT). Exposes the symbol
# `luaopen_cjson` that `components/lua_engine/src/lua_require.c`
# pulls in when CONFIG_LUA_ENGINE_WITH_CJSON=y.
#
# Only the parts needed by the Lua binding are compiled: the main
# module, the string-buffer helper, and the default libc-backed
# float converter. We intentionally do NOT include the bundled
# `dtoa.c`/`g_fmt.c` fast-float path — it's ~5k LOC of Sun-licensed
# code only useful for ≤1ppm float->string fidelity which nothing
# here needs. `fpconv.c` falls back to `strtod()` + `snprintf()`
# when USE_INTERNAL_FPCONV is not defined.

idf_component_register(
    SRCS
        src/lua_cjson.c
        src/strbuf.c
        src/fpconv.c
    INCLUDE_DIRS
        src
    PRIV_REQUIRES
        lua
)

# No extra defines — upstream defaults give:
#   encoder:  refuses NaN/Inf (raises error) — safe default
#   decoder:  accepts JSON "NaN"/"Infinity" tokens — matches z2m parity
# Scripts that want strict RFC-4627 decode can call
# `cjson.decode_invalid_numbers(false)` at runtime.

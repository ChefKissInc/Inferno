/* SPDX-License-Identifier: MIT */
/*
 * Define tcg_debug_assert
 * Copyright (c) 2008 Fabrice Bellard
 * Copyright (c) 2026 Visual Ehrmanntraut
 */

#pragma once

#if defined CONFIG_DEBUG_TCG || defined QEMU_STATIC_ANALYSIS
    #define tcg_debug_assert(X) \
        do {                    \
            assert(X);          \
        }                       \
        while (0)
    #define tcg_debug_assert_not_reached() assert_not_reached()
#else
    #define tcg_debug_assert(X)                    \
        do {                                       \
            if (!(X)) { __builtin_unreachable(); } \
        }                                          \
        while (0)
    #define tcg_debug_assert_not_reached() __builtin_unreachable()
#endif

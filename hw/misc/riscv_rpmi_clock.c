/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * RISC-V RPMI Clock service.
 *
 * Copyright (c) 2026 Microchip
 */

#include "qemu/osdep.h"
#include "riscv_rpmi_internal.h"

enum {
    RISCV_RPMI_CLK_CPU,
    RISCV_RPMI_CLK_BUS,
    RISCV_RPMI_CLK_COUNT,
};

static const rpmi_uint64_t riscv_rpmi_clk_cpu_rates[] = {
    250000000ULL,
    500000000ULL,
    1000000000ULL,
};

/* Linear clock rates are described as min, max and step. */
static const rpmi_uint64_t riscv_rpmi_clk_bus_rates[] = {
    50000000ULL,
    500000000ULL,
    50000000ULL,
};

static const struct rpmi_clock_data riscv_rpmi_clock_data[] = {
    [RISCV_RPMI_CLK_CPU] = {
        .parent_id = -1U,
        .rate_count = ARRAY_SIZE(riscv_rpmi_clk_cpu_rates),
        .clock_type = RPMI_CLK_TYPE_DISCRETE,
        .name = "cpu",
        .clock_rate_array = riscv_rpmi_clk_cpu_rates,
    },
    [RISCV_RPMI_CLK_BUS] = {
        .parent_id = -1U,
        .rate_count = ARRAY_SIZE(riscv_rpmi_clk_bus_rates),
        .clock_type = RPMI_CLK_TYPE_LINEAR,
        .name = "bus",
        .clock_rate_array = riscv_rpmi_clk_bus_rates,
    },
};

static const uint64_t riscv_rpmi_clock_default_rates[] = {
    [RISCV_RPMI_CLK_CPU] = 1000000000ULL,
    [RISCV_RPMI_CLK_BUS] = 100000000ULL,
};

QEMU_BUILD_BUG_ON(ARRAY_SIZE(riscv_rpmi_clock_data) != RISCV_RPMI_CLK_COUNT);
QEMU_BUILD_BUG_ON(ARRAY_SIZE(riscv_rpmi_clock_default_rates) !=
                  RISCV_RPMI_CLK_COUNT);

static bool riscv_rpmi_clock_match_discrete(
    const struct rpmi_clock_data *clk, enum rpmi_clock_rate_match match,
    uint64_t rate, uint64_t *new_rate)
{
    bool found = false;
    uint64_t best = 0;

    for (uint32_t i = 0; i < clk->rate_count; i++) {
        uint64_t r = clk->clock_rate_array[i];

        switch (match) {
        case RPMI_CLK_RATE_MATCH_ROUND_DOWN:
            if (r <= rate && (!found || r > best)) {
                best = r;
                found = true;
            }
            break;
        case RPMI_CLK_RATE_MATCH_ROUND_UP:
            if (r >= rate && (!found || r < best)) {
                best = r;
                found = true;
            }
            break;
        default:
            if (!found ||
                (r > rate ? r - rate : rate - r) <
                (best > rate ? best - rate : rate - best)) {
                best = r;
                found = true;
            }
            break;
        }
    }

    *new_rate = best;
    return found;
}

static bool riscv_rpmi_clock_match_linear(
    const struct rpmi_clock_data *clk, enum rpmi_clock_rate_match match,
    uint64_t rate, uint64_t *new_rate)
{
    uint64_t min = clk->clock_rate_array[0];
    uint64_t max = clk->clock_rate_array[1];
    uint64_t step = clk->clock_rate_array[2];
    uint64_t down, up;

    if (rate < min || rate > max) {
        return false;
    }

    down = min + (rate - min) / step * step;
    up = down == rate ? down : MIN(down + step, max);

    switch (match) {
    case RPMI_CLK_RATE_MATCH_ROUND_DOWN:
        *new_rate = down;
        break;
    case RPMI_CLK_RATE_MATCH_ROUND_UP:
        *new_rate = up;
        break;
    default:
        *new_rate = rate - down <= up - rate ? down : up;
        break;
    }

    return true;
}

static enum rpmi_error riscv_rpmi_clock_set_state(void *priv,
                                                  rpmi_uint32_t clock_id,
                                                  enum rpmi_clock_state state)
{
    RiscvRpmiState *s = priv;

    if (clock_id >= RISCV_RPMI_CLK_COUNT || state >= RPMI_CLK_STATE_MAX) {
        return RPMI_ERR_INVALID_PARAM;
    }

    s->clock_states[clock_id] = state;
    return RPMI_SUCCESS;
}

static enum rpmi_error riscv_rpmi_clock_get_state_and_rate(
    void *priv, rpmi_uint32_t clock_id, enum rpmi_clock_state *state,
    rpmi_uint64_t *rate)
{
    RiscvRpmiState *s = priv;

    if (clock_id >= RISCV_RPMI_CLK_COUNT) {
        return RPMI_ERR_INVALID_PARAM;
    }

    if (state) {
        *state = s->clock_states[clock_id];
    }
    if (rate) {
        *rate = s->clock_rates[clock_id];
    }
    return RPMI_SUCCESS;
}

static rpmi_bool_t riscv_rpmi_clock_rate_change_match(void *priv,
                                                      rpmi_uint32_t clock_id,
                                                      rpmi_uint64_t rate)
{
    RiscvRpmiState *s = priv;

    if (clock_id >= RISCV_RPMI_CLK_COUNT) {
        return false;
    }

    return s->clock_rates[clock_id] != rate;
}

static enum rpmi_error riscv_rpmi_clock_set_rate(
    void *priv, rpmi_uint32_t clock_id, enum rpmi_clock_rate_match match,
    rpmi_uint64_t rate, rpmi_uint64_t *new_rate)
{
    RiscvRpmiState *s = priv;
    const struct rpmi_clock_data *clk;
    uint64_t matched;
    bool found;

    if (clock_id >= RISCV_RPMI_CLK_COUNT || !new_rate) {
        return RPMI_ERR_INVALID_PARAM;
    }

    clk = &riscv_rpmi_clock_data[clock_id];
    if (clk->clock_type == RPMI_CLK_TYPE_LINEAR) {
        found = riscv_rpmi_clock_match_linear(clk, match, rate, &matched);
    } else {
        found = riscv_rpmi_clock_match_discrete(clk, match, rate, &matched);
    }
    if (!found) {
        return RPMI_ERR_INVALID_PARAM;
    }

    s->clock_rates[clock_id] = matched;
    *new_rate = matched;
    return RPMI_SUCCESS;
}

static enum rpmi_error riscv_rpmi_clock_set_rate_recalc(
    void *priv, rpmi_uint32_t clock_id, rpmi_uint64_t parent_rate,
    rpmi_uint64_t *new_rate)
{
    RiscvRpmiState *s = priv;

    if (clock_id >= RISCV_RPMI_CLK_COUNT || !new_rate) {
        return RPMI_ERR_INVALID_PARAM;
    }

    s->clock_rates[clock_id] = parent_rate;
    *new_rate = parent_rate;
    return RPMI_SUCCESS;
}

static const struct rpmi_clock_platform_ops riscv_rpmi_clock_ops = {
    .set_state = riscv_rpmi_clock_set_state,
    .get_state_and_rate = riscv_rpmi_clock_get_state_and_rate,
    .rate_change_match = riscv_rpmi_clock_rate_change_match,
    .set_rate = riscv_rpmi_clock_set_rate,
    .set_rate_recalc = riscv_rpmi_clock_set_rate_recalc,
};

static void riscv_rpmi_clock_set_defaults(RiscvRpmiState *s)
{
    for (uint32_t i = 0; i < RISCV_RPMI_CLK_COUNT; i++) {
        s->clock_states[i] = RPMI_CLK_STATE_ENABLED;
        s->clock_rates[i] = riscv_rpmi_clock_default_rates[i];
    }
}

static bool riscv_rpmi_clock_create(RiscvRpmiState *s,
                                    struct rpmi_service_group **group,
                                    Error **errp)
{
    if (!s->clock_states) {
        s->clock_states = g_new0(uint32_t, RISCV_RPMI_CLK_COUNT);
        s->clock_rates = g_new0(uint64_t, RISCV_RPMI_CLK_COUNT);
    }
    riscv_rpmi_clock_set_defaults(s);

    *group = rpmi_service_group_clock_create(RISCV_RPMI_CLK_COUNT,
                                             riscv_rpmi_clock_data,
                                             &riscv_rpmi_clock_ops, s);
    if (!*group) {
        error_setg(errp, "failed to create RPMI clock service group");
        return false;
    }

    return true;
}

static void riscv_rpmi_clock_destroy(RiscvRpmiState *s)
{
    if (s->clock_group) {
        rpmi_service_group_clock_destroy(s->clock_group);
        s->clock_group = NULL;
    }

    g_clear_pointer(&s->clock_states, g_free);
    g_clear_pointer(&s->clock_rates, g_free);
}

bool riscv_rpmi_clock_add(RiscvRpmiState *s, Error **errp)
{
    struct rpmi_service_group *group;

    if (s->clock_group) {
        error_setg(errp, "duplicate RPMI clock descriptor");
        return false;
    }

    if (!riscv_rpmi_clock_create(s, &group, errp)) {
        riscv_rpmi_clock_destroy(s);
        return false;
    }

    if (!riscv_rpmi_context_add_group(s, group, "clock", errp)) {
        s->clock_group = group;
        riscv_rpmi_clock_destroy(s);
        return false;
    }

    s->clock_group = group;
    return true;
}

void riscv_rpmi_clock_remove(RiscvRpmiState *s)
{
    riscv_rpmi_context_remove_group(s, s->clock_group);
    riscv_rpmi_clock_destroy(s);
}

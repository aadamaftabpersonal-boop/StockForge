#include "payment_sim.h"

#include <string.h>

static const char *const NAMES[] = {"success", "decline", "timeout", "flaky"};

pay_scenario payment_sim_parse(const char *name) {
    for (int i = 0; i < 4; i++)
        if (strcmp(name, NAMES[i]) == 0) return (pay_scenario)i;
    return SCEN_INVALID;
}

const char *payment_sim_name(pay_scenario s) { return (s >= 0 && s <= SCEN_FLAKY) ? NAMES[s] : "invalid"; }

pay_result payment_sim_charge(pay_scenario s, unsigned attempt_no) {
    switch (s) {
    case SCEN_SUCCESS: return PAY_SUCCEEDED;
    case SCEN_DECLINE: return PAY_DECLINED;
    case SCEN_TIMEOUT: return PAY_TIMED_OUT;
    case SCEN_FLAKY: return (attempt_no % 2 == 0) ? PAY_SUCCEEDED : PAY_DECLINED;
    default: return PAY_DECLINED;
    }
}

const char *payment_result_sql(pay_result r) {
    return r == PAY_SUCCEEDED ? "succeeded" : r == PAY_DECLINED ? "failed" : "timed_out";
}

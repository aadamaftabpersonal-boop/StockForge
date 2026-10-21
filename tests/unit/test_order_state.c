#include <string.h>

#include "order_state.h"
#include "payment_sim.h"
#include "testutil.h"

/* The documented lifecycle, restated independently of the implementation's
 * table: any (from,to) pair NOT listed here must be rejected. */
static int expected_legal(order_status f, order_status t) {
    static const struct { order_status f, t; } LEGAL[] = {
        {OS_PENDING_PAYMENT, OS_PAID}, {OS_PENDING_PAYMENT, OS_CANCELLED}, {OS_PENDING_PAYMENT, OS_EXPIRED},
        {OS_PAID, OS_SHIPPED},         {OS_PAID, OS_CANCELLED},            {OS_SHIPPED, OS_DELIVERED},
    };
    for (size_t i = 0; i < sizeof LEGAL / sizeof LEGAL[0]; i++)
        if (LEGAL[i].f == f && LEGAL[i].t == t) return 1;
    return 0;
}

static void test_full_matrix(void) {
    int legal = 0;
    for (int f = 0; f < OS_COUNT; f++)
        for (int t = 0; t < OS_COUNT; t++) {
            int got = os_can_transition((order_status)f, (order_status)t);
            CHECK(got == expected_legal((order_status)f, (order_status)t));
            legal += got;
        }
    CHECK(legal == 6);
    for (int s = 0; s < OS_COUNT; s++) CHECK(!os_can_transition((order_status)s, (order_status)s));  /* no self loops */
}

static void test_terminal_and_names(void) {
    CHECK(os_is_terminal(OS_CANCELLED) && os_is_terminal(OS_EXPIRED) && os_is_terminal(OS_DELIVERED));
    CHECK(!os_is_terminal(OS_PENDING_PAYMENT) && !os_is_terminal(OS_PAID) && !os_is_terminal(OS_SHIPPED));
    for (int s = 0; s < OS_COUNT; s++) CHECK(os_parse(os_name((order_status)s)) == (order_status)s);
    CHECK(os_parse("paid") == OS_INVALID);          /* names match the SQL ENUM exactly */
    CHECK(os_parse("") == OS_INVALID && os_parse("REFUNDED") == OS_INVALID);
    CHECK(!os_can_transition(OS_INVALID, OS_PAID) && !os_can_transition(OS_PAID, OS_INVALID));
    CHECK(!os_can_transition((order_status)99, OS_PAID));
}

static void test_payment_sim(void) {
    CHECK(payment_sim_parse("success") == SCEN_SUCCESS && payment_sim_parse("flaky") == SCEN_FLAKY);
    CHECK(payment_sim_parse("SUCCESS") == SCEN_INVALID && payment_sim_parse("") == SCEN_INVALID);
    for (unsigned a = 1; a <= 6; a++) {
        CHECK(payment_sim_charge(SCEN_SUCCESS, a) == PAY_SUCCEEDED);
        CHECK(payment_sim_charge(SCEN_DECLINE, a) == PAY_DECLINED);
        CHECK(payment_sim_charge(SCEN_TIMEOUT, a) == PAY_TIMED_OUT);
        CHECK(payment_sim_charge(SCEN_FLAKY, a) == (a % 2 ? PAY_DECLINED : PAY_SUCCEEDED));   /* deterministic */
    }
    CHECK(strcmp(payment_result_sql(PAY_SUCCEEDED), "succeeded") == 0);
    CHECK(strcmp(payment_result_sql(PAY_DECLINED), "failed") == 0);
    CHECK(strcmp(payment_result_sql(PAY_TIMED_OUT), "timed_out") == 0);
    CHECK(strcmp(payment_sim_name(SCEN_DECLINE), "decline") == 0);
}

int main(void) {
    test_full_matrix();
    test_terminal_and_names();
    test_payment_sim();
    TEST_MAIN_END();
}

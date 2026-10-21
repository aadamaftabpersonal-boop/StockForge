#ifndef SF_PAYMENT_SIM_H
#define SF_PAYMENT_SIM_H

/* SIMULATED payment provider. No card data exists anywhere in this system:
 * there is no PAN, CVV or token field. The outcome is a pure function of the
 * requested scenario and the attempt number, so tests are deterministic.
 *
 * Boundary with a real provider (documented in docs/CHECKOUT.md): a real
 * integration would (1) persist a 'pending' attempt, (2) COMMIT, (3) call the
 * provider OUTSIDE any database transaction with an idempotency key, then
 * (4) record the result. Here step 3 is an in-memory function call, so the
 * attempt is recorded in the same transaction as its effect.                  */
typedef enum { SCEN_SUCCESS = 0, SCEN_DECLINE, SCEN_TIMEOUT, SCEN_FLAKY, SCEN_INVALID = -1 } pay_scenario;
typedef enum { PAY_SUCCEEDED = 0, PAY_DECLINED, PAY_TIMED_OUT } pay_result;

pay_scenario payment_sim_parse(const char *name);          /* "success|decline|timeout|flaky" */
const char  *payment_sim_name(pay_scenario s);
/* success: always succeeds. decline: always declined. timeout: always times out.
 * flaky: odd attempts are declined, even attempts succeed. */
pay_result   payment_sim_charge(pay_scenario s, unsigned attempt_no);
const char  *payment_result_sql(pay_result r);             /* "succeeded" | "failed" | "timed_out" */

#endif

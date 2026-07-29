#ifndef WLH_HOST_SIM_REPL_H
#define WLH_HOST_SIM_REPL_H

#include "app.h"

/* Runs the interactive JSON Lines REPL until quit/EOF/signal/transport loss.
 * Returns 0 on a clean exit. */
int wlh_repl_run(app_t *app);

/* Emits asynchronous host events as JSON Lines. No-op unless the REPL is
 * active. Called from the Core task; must not block or re-enter the Core. */
void wlh_repl_on_host_event(app_t *app, const wlh_host_event_t *event);

/* Claims ping results whose request_id belongs to the REPL. Returns true
 * when the result was consumed (the caller must not forward it). */
bool wlh_repl_on_ping_result(app_t *app, const sim_ping_result_t *result);

#endif

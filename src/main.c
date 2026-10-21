#include <stdio.h>

#include "config.h"
#include "log.h"
#include "server.h"

int main(void) {
    config cfg;
    char err[160];
    if (config_load(&cfg, err, sizeof err)) {
        log_msg("error", "config", err);
        return 2;
    }
    return server_run(&cfg);
}

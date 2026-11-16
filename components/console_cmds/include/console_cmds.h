#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Starts the REPL on the console UART and registers all commands.
esp_err_t console_cmds_start(void);

// Shared formatter used by "show status" and the UI: "3d 04:17" style.
void console_fmt_uptime(char *buf, size_t len, int64_t us, bool with_seconds);

/* SPDX-License-Identifier: Apache-2.0 */
/* Host stub: logs are dropped. */
#pragma once

static inline void log_stub(const char *fmt, ...)
{
	(void)fmt;
}

#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...) log_stub(__VA_ARGS__)
#define LOG_WRN(...) log_stub(__VA_ARGS__)
#define LOG_ERR(...) log_stub(__VA_ARGS__)

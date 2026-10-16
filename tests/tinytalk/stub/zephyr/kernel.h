/* SPDX-License-Identifier: Apache-2.0 */
/* Host stub: the work queue is test_adapter.c's list. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define BIT(n)        (1u << (n))
#define ARG_UNUSED(x) (void)(x)

struct k_work {
	void (*handler)(struct k_work *work);
};

#define K_WORK_DEFINE(name, fn) struct k_work name = {fn}

int k_work_submit(struct k_work *work);

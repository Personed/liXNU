// SPDX-License-Identifier: GPL-2.0-only
#include "fki_x86_64_transaction.h"

#include <assert.h>
#include <stdio.h>

static void run_events(enum fki_x86_transaction_state *state,
		       const enum fki_x86_transaction_event *events,
		       unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		assert(fki_x86_transaction_transition(state, events[i]) == 0);
}

static void test_normal_return(void)
{
	enum fki_x86_transaction_state state = FKI_X86_TX_INIT;
	const enum fki_x86_transaction_event events[] = {
		FKI_X86_TX_VALIDATE_OK, FKI_X86_TX_SNAPSHOT_OK,
		FKI_X86_TX_ENTER, FKI_X86_TX_COOPERATIVE_STOP,
		FKI_X86_TX_RESTORE_OK, FKI_X86_TX_FINISH,
	};

	run_events(&state, events, sizeof(events) / sizeof(events[0]));
	assert(state == FKI_X86_TX_COMPLETE);
	assert(fki_x86_transaction_terminal(state));
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_ABORT) == -EINVAL);
}

static void test_resume_cycle(void)
{
	enum fki_x86_transaction_state state = FKI_X86_TX_INIT;
	const enum fki_x86_transaction_event events[] = {
		FKI_X86_TX_VALIDATE_OK, FKI_X86_TX_SNAPSHOT_OK,
		FKI_X86_TX_ENTER, FKI_X86_TX_COOPERATIVE_STOP,
		FKI_X86_TX_RESTORE_OK, FKI_X86_TX_PUBLISH_WAIT,
		FKI_X86_TX_ACCEPT_REPLY, FKI_X86_TX_SNAPSHOT_OK,
		FKI_X86_TX_ENTER, FKI_X86_TX_FAULT_STOP,
		FKI_X86_TX_RESTORE_OK, FKI_X86_TX_FINISH,
	};

	run_events(&state, events, sizeof(events) / sizeof(events[0]));
	assert(state == FKI_X86_TX_COMPLETE);
}

static void test_shutdown_terminal_mappings(void)
{
	enum fki_x86_transaction_state state = FKI_X86_TX_INIT;
	const enum fki_x86_transaction_event controlled[] = {
		FKI_X86_TX_VALIDATE_OK, FKI_X86_TX_SNAPSHOT_OK,
		FKI_X86_TX_ENTER, FKI_X86_TX_COOPERATIVE_STOP,
		FKI_X86_TX_RESTORE_OK, FKI_X86_TX_FINISH,
	};
	const enum fki_x86_transaction_event unrecoverable[] = {
		FKI_X86_TX_VALIDATE_OK, FKI_X86_TX_SNAPSHOT_OK,
		FKI_X86_TX_ENTER, FKI_X86_TX_FAULT_STOP,
		FKI_X86_TX_RESTORE_OK, FKI_X86_TX_ABORT,
	};

	run_events(&state, controlled, sizeof(controlled) / sizeof(controlled[0]));
	assert(state == FKI_X86_TX_COMPLETE);
	state = FKI_X86_TX_INIT;
	run_events(&state, unrecoverable,
		   sizeof(unrecoverable) / sizeof(unrecoverable[0]));
	assert(state == FKI_X86_TX_ABORTED);
}

static void test_setup_failures(void)
{
	enum fki_x86_transaction_state state = FKI_X86_TX_INIT;

	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_VALIDATE_FAIL) == 0);
	assert(state == FKI_X86_TX_ABORTED);
	state = FKI_X86_TX_INIT;
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_VALIDATE_OK) == 0);
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_SNAPSHOT_FAIL) == 0);
	assert(state == FKI_X86_TX_ABORTED);
}

static void test_abort_timeout_and_cancel(void)
{
	enum fki_x86_transaction_state state;
	const struct {
		enum fki_x86_transaction_state state;
		enum fki_x86_transaction_event event;
	} cases[] = {
		{ FKI_X86_TX_ARMED, FKI_X86_TX_ABORT },
		{ FKI_X86_TX_FOREIGN, FKI_X86_TX_ABORT },
		{ FKI_X86_TX_STOPPED, FKI_X86_TX_RESTORE_FAIL },
		{ FKI_X86_TX_RESTORED, FKI_X86_TX_CANCEL },
		{ FKI_X86_TX_WAITING, FKI_X86_TX_TIMEOUT },
		{ FKI_X86_TX_WAITING, FKI_X86_TX_CANCEL },
		{ FKI_X86_TX_RESUME_READY, FKI_X86_TX_SNAPSHOT_FAIL },
		{ FKI_X86_TX_RESUME_READY, FKI_X86_TX_ABORT },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		state = cases[i].state;
		assert(fki_x86_transaction_transition(&state, cases[i].event) == 0);
		assert(state == FKI_X86_TX_ABORTED);
	}
}

static void test_invalid_transitions(void)
{
	enum fki_x86_transaction_state state = FKI_X86_TX_INIT;

	assert(fki_x86_transaction_transition(NULL, FKI_X86_TX_VALIDATE_OK) == -EINVAL);
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_ENTER) == -EINVAL);
	assert(state == FKI_X86_TX_INIT);
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_VALIDATE_OK) == 0);
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_ENTER) == -EINVAL);
	assert(state == FKI_X86_TX_VALIDATED);
	state = FKI_X86_TX_WAITING;
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_ENTER) == -EINVAL);
	assert(state == FKI_X86_TX_WAITING);
	state = FKI_X86_TX_RESUME_READY;
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_TIMEOUT) == -EINVAL);
	assert(fki_x86_transaction_transition(&state, FKI_X86_TX_CANCEL) == -EINVAL);
	assert(state == FKI_X86_TX_RESUME_READY);
}

int main(void)
{
	test_normal_return();
	test_resume_cycle();
	test_shutdown_terminal_mappings();
	test_setup_failures();
	test_abort_timeout_and_cancel();
	test_invalid_transitions();
	puts("FKI x86 transaction fixtures passed");
	return 0;
}

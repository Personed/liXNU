/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

/*
 * Validation-only model of the x86_64 FKI transaction lifecycle.
 * Events are synthetic contract events: this helper neither enters foreign
 * code nor snapshots, restores, or arms real CPU state.
 */
enum fki_x86_transaction_state {
	FKI_X86_TX_INIT,
	FKI_X86_TX_VALIDATED,
	FKI_X86_TX_ARMED,
	FKI_X86_TX_FOREIGN,
	FKI_X86_TX_STOPPED,
	FKI_X86_TX_RESTORED,
	FKI_X86_TX_WAITING,
	FKI_X86_TX_RESUME_READY,
	FKI_X86_TX_COMPLETE,
	FKI_X86_TX_ABORTED,
};

enum fki_x86_transaction_event {
	FKI_X86_TX_VALIDATE_OK,
	FKI_X86_TX_VALIDATE_FAIL,
	FKI_X86_TX_SNAPSHOT_OK,
	FKI_X86_TX_SNAPSHOT_FAIL,
	FKI_X86_TX_ENTER,
	FKI_X86_TX_COOPERATIVE_STOP,
	FKI_X86_TX_FAULT_STOP,
	FKI_X86_TX_RESTORE_OK,
	FKI_X86_TX_RESTORE_FAIL,
	FKI_X86_TX_FINISH,
	FKI_X86_TX_PUBLISH_WAIT,
	FKI_X86_TX_ACCEPT_REPLY,
	FKI_X86_TX_TIMEOUT,
	FKI_X86_TX_CANCEL,
	FKI_X86_TX_ABORT,
};

static inline bool fki_x86_transaction_terminal(enum fki_x86_transaction_state state)
{
	return state == FKI_X86_TX_COMPLETE || state == FKI_X86_TX_ABORTED;
}

static inline int fki_x86_transaction_transition(
	enum fki_x86_transaction_state *state,
	enum fki_x86_transaction_event event)
{
	enum fki_x86_transaction_state next;

	if (!state || fki_x86_transaction_terminal(*state))
		return -EINVAL;

	switch (*state) {
	case FKI_X86_TX_INIT:
		if (event == FKI_X86_TX_VALIDATE_OK)
			next = FKI_X86_TX_VALIDATED;
		else if (event == FKI_X86_TX_VALIDATE_FAIL)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_VALIDATED:
		if (event == FKI_X86_TX_SNAPSHOT_OK)
			next = FKI_X86_TX_ARMED;
		else if (event == FKI_X86_TX_SNAPSHOT_FAIL)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_ARMED:
		if (event == FKI_X86_TX_ENTER)
			next = FKI_X86_TX_FOREIGN;
		else if (event == FKI_X86_TX_ABORT || event == FKI_X86_TX_CANCEL)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_FOREIGN:
		if (event == FKI_X86_TX_COOPERATIVE_STOP || event == FKI_X86_TX_FAULT_STOP)
			next = FKI_X86_TX_STOPPED;
		else if (event == FKI_X86_TX_ABORT)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_STOPPED:
		if (event == FKI_X86_TX_RESTORE_OK)
			next = FKI_X86_TX_RESTORED;
		else if (event == FKI_X86_TX_RESTORE_FAIL || event == FKI_X86_TX_ABORT)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_RESTORED:
		if (event == FKI_X86_TX_FINISH)
			next = FKI_X86_TX_COMPLETE;
		else if (event == FKI_X86_TX_PUBLISH_WAIT)
			next = FKI_X86_TX_WAITING;
		else if (event == FKI_X86_TX_ABORT || event == FKI_X86_TX_CANCEL)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_WAITING:
		if (event == FKI_X86_TX_ACCEPT_REPLY)
			next = FKI_X86_TX_RESUME_READY;
		else if (event == FKI_X86_TX_TIMEOUT || event == FKI_X86_TX_CANCEL ||
			 event == FKI_X86_TX_ABORT)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	case FKI_X86_TX_RESUME_READY:
		if (event == FKI_X86_TX_SNAPSHOT_OK)
			next = FKI_X86_TX_ARMED;
		else if (event == FKI_X86_TX_SNAPSHOT_FAIL || event == FKI_X86_TX_ABORT)
			next = FKI_X86_TX_ABORTED;
		else
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	*state = next;
	return 0;
}

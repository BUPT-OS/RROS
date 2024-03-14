// SPDX-License-Identifier: GPL-2.0-only
/*
 * AppArmor security module
 *
 * This file contains AppArmor task related definitions and mediation
 *
 * Copyright 2017 Canonical Ltd.
 *
 * TODO
 * If a task uses change_hat it currently does not return to the old
 * cred or task context but instead creates a new one.  Ideally the task
 * should return to the previous cred if it has not been modified.
 */

#include <linux/gfp.h>
#include <linux/ptrace.h>

#include "include/audit.h"
#include "include/cred.h"
#include "include/policy.h"
#include "include/task.h"

/**
 * aa_get_task_label - Get another task's label
 * @task: task to query  (NOT NULL)
 *
 * Returns: counted reference to @task's label
 */
struct aa_label *aa_get_task_label(struct task_struct *task)
{
	struct aa_label *p;

	rcu_read_lock();
	p = aa_get_newest_cred_label(__task_cred(task));
	rcu_read_unlock();

	return p;
}

/**
 * aa_replace_current_label - replace the current tasks label
 * @label: new label  (NOT NULL)
 *
 * Returns: 0 or error on failure
 */
int aa_replace_current_label(struct aa_label *label)
{
	struct aa_label *old = aa_current_raw_label();
	struct aa_task_ctx *ctx = task_ctx(current);
	struct cred *new;

	AA_BUG(!label);

	if (old == label)
		return 0;

	if (current_cred() != current_real_cred())
		return -EBUSY;

	new  = prepare_creds();
	if (!new)
		return -ENOMEM;

	if (ctx->nnp && label_is_stale(ctx->nnp)) {
		struct aa_label *tmp = ctx->nnp;

		ctx->nnp = aa_get_newest_label(tmp);
		aa_put_label(tmp);
	}
	if (unconfined(label) || (labels_ns(old) != labels_ns(label)))
		/*
		 * if switching to unconfined or a different label namespace
		 * clear out context state
		 */
		aa_clear_task_ctx_trans(task_ctx(current));

	/*
	 * be careful switching cred label, when racing replacement it
	 * is possible that the cred labels's->proxy->label is the reference
	 * keeping @label valid, so make sure to get its reference before
	 * dropping the reference on the cred's label
	 */
	aa_get_label(label);
	aa_put_label(cred_label(new));
	set_cred_label(new, label);

	commit_creds(new);
	return 0;
}


/**
 * aa_set_current_onexec - set the tasks change_profile to happen onexec
 * @label: system label to set at exec  (MAYBE NULL to clear value)
 * @stack: whether stacking should be done
 * Returns: 0 or error on failure
 */
int aa_set_current_onexec(struct aa_label *label, bool stack)
{
	struct aa_task_ctx *ctx = task_ctx(current);

	aa_get_label(label);
	aa_put_label(ctx->onexec);
	ctx->onexec = label;
	ctx->token = stack;

	return 0;
}

/**
 * aa_set_current_hat - set the current tasks hat
 * @label: label to set as the current hat  (NOT NULL)
 * @token: token value that must be specified to change from the hat
 *
 * Do switch of tasks hat.  If the task is currently in a hat
 * validate the token to match.
 *
 * Returns: 0 or error on failure
 */
int aa_set_current_hat(struct aa_label *label, u64 token)
{
	struct aa_task_ctx *ctx = task_ctx(current);
	struct cred *new;

	new = prepare_creds();
	if (!new)
		return -ENOMEM;
	AA_BUG(!label);

	if (!ctx->previous) {
		/* transfer refcount */
		ctx->previous = cred_label(new);
		ctx->token = token;
	} else if (ctx->token == token) {
		aa_put_label(cred_label(new));
	} else {
		/* previous_profile && ctx->token != token */
		abort_creds(new);
		return -EACCES;
	}

	set_cred_label(new, aa_get_newest_label(label));
	/* clear exec on switching context */
	aa_put_label(ctx->onexec);
	ctx->onexec = NULL;

	commit_creds(new);
	return 0;
}

/**
 * aa_restore_previous_label - exit from hat context restoring previous label
 * @token: the token that must be matched to exit hat context
 *
 * Attempt to return out of a hat to the previous label.  The token
 * must match the stored token value.
 *
 * Returns: 0 or error of failure
 */
int aa_restore_previous_label(u64 token)
{
	struct aa_task_ctx *ctx = task_ctx(current);
	struct cred *new;

	if (ctx->token != token)
		return -EACCES;
	/* ignore restores when there is no saved label */
	if (!ctx->previous)
		return 0;

	new = prepare_creds();
	if (!new)
		return -ENOMEM;

	aa_put_label(cred_label(new));
	set_cred_label(new, aa_get_newest_label(ctx->previous));
	AA_BUG(!cred_label(new));
	/* clear exec && prev information when restoring to previous context */
	aa_clear_task_ctx_trans(ctx);

	commit_creds(new);

	return 0;
}

/**
 * audit_ptrace_mask - convert mask to permission string
 * @mask: permission mask to convert
 *
 * Returns: pointer to static string
 */
static const char *audit_ptrace_mask(u32 mask)
{
	switch (mask) {
	case MAY_READ:
		return "read";
	case MAY_WRITE:
		return "trace";
	case AA_MAY_BE_READ:
		return "readby";
	case AA_MAY_BE_TRACED:
		return "tracedby";
	}
	return "";
}

/* call back to audit ptrace fields */
static void audit_ptrace_cb(struct audit_buffer *ab, void *va)
{
	struct common_audit_data *sa = va;

	if (aad(sa)->request & AA_PTRACE_PERM_MASK) {
		audit_log_format(ab, " requested_mask=\"%s\"",
				 audit_ptrace_mask(aad(sa)->request));

		if (aad(sa)->denied & AA_PTRACE_PERM_MASK) {
			audit_log_format(ab, " denied_mask=\"%s\"",
					 audit_ptrace_mask(aad(sa)->denied));
		}
	}
	audit_log_format(ab, " peer=");
	aa_label_xaudit(ab, labels_ns(aad(sa)->label), aad(sa)->peer,
			FLAGS_NONE, GFP_ATOMIC);
}

/* assumes check for RULE_MEDIATES is already done */
/* TODO: conditionals */
static int profile_ptrace_perm(struct aa_profile *profile,
			     struct aa_label *peer, u32 request,
			     struct common_audit_data *sa)
{
	struct aa_ruleset *rules = list_first_entry(&profile->rules,
						    typeof(*rules), list);
	struct aa_perms perms = { };

	aad(sa)->peer = peer;
	aa_profile_match_label(profile, rules, peer, AA_CLASS_PTRACE, request,
			       &perms);
	aa_apply_modes_to_perms(profile, &perms);
	return aa_check_perms(profile, &perms, request, sa, audit_ptrace_cb);
}

static int profile_tracee_perm(struct aa_profile *tracee,
			       struct aa_label *tracer, u32 request,
			       struct common_audit_data *sa)
{
	if (profile_unconfined(tracee) || unconfined(tracer) ||
	    !ANY_RULE_MEDIATES(&tracee->rules, AA_CLASS_PTRACE))
		return 0;

	return profile_ptrace_perm(tracee, tracer, request, sa);
}

static int profile_tracer_perm(struct aa_profile *tracer,
			       struct aa_label *tracee, u32 request,
			       struct common_audit_data *sa)
{
	if (profile_unconfined(tracer))
		return 0;

	if (ANY_RULE_MEDIATES(&tracer->rules, AA_CLASS_PTRACE))
		return profile_ptrace_perm(tracer, tracee, request, sa);

	/* profile uses the old style capability check for ptrace */
	if (&tracer->label == tracee)
		return 0;

	aad(sa)->label = &tracer->label;
	aad(sa)->peer = tracee;
	aad(sa)->request = 0;
	aad(sa)->error = aa_capable(&tracer->label, CAP_SYS_PTRACE,
				    CAP_OPT_NONE);

	return aa_audit(AUDIT_APPARMOR_AUTO, tracer, sa, audit_ptrace_cb);
}

/**
 * aa_may_ptrace - test if tracer task can trace the tracee
 * @tracer: label of the task doing the tracing  (NOT NULL)
 * @tracee: task label to be traced
 * @request: permission request
 *
 * Returns: %0 else error code if permission denied or error
 */
int aa_may_ptrace(struct aa_label *tracer, struct aa_label *tracee,
		  u32 request)
{
	struct aa_profile *profile;
	u32 xrequest = request << PTRACE_PERM_SHIFT;
	DEFINE_AUDIT_DATA(sa, LSM_AUDIT_DATA_NONE, AA_CLASS_PTRACE, OP_PTRACE);

	return xcheck_labels(tracer, tracee, profile,
			profile_tracer_perm(profile, tracee, request, &sa),
			profile_tracee_perm(profile, tracer, xrequest, &sa));
}

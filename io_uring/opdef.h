// SPDX-License-Identifier: GPL-2.0
#ifndef IOU_OP_DEF_H
#define IOU_OP_DEF_H

struct io_issue_def {
	/* needs req->file assigned */
	unsigned		needs_file : 1;
	/* should block plug */
	unsigned		plug : 1;
	/* hash wq insertion if file is a regular file */
	unsigned		hash_reg_file : 1;
	/* unbound wq insertion if file is a non-regular file */
	unsigned		unbound_nonreg_file : 1;
	/* set if opcode supports polled "wait" */
	unsigned		pollin : 1;
	unsigned		pollout : 1;
	unsigned		poll_exclusive : 1;
	/* op supports buffer selection */
	unsigned		buffer_select : 1;
	/* opcode is not supported by this kernel */
	unsigned		not_supported : 1;
	/* skip auditing */
	unsigned		audit_skip : 1;
	/* supports ioprio */
	unsigned		ioprio : 1;
	/* supports iopoll */
	unsigned		iopoll : 1;
	/* have to be put into the iopoll list */
	unsigned		iopoll_queue : 1;
	/* opcode specific path will handle ->async_data allocation if needed */
	unsigned		manual_alloc : 1;

	int (*issue)(struct io_kiocb *, unsigned int);
	int (*prep)(struct io_kiocb *, const struct io_uring_sqe *);
};

struct io_cold_def {
	/* size of async data needed, if any */
	unsigned short		async_size;

	const char		*name;

	int (*prep_async)(struct io_kiocb *);
	void (*cleanup)(struct io_kiocb *);
	void (*fail)(struct io_kiocb *);
};

extern const struct io_issue_def io_issue_defs[];
extern const struct io_cold_def io_cold_defs[];

void io_uring_optable_init(void);
#endif

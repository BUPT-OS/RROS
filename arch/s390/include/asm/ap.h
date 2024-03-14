/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Adjunct processor (AP) interfaces
 *
 * Copyright IBM Corp. 2017
 *
 * Author(s): Tony Krowiak <akrowia@linux.vnet.ibm.com>
 *	      Martin Schwidefsky <schwidefsky@de.ibm.com>
 *	      Harald Freudenberger <freude@de.ibm.com>
 */

#ifndef _ASM_S390_AP_H_
#define _ASM_S390_AP_H_

#include <linux/io.h>
#include <asm/asm-extable.h>

/**
 * The ap_qid_t identifier of an ap queue.
 * If the AP facilities test (APFT) facility is available,
 * card and queue index are 8 bit values, otherwise
 * card index is 6 bit and queue index a 4 bit value.
 */
typedef unsigned int ap_qid_t;

#define AP_MKQID(_card, _queue) (((_card) & 0xff) << 8 | ((_queue) & 0xff))
#define AP_QID_CARD(_qid) (((_qid) >> 8) & 0xff)
#define AP_QID_QUEUE(_qid) ((_qid) & 0xff)

/**
 * struct ap_queue_status - Holds the AP queue status.
 * @queue_empty: Shows if queue is empty
 * @replies_waiting: Waiting replies
 * @queue_full: Is 1 if the queue is full
 * @irq_enabled: Shows if interrupts are enabled for the AP
 * @response_code: Holds the 8 bit response code
 *
 * The ap queue status word is returned by all three AP functions
 * (PQAP, NQAP and DQAP).  There's a set of flags in the first
 * byte, followed by a 1 byte response code.
 */
struct ap_queue_status {
	unsigned int queue_empty	: 1;
	unsigned int replies_waiting	: 1;
	unsigned int queue_full		: 1;
	unsigned int			: 3;
	unsigned int async		: 1;
	unsigned int irq_enabled	: 1;
	unsigned int response_code	: 8;
	unsigned int			: 16;
};

/*
 * AP queue status reg union to access the reg1
 * register with the lower 32 bits comprising the
 * ap queue status.
 */
union ap_queue_status_reg {
	unsigned long value;
	struct {
		u32 _pad;
		struct ap_queue_status status;
	};
};

/**
 * ap_intructions_available() - Test if AP instructions are available.
 *
 * Returns true if the AP instructions are installed, otherwise false.
 */
static inline bool ap_instructions_available(void)
{
	unsigned long reg0 = AP_MKQID(0, 0);
	unsigned long reg1 = 0;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid into gr0 */
		"	lghi	1,0\n"			/* 0 into gr1 */
		"	lghi	2,0\n"			/* 0 into gr2 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(TAPQ) */
		"0:	la	%[reg1],1\n"		/* 1 into reg1 */
		"1:\n"
		EX_TABLE(0b, 1b)
		: [reg1] "+&d" (reg1)
		: [reg0] "d" (reg0)
		: "cc", "0", "1", "2");
	return reg1 != 0;
}

/* TAPQ register GR2 response struct */
struct ap_tapq_gr2 {
	union {
		unsigned long value;
		struct {
			unsigned int fac    : 32; /* facility bits */
			unsigned int apinfo : 32; /* ap type, ... */
		};
		struct {
			unsigned int s	   :  1; /* APSC */
			unsigned int m	   :  1; /* AP4KM */
			unsigned int c	   :  1; /* AP4KC */
			unsigned int mode  :  3;
			unsigned int n	   :  1; /* APXA */
			unsigned int	   :  1;
			unsigned int class :  8;
			unsigned int bs	   :  2; /* SE bind/assoc */
			unsigned int	   : 14;
			unsigned int at	   :  8; /* ap type */
			unsigned int nd	   :  8; /* nr of domains */
			unsigned int	   :  4;
			unsigned int ml	   :  4; /* apxl ml */
			unsigned int	   :  4;
			unsigned int qd	   :  4; /* queue depth */
		};
	};
};

/*
 * Convenience defines to be used with the bs field from struct ap_tapq_gr2
 */
#define AP_BS_Q_USABLE		      0
#define AP_BS_Q_USABLE_NO_SECURE_KEY  1
#define AP_BS_Q_AVAIL_FOR_BINDING     2
#define AP_BS_Q_UNUSABLE	      3

/**
 * ap_tapq(): Test adjunct processor queue.
 * @qid: The AP queue number
 * @info: Pointer to queue descriptor
 *
 * Returns AP queue status structure.
 */
static inline struct ap_queue_status ap_tapq(ap_qid_t qid, struct ap_tapq_gr2 *info)
{
	union ap_queue_status_reg reg1;
	unsigned long reg2;

	asm volatile(
		"	lgr	0,%[qid]\n"		/* qid into gr0 */
		"	lghi	2,0\n"			/* 0 into gr2 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(TAPQ) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		"	lgr	%[reg2],2\n"		/* gr2 into reg2 */
		: [reg1] "=&d" (reg1.value), [reg2] "=&d" (reg2)
		: [qid] "d" (qid)
		: "cc", "0", "1", "2");
	if (info)
		info->value = reg2;
	return reg1.status;
}

/**
 * ap_test_queue(): Test adjunct processor queue.
 * @qid: The AP queue number
 * @tbit: Test facilities bit
 * @info: Ptr to tapq gr2 struct
 *
 * Returns AP queue status structure.
 */
static inline struct ap_queue_status ap_test_queue(ap_qid_t qid, int tbit,
						   struct ap_tapq_gr2 *info)
{
	if (tbit)
		qid |= 1UL << 23; /* set T bit*/
	return ap_tapq(qid, info);
}

/**
 * ap_pqap_rapq(): Reset adjunct processor queue.
 * @qid: The AP queue number
 * @fbit: if != 0 set F bit
 *
 * Returns AP queue status structure.
 */
static inline struct ap_queue_status ap_rapq(ap_qid_t qid, int fbit)
{
	unsigned long reg0 = qid | (1UL << 24);  /* fc 1UL is RAPQ */
	union ap_queue_status_reg reg1;

	if (fbit)
		reg0 |= 1UL << 22;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid arg into gr0 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(RAPQ) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		: [reg1] "=&d" (reg1.value)
		: [reg0] "d" (reg0)
		: "cc", "0", "1");
	return reg1.status;
}

/**
 * ap_pqap_zapq(): Reset and zeroize adjunct processor queue.
 * @qid: The AP queue number
 * @fbit: if != 0 set F bit
 *
 * Returns AP queue status structure.
 */
static inline struct ap_queue_status ap_zapq(ap_qid_t qid, int fbit)
{
	unsigned long reg0 = qid | (2UL << 24);  /* fc 2UL is ZAPQ */
	union ap_queue_status_reg reg1;

	if (fbit)
		reg0 |= 1UL << 22;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid arg into gr0 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(ZAPQ) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		: [reg1] "=&d" (reg1.value)
		: [reg0] "d" (reg0)
		: "cc", "0", "1");
	return reg1.status;
}

/**
 * struct ap_config_info - convenience struct for AP crypto
 * config info as returned by the ap_qci() function.
 */
struct ap_config_info {
	unsigned int apsc	 : 1;	/* S bit */
	unsigned int apxa	 : 1;	/* N bit */
	unsigned int qact	 : 1;	/* C bit */
	unsigned int rc8a	 : 1;	/* R bit */
	unsigned int		 : 4;
	unsigned int apsb	 : 1;	/* B bit */
	unsigned int		 : 23;
	unsigned char na;		/* max # of APs - 1 */
	unsigned char nd;		/* max # of Domains - 1 */
	unsigned char _reserved0[10];
	unsigned int apm[8];		/* AP ID mask */
	unsigned int aqm[8];		/* AP (usage) queue mask */
	unsigned int adm[8];		/* AP (control) domain mask */
	unsigned char _reserved1[16];
} __aligned(8);

/**
 * ap_qci(): Get AP configuration data
 *
 * Returns 0 on success, or -EOPNOTSUPP.
 */
static inline int ap_qci(struct ap_config_info *config)
{
	unsigned long reg0 = 4UL << 24;  /* fc 4UL is QCI */
	unsigned long reg1 = -EOPNOTSUPP;
	struct ap_config_info *reg2 = config;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* QCI fc into gr0 */
		"	lgr	2,%[reg2]\n"		/* ptr to config into gr2 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(QCI) */
		"0:	la	%[reg1],0\n"		/* good case, QCI fc available */
		"1:\n"
		EX_TABLE(0b, 1b)
		: [reg1] "+&d" (reg1)
		: [reg0] "d" (reg0), [reg2] "d" (reg2)
		: "cc", "memory", "0", "2");

	return reg1;
}

/*
 * struct ap_qirq_ctrl - convenient struct for easy invocation
 * of the ap_aqic() function. This struct is passed as GR1
 * parameter to the PQAP(AQIC) instruction. For details please
 * see the AR documentation.
 */
union ap_qirq_ctrl {
	unsigned long value;
	struct {
		unsigned int	   : 8;
		unsigned int zone  : 8;	/* zone info */
		unsigned int ir	   : 1;	/* ir flag: enable (1) or disable (0) irq */
		unsigned int	   : 4;
		unsigned int gisc  : 3;	/* guest isc field */
		unsigned int	   : 6;
		unsigned int gf	   : 2;	/* gisa format */
		unsigned int	   : 1;
		unsigned int gisa  : 27;	/* gisa origin */
		unsigned int	   : 1;
		unsigned int isc   : 3;	/* irq sub class */
	};
};

/**
 * ap_aqic(): Control interruption for a specific AP.
 * @qid: The AP queue number
 * @qirqctrl: struct ap_qirq_ctrl (64 bit value)
 * @pa_ind: Physical address of the notification indicator byte
 *
 * Returns AP queue status.
 */
static inline struct ap_queue_status ap_aqic(ap_qid_t qid,
					     union ap_qirq_ctrl qirqctrl,
					     phys_addr_t pa_ind)
{
	unsigned long reg0 = qid | (3UL << 24);  /* fc 3UL is AQIC */
	union ap_queue_status_reg reg1;
	unsigned long reg2 = pa_ind;

	reg1.value = qirqctrl.value;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid param into gr0 */
		"	lgr	1,%[reg1]\n"		/* irq ctrl into gr1 */
		"	lgr	2,%[reg2]\n"		/* ni addr into gr2 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(AQIC) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		: [reg1] "+&d" (reg1.value)
		: [reg0] "d" (reg0), [reg2] "d" (reg2)
		: "cc", "memory", "0", "1", "2");

	return reg1.status;
}

/*
 * union ap_qact_ap_info - used together with the
 * ap_aqic() function to provide a convenient way
 * to handle the ap info needed by the qact function.
 */
union ap_qact_ap_info {
	unsigned long val;
	struct {
		unsigned int	  : 3;
		unsigned int mode : 3;
		unsigned int	  : 26;
		unsigned int cat  : 8;
		unsigned int	  : 8;
		unsigned char ver[2];
	};
};

/**
 * ap_qact(): Query AP compatibility type.
 * @qid: The AP queue number
 * @apinfo: On input the info about the AP queue. On output the
 *	    alternate AP queue info provided by the qact function
 *	    in GR2 is stored in.
 *
 * Returns AP queue status. Check response_code field for failures.
 */
static inline struct ap_queue_status ap_qact(ap_qid_t qid, int ifbit,
					     union ap_qact_ap_info *apinfo)
{
	unsigned long reg0 = qid | (5UL << 24) | ((ifbit & 0x01) << 22);
	union ap_queue_status_reg reg1;
	unsigned long reg2;

	reg1.value = apinfo->val;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid param into gr0 */
		"	lgr	1,%[reg1]\n"		/* qact in info into gr1 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(QACT) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		"	lgr	%[reg2],2\n"		/* qact out info into reg2 */
		: [reg1] "+&d" (reg1.value), [reg2] "=&d" (reg2)
		: [reg0] "d" (reg0)
		: "cc", "0", "1", "2");
	apinfo->val = reg2;
	return reg1.status;
}

/*
 * ap_bapq(): SE bind AP queue.
 * @qid: The AP queue number
 *
 * Returns AP queue status structure.
 *
 * Invoking this function in a non-SE environment
 * may case a specification exception.
 */
static inline struct ap_queue_status ap_bapq(ap_qid_t qid)
{
	unsigned long reg0 = qid | (7UL << 24);  /* fc 7 is BAPQ */
	union ap_queue_status_reg reg1;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid arg into gr0 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(BAPQ) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		: [reg1] "=&d" (reg1.value)
		: [reg0] "d" (reg0)
		: "cc", "0", "1");

	return reg1.status;
}

/*
 * ap_aapq(): SE associate AP queue.
 * @qid: The AP queue number
 * @sec_idx: The secret index
 *
 * Returns AP queue status structure.
 *
 * Invoking this function in a non-SE environment
 * may case a specification exception.
 */
static inline struct ap_queue_status ap_aapq(ap_qid_t qid, unsigned int sec_idx)
{
	unsigned long reg0 = qid | (8UL << 24);  /* fc 8 is AAPQ */
	unsigned long reg2 = sec_idx;
	union ap_queue_status_reg reg1;

	asm volatile(
		"	lgr	0,%[reg0]\n"		/* qid arg into gr0 */
		"	lgr	2,%[reg2]\n"		/* secret index into gr2 */
		"	.insn	rre,0xb2af0000,0,0\n"	/* PQAP(AAPQ) */
		"	lgr	%[reg1],1\n"		/* gr1 (status) into reg1 */
		: [reg1] "=&d" (reg1.value)
		: [reg0] "d" (reg0), [reg2] "d" (reg2)
		: "cc", "0", "1", "2");

	return reg1.status;
}

/**
 * ap_nqap(): Send message to adjunct processor queue.
 * @qid: The AP queue number
 * @psmid: The program supplied message identifier
 * @msg: The message text
 * @length: The message length
 *
 * Returns AP queue status structure.
 * Condition code 1 on NQAP can't happen because the L bit is 1.
 * Condition code 2 on NQAP also means the send is incomplete,
 * because a segment boundary was reached. The NQAP is repeated.
 */
static inline struct ap_queue_status ap_nqap(ap_qid_t qid,
					     unsigned long long psmid,
					     void *msg, size_t length)
{
	unsigned long reg0 = qid | 0x40000000UL;  /* 0x4... is last msg part */
	union register_pair nqap_r1, nqap_r2;
	union ap_queue_status_reg reg1;

	nqap_r1.even = (unsigned int)(psmid >> 32);
	nqap_r1.odd  = psmid & 0xffffffff;
	nqap_r2.even = (unsigned long)msg;
	nqap_r2.odd  = (unsigned long)length;

	asm volatile (
		"	lgr	0,%[reg0]\n"  /* qid param in gr0 */
		"0:	.insn	rre,0xb2ad0000,%[nqap_r1],%[nqap_r2]\n"
		"	brc	2,0b\n"       /* handle partial completion */
		"	lgr	%[reg1],1\n"  /* gr1 (status) into reg1 */
		: [reg0] "+&d" (reg0), [reg1] "=&d" (reg1.value),
		  [nqap_r2] "+&d" (nqap_r2.pair)
		: [nqap_r1] "d" (nqap_r1.pair)
		: "cc", "memory", "0", "1");
	return reg1.status;
}

/**
 * ap_dqap(): Receive message from adjunct processor queue.
 * @qid: The AP queue number
 * @psmid: Pointer to program supplied message identifier
 * @msg: Pointer to message buffer
 * @msglen: Message buffer size
 * @length: Pointer to length of actually written bytes
 * @reslength: Residual length on return
 * @resgr0: input: gr0 value (only used if != 0), output: residual gr0 content
 *
 * Returns AP queue status structure.
 * Condition code 1 on DQAP means the receive has taken place
 * but only partially.	The response is incomplete, hence the
 * DQAP is repeated.
 * Condition code 2 on DQAP also means the receive is incomplete,
 * this time because a segment boundary was reached. Again, the
 * DQAP is repeated.
 * Note that gpr2 is used by the DQAP instruction to keep track of
 * any 'residual' length, in case the instruction gets interrupted.
 * Hence it gets zeroed before the instruction.
 * If the message does not fit into the buffer, this function will
 * return with a truncated message and the reply in the firmware queue
 * is not removed. This is indicated to the caller with an
 * ap_queue_status response_code value of all bits on (0xFF) and (if
 * the reslength ptr is given) the remaining length is stored in
 * *reslength and (if the resgr0 ptr is given) the updated gr0 value
 * for further processing of this msg entry is stored in *resgr0. The
 * caller needs to detect this situation and should invoke ap_dqap
 * with a valid resgr0 ptr and a value in there != 0 to indicate that
 * *resgr0 is to be used instead of qid to further process this entry.
 */
static inline struct ap_queue_status ap_dqap(ap_qid_t qid,
					     unsigned long *psmid,
					     void *msg, size_t msglen,
					     size_t *length,
					     size_t *reslength,
					     unsigned long *resgr0)
{
	unsigned long reg0 = resgr0 && *resgr0 ? *resgr0 : qid | 0x80000000UL;
	union ap_queue_status_reg reg1;
	unsigned long reg2;
	union register_pair rp1, rp2;

	rp1.even = 0UL;
	rp1.odd  = 0UL;
	rp2.even = (unsigned long)msg;
	rp2.odd  = (unsigned long)msglen;

	asm volatile(
		"	lgr	0,%[reg0]\n"   /* qid param into gr0 */
		"	lghi	2,0\n"	       /* 0 into gr2 (res length) */
		"0:	ltgr	%N[rp2],%N[rp2]\n" /* check buf len */
		"	jz	2f\n"	       /* go out if buf len is 0 */
		"1:	.insn	rre,0xb2ae0000,%[rp1],%[rp2]\n"
		"	brc	6,0b\n"        /* handle partial complete */
		"2:	lgr	%[reg0],0\n"   /* gr0 (qid + info) into reg0 */
		"	lgr	%[reg1],1\n"   /* gr1 (status) into reg1 */
		"	lgr	%[reg2],2\n"   /* gr2 (res length) into reg2 */
		: [reg0] "+&d" (reg0), [reg1] "=&d" (reg1.value),
		  [reg2] "=&d" (reg2), [rp1] "+&d" (rp1.pair),
		  [rp2] "+&d" (rp2.pair)
		:
		: "cc", "memory", "0", "1", "2");

	if (reslength)
		*reslength = reg2;
	if (reg2 != 0 && rp2.odd == 0) {
		/*
		 * Partially complete, status in gr1 is not set.
		 * Signal the caller that this dqap is only partially received
		 * with a special status response code 0xFF and *resgr0 updated
		 */
		reg1.status.response_code = 0xFF;
		if (resgr0)
			*resgr0 = reg0;
	} else {
		*psmid = (rp1.even << 32) + rp1.odd;
		if (resgr0)
			*resgr0 = 0;
	}

	/* update *length with the nr of bytes stored into the msg buffer */
	if (length)
		*length = msglen - rp2.odd;

	return reg1.status;
}

/*
 * Interface to tell the AP bus code that a configuration
 * change has happened. The bus code should at least do
 * an ap bus resource rescan.
 */
#if IS_ENABLED(CONFIG_ZCRYPT)
void ap_bus_cfg_chg(void);
#else
static inline void ap_bus_cfg_chg(void){}
#endif

#endif /* _ASM_S390_AP_H_ */

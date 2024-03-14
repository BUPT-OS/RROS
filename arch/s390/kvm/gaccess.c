// SPDX-License-Identifier: GPL-2.0
/*
 * guest access functions
 *
 * Copyright IBM Corp. 2014
 *
 */

#include <linux/vmalloc.h>
#include <linux/mm_types.h>
#include <linux/err.h>
#include <linux/pgtable.h>
#include <linux/bitfield.h>

#include <asm/gmap.h>
#include "kvm-s390.h"
#include "gaccess.h"
#include <asm/switch_to.h>

union asce {
	unsigned long val;
	struct {
		unsigned long origin : 52; /* Region- or Segment-Table Origin */
		unsigned long	 : 2;
		unsigned long g  : 1; /* Subspace Group Control */
		unsigned long p  : 1; /* Private Space Control */
		unsigned long s  : 1; /* Storage-Alteration-Event Control */
		unsigned long x  : 1; /* Space-Switch-Event Control */
		unsigned long r  : 1; /* Real-Space Control */
		unsigned long	 : 1;
		unsigned long dt : 2; /* Designation-Type Control */
		unsigned long tl : 2; /* Region- or Segment-Table Length */
	};
};

enum {
	ASCE_TYPE_SEGMENT = 0,
	ASCE_TYPE_REGION3 = 1,
	ASCE_TYPE_REGION2 = 2,
	ASCE_TYPE_REGION1 = 3
};

union region1_table_entry {
	unsigned long val;
	struct {
		unsigned long rto: 52;/* Region-Table Origin */
		unsigned long	 : 2;
		unsigned long p  : 1; /* DAT-Protection Bit */
		unsigned long	 : 1;
		unsigned long tf : 2; /* Region-Second-Table Offset */
		unsigned long i  : 1; /* Region-Invalid Bit */
		unsigned long	 : 1;
		unsigned long tt : 2; /* Table-Type Bits */
		unsigned long tl : 2; /* Region-Second-Table Length */
	};
};

union region2_table_entry {
	unsigned long val;
	struct {
		unsigned long rto: 52;/* Region-Table Origin */
		unsigned long	 : 2;
		unsigned long p  : 1; /* DAT-Protection Bit */
		unsigned long	 : 1;
		unsigned long tf : 2; /* Region-Third-Table Offset */
		unsigned long i  : 1; /* Region-Invalid Bit */
		unsigned long	 : 1;
		unsigned long tt : 2; /* Table-Type Bits */
		unsigned long tl : 2; /* Region-Third-Table Length */
	};
};

struct region3_table_entry_fc0 {
	unsigned long sto: 52;/* Segment-Table Origin */
	unsigned long	 : 1;
	unsigned long fc : 1; /* Format-Control */
	unsigned long p  : 1; /* DAT-Protection Bit */
	unsigned long	 : 1;
	unsigned long tf : 2; /* Segment-Table Offset */
	unsigned long i  : 1; /* Region-Invalid Bit */
	unsigned long cr : 1; /* Common-Region Bit */
	unsigned long tt : 2; /* Table-Type Bits */
	unsigned long tl : 2; /* Segment-Table Length */
};

struct region3_table_entry_fc1 {
	unsigned long rfaa : 33; /* Region-Frame Absolute Address */
	unsigned long	 : 14;
	unsigned long av : 1; /* ACCF-Validity Control */
	unsigned long acc: 4; /* Access-Control Bits */
	unsigned long f  : 1; /* Fetch-Protection Bit */
	unsigned long fc : 1; /* Format-Control */
	unsigned long p  : 1; /* DAT-Protection Bit */
	unsigned long iep: 1; /* Instruction-Execution-Protection */
	unsigned long	 : 2;
	unsigned long i  : 1; /* Region-Invalid Bit */
	unsigned long cr : 1; /* Common-Region Bit */
	unsigned long tt : 2; /* Table-Type Bits */
	unsigned long	 : 2;
};

union region3_table_entry {
	unsigned long val;
	struct region3_table_entry_fc0 fc0;
	struct region3_table_entry_fc1 fc1;
	struct {
		unsigned long	 : 53;
		unsigned long fc : 1; /* Format-Control */
		unsigned long	 : 4;
		unsigned long i  : 1; /* Region-Invalid Bit */
		unsigned long cr : 1; /* Common-Region Bit */
		unsigned long tt : 2; /* Table-Type Bits */
		unsigned long	 : 2;
	};
};

struct segment_entry_fc0 {
	unsigned long pto: 53;/* Page-Table Origin */
	unsigned long fc : 1; /* Format-Control */
	unsigned long p  : 1; /* DAT-Protection Bit */
	unsigned long	 : 3;
	unsigned long i  : 1; /* Segment-Invalid Bit */
	unsigned long cs : 1; /* Common-Segment Bit */
	unsigned long tt : 2; /* Table-Type Bits */
	unsigned long	 : 2;
};

struct segment_entry_fc1 {
	unsigned long sfaa : 44; /* Segment-Frame Absolute Address */
	unsigned long	 : 3;
	unsigned long av : 1; /* ACCF-Validity Control */
	unsigned long acc: 4; /* Access-Control Bits */
	unsigned long f  : 1; /* Fetch-Protection Bit */
	unsigned long fc : 1; /* Format-Control */
	unsigned long p  : 1; /* DAT-Protection Bit */
	unsigned long iep: 1; /* Instruction-Execution-Protection */
	unsigned long	 : 2;
	unsigned long i  : 1; /* Segment-Invalid Bit */
	unsigned long cs : 1; /* Common-Segment Bit */
	unsigned long tt : 2; /* Table-Type Bits */
	unsigned long	 : 2;
};

union segment_table_entry {
	unsigned long val;
	struct segment_entry_fc0 fc0;
	struct segment_entry_fc1 fc1;
	struct {
		unsigned long	 : 53;
		unsigned long fc : 1; /* Format-Control */
		unsigned long	 : 4;
		unsigned long i  : 1; /* Segment-Invalid Bit */
		unsigned long cs : 1; /* Common-Segment Bit */
		unsigned long tt : 2; /* Table-Type Bits */
		unsigned long	 : 2;
	};
};

enum {
	TABLE_TYPE_SEGMENT = 0,
	TABLE_TYPE_REGION3 = 1,
	TABLE_TYPE_REGION2 = 2,
	TABLE_TYPE_REGION1 = 3
};

union page_table_entry {
	unsigned long val;
	struct {
		unsigned long pfra : 52; /* Page-Frame Real Address */
		unsigned long z  : 1; /* Zero Bit */
		unsigned long i  : 1; /* Page-Invalid Bit */
		unsigned long p  : 1; /* DAT-Protection Bit */
		unsigned long iep: 1; /* Instruction-Execution-Protection */
		unsigned long	 : 8;
	};
};

/*
 * vaddress union in order to easily decode a virtual address into its
 * region first index, region second index etc. parts.
 */
union vaddress {
	unsigned long addr;
	struct {
		unsigned long rfx : 11;
		unsigned long rsx : 11;
		unsigned long rtx : 11;
		unsigned long sx  : 11;
		unsigned long px  : 8;
		unsigned long bx  : 12;
	};
	struct {
		unsigned long rfx01 : 2;
		unsigned long	    : 9;
		unsigned long rsx01 : 2;
		unsigned long	    : 9;
		unsigned long rtx01 : 2;
		unsigned long	    : 9;
		unsigned long sx01  : 2;
		unsigned long	    : 29;
	};
};

/*
 * raddress union which will contain the result (real or absolute address)
 * after a page table walk. The rfaa, sfaa and pfra members are used to
 * simply assign them the value of a region, segment or page table entry.
 */
union raddress {
	unsigned long addr;
	unsigned long rfaa : 33; /* Region-Frame Absolute Address */
	unsigned long sfaa : 44; /* Segment-Frame Absolute Address */
	unsigned long pfra : 52; /* Page-Frame Real Address */
};

union alet {
	u32 val;
	struct {
		u32 reserved : 7;
		u32 p        : 1;
		u32 alesn    : 8;
		u32 alen     : 16;
	};
};

union ald {
	u32 val;
	struct {
		u32     : 1;
		u32 alo : 24;
		u32 all : 7;
	};
};

struct ale {
	unsigned long i      : 1; /* ALEN-Invalid Bit */
	unsigned long        : 5;
	unsigned long fo     : 1; /* Fetch-Only Bit */
	unsigned long p      : 1; /* Private Bit */
	unsigned long alesn  : 8; /* Access-List-Entry Sequence Number */
	unsigned long aleax  : 16; /* Access-List-Entry Authorization Index */
	unsigned long        : 32;
	unsigned long        : 1;
	unsigned long asteo  : 25; /* ASN-Second-Table-Entry Origin */
	unsigned long        : 6;
	unsigned long astesn : 32; /* ASTE Sequence Number */
};

struct aste {
	unsigned long i      : 1; /* ASX-Invalid Bit */
	unsigned long ato    : 29; /* Authority-Table Origin */
	unsigned long        : 1;
	unsigned long b      : 1; /* Base-Space Bit */
	unsigned long ax     : 16; /* Authorization Index */
	unsigned long atl    : 12; /* Authority-Table Length */
	unsigned long        : 2;
	unsigned long ca     : 1; /* Controlled-ASN Bit */
	unsigned long ra     : 1; /* Reusable-ASN Bit */
	unsigned long asce   : 64; /* Address-Space-Control Element */
	unsigned long ald    : 32;
	unsigned long astesn : 32;
	/* .. more fields there */
};

int ipte_lock_held(struct kvm *kvm)
{
	if (sclp.has_siif) {
		int rc;

		read_lock(&kvm->arch.sca_lock);
		rc = kvm_s390_get_ipte_control(kvm)->kh != 0;
		read_unlock(&kvm->arch.sca_lock);
		return rc;
	}
	return kvm->arch.ipte_lock_count != 0;
}

static void ipte_lock_simple(struct kvm *kvm)
{
	union ipte_control old, new, *ic;

	mutex_lock(&kvm->arch.ipte_mutex);
	kvm->arch.ipte_lock_count++;
	if (kvm->arch.ipte_lock_count > 1)
		goto out;
retry:
	read_lock(&kvm->arch.sca_lock);
	ic = kvm_s390_get_ipte_control(kvm);
	do {
		old = READ_ONCE(*ic);
		if (old.k) {
			read_unlock(&kvm->arch.sca_lock);
			cond_resched();
			goto retry;
		}
		new = old;
		new.k = 1;
	} while (cmpxchg(&ic->val, old.val, new.val) != old.val);
	read_unlock(&kvm->arch.sca_lock);
out:
	mutex_unlock(&kvm->arch.ipte_mutex);
}

static void ipte_unlock_simple(struct kvm *kvm)
{
	union ipte_control old, new, *ic;

	mutex_lock(&kvm->arch.ipte_mutex);
	kvm->arch.ipte_lock_count--;
	if (kvm->arch.ipte_lock_count)
		goto out;
	read_lock(&kvm->arch.sca_lock);
	ic = kvm_s390_get_ipte_control(kvm);
	do {
		old = READ_ONCE(*ic);
		new = old;
		new.k = 0;
	} while (cmpxchg(&ic->val, old.val, new.val) != old.val);
	read_unlock(&kvm->arch.sca_lock);
	wake_up(&kvm->arch.ipte_wq);
out:
	mutex_unlock(&kvm->arch.ipte_mutex);
}

static void ipte_lock_siif(struct kvm *kvm)
{
	union ipte_control old, new, *ic;

retry:
	read_lock(&kvm->arch.sca_lock);
	ic = kvm_s390_get_ipte_control(kvm);
	do {
		old = READ_ONCE(*ic);
		if (old.kg) {
			read_unlock(&kvm->arch.sca_lock);
			cond_resched();
			goto retry;
		}
		new = old;
		new.k = 1;
		new.kh++;
	} while (cmpxchg(&ic->val, old.val, new.val) != old.val);
	read_unlock(&kvm->arch.sca_lock);
}

static void ipte_unlock_siif(struct kvm *kvm)
{
	union ipte_control old, new, *ic;

	read_lock(&kvm->arch.sca_lock);
	ic = kvm_s390_get_ipte_control(kvm);
	do {
		old = READ_ONCE(*ic);
		new = old;
		new.kh--;
		if (!new.kh)
			new.k = 0;
	} while (cmpxchg(&ic->val, old.val, new.val) != old.val);
	read_unlock(&kvm->arch.sca_lock);
	if (!new.kh)
		wake_up(&kvm->arch.ipte_wq);
}

void ipte_lock(struct kvm *kvm)
{
	if (sclp.has_siif)
		ipte_lock_siif(kvm);
	else
		ipte_lock_simple(kvm);
}

void ipte_unlock(struct kvm *kvm)
{
	if (sclp.has_siif)
		ipte_unlock_siif(kvm);
	else
		ipte_unlock_simple(kvm);
}

static int ar_translation(struct kvm_vcpu *vcpu, union asce *asce, u8 ar,
			  enum gacc_mode mode)
{
	union alet alet;
	struct ale ale;
	struct aste aste;
	unsigned long ald_addr, authority_table_addr;
	union ald ald;
	int eax, rc;
	u8 authority_table;

	if (ar >= NUM_ACRS)
		return -EINVAL;

	save_access_regs(vcpu->run->s.regs.acrs);
	alet.val = vcpu->run->s.regs.acrs[ar];

	if (ar == 0 || alet.val == 0) {
		asce->val = vcpu->arch.sie_block->gcr[1];
		return 0;
	} else if (alet.val == 1) {
		asce->val = vcpu->arch.sie_block->gcr[7];
		return 0;
	}

	if (alet.reserved)
		return PGM_ALET_SPECIFICATION;

	if (alet.p)
		ald_addr = vcpu->arch.sie_block->gcr[5];
	else
		ald_addr = vcpu->arch.sie_block->gcr[2];
	ald_addr &= 0x7fffffc0;

	rc = read_guest_real(vcpu, ald_addr + 16, &ald.val, sizeof(union ald));
	if (rc)
		return rc;

	if (alet.alen / 8 > ald.all)
		return PGM_ALEN_TRANSLATION;

	if (0x7fffffff - ald.alo * 128 < alet.alen * 16)
		return PGM_ADDRESSING;

	rc = read_guest_real(vcpu, ald.alo * 128 + alet.alen * 16, &ale,
			     sizeof(struct ale));
	if (rc)
		return rc;

	if (ale.i == 1)
		return PGM_ALEN_TRANSLATION;
	if (ale.alesn != alet.alesn)
		return PGM_ALE_SEQUENCE;

	rc = read_guest_real(vcpu, ale.asteo * 64, &aste, sizeof(struct aste));
	if (rc)
		return rc;

	if (aste.i)
		return PGM_ASTE_VALIDITY;
	if (aste.astesn != ale.astesn)
		return PGM_ASTE_SEQUENCE;

	if (ale.p == 1) {
		eax = (vcpu->arch.sie_block->gcr[8] >> 16) & 0xffff;
		if (ale.aleax != eax) {
			if (eax / 16 > aste.atl)
				return PGM_EXTENDED_AUTHORITY;

			authority_table_addr = aste.ato * 4 + eax / 4;

			rc = read_guest_real(vcpu, authority_table_addr,
					     &authority_table,
					     sizeof(u8));
			if (rc)
				return rc;

			if ((authority_table & (0x40 >> ((eax & 3) * 2))) == 0)
				return PGM_EXTENDED_AUTHORITY;
		}
	}

	if (ale.fo == 1 && mode == GACC_STORE)
		return PGM_PROTECTION;

	asce->val = aste.asce;
	return 0;
}

struct trans_exc_code_bits {
	unsigned long addr : 52; /* Translation-exception Address */
	unsigned long fsi  : 2;  /* Access Exception Fetch/Store Indication */
	unsigned long	   : 2;
	unsigned long b56  : 1;
	unsigned long	   : 3;
	unsigned long b60  : 1;
	unsigned long b61  : 1;
	unsigned long as   : 2;  /* ASCE Identifier */
};

enum {
	FSI_UNKNOWN = 0, /* Unknown whether fetch or store */
	FSI_STORE   = 1, /* Exception was due to store operation */
	FSI_FETCH   = 2  /* Exception was due to fetch operation */
};

enum prot_type {
	PROT_TYPE_LA   = 0,
	PROT_TYPE_KEYC = 1,
	PROT_TYPE_ALC  = 2,
	PROT_TYPE_DAT  = 3,
	PROT_TYPE_IEP  = 4,
	/* Dummy value for passing an initialized value when code != PGM_PROTECTION */
	PROT_NONE,
};

static int trans_exc_ending(struct kvm_vcpu *vcpu, int code, unsigned long gva, u8 ar,
			    enum gacc_mode mode, enum prot_type prot, bool terminate)
{
	struct kvm_s390_pgm_info *pgm = &vcpu->arch.pgm;
	struct trans_exc_code_bits *tec;

	memset(pgm, 0, sizeof(*pgm));
	pgm->code = code;
	tec = (struct trans_exc_code_bits *)&pgm->trans_exc_code;

	switch (code) {
	case PGM_PROTECTION:
		switch (prot) {
		case PROT_NONE:
			/* We should never get here, acts like termination */
			WARN_ON_ONCE(1);
			break;
		case PROT_TYPE_IEP:
			tec->b61 = 1;
			fallthrough;
		case PROT_TYPE_LA:
			tec->b56 = 1;
			break;
		case PROT_TYPE_KEYC:
			tec->b60 = 1;
			break;
		case PROT_TYPE_ALC:
			tec->b60 = 1;
			fallthrough;
		case PROT_TYPE_DAT:
			tec->b61 = 1;
			break;
		}
		if (terminate) {
			tec->b56 = 0;
			tec->b60 = 0;
			tec->b61 = 0;
		}
		fallthrough;
	case PGM_ASCE_TYPE:
	case PGM_PAGE_TRANSLATION:
	case PGM_REGION_FIRST_TRANS:
	case PGM_REGION_SECOND_TRANS:
	case PGM_REGION_THIRD_TRANS:
	case PGM_SEGMENT_TRANSLATION:
		/*
		 * op_access_id only applies to MOVE_PAGE -> set bit 61
		 * exc_access_id has to be set to 0 for some instructions. Both
		 * cases have to be handled by the caller.
		 */
		tec->addr = gva >> PAGE_SHIFT;
		tec->fsi = mode == GACC_STORE ? FSI_STORE : FSI_FETCH;
		tec->as = psw_bits(vcpu->arch.sie_block->gpsw).as;
		fallthrough;
	case PGM_ALEN_TRANSLATION:
	case PGM_ALE_SEQUENCE:
	case PGM_ASTE_VALIDITY:
	case PGM_ASTE_SEQUENCE:
	case PGM_EXTENDED_AUTHORITY:
		/*
		 * We can always store exc_access_id, as it is
		 * undefined for non-ar cases. It is undefined for
		 * most DAT protection exceptions.
		 */
		pgm->exc_access_id = ar;
		break;
	}
	return code;
}

static int trans_exc(struct kvm_vcpu *vcpu, int code, unsigned long gva, u8 ar,
		     enum gacc_mode mode, enum prot_type prot)
{
	return trans_exc_ending(vcpu, code, gva, ar, mode, prot, false);
}

static int get_vcpu_asce(struct kvm_vcpu *vcpu, union asce *asce,
			 unsigned long ga, u8 ar, enum gacc_mode mode)
{
	int rc;
	struct psw_bits psw = psw_bits(vcpu->arch.sie_block->gpsw);

	if (!psw.dat) {
		asce->val = 0;
		asce->r = 1;
		return 0;
	}

	if ((mode == GACC_IFETCH) && (psw.as != PSW_BITS_AS_HOME))
		psw.as = PSW_BITS_AS_PRIMARY;

	switch (psw.as) {
	case PSW_BITS_AS_PRIMARY:
		asce->val = vcpu->arch.sie_block->gcr[1];
		return 0;
	case PSW_BITS_AS_SECONDARY:
		asce->val = vcpu->arch.sie_block->gcr[7];
		return 0;
	case PSW_BITS_AS_HOME:
		asce->val = vcpu->arch.sie_block->gcr[13];
		return 0;
	case PSW_BITS_AS_ACCREG:
		rc = ar_translation(vcpu, asce, ar, mode);
		if (rc > 0)
			return trans_exc(vcpu, rc, ga, ar, mode, PROT_TYPE_ALC);
		return rc;
	}
	return 0;
}

static int deref_table(struct kvm *kvm, unsigned long gpa, unsigned long *val)
{
	return kvm_read_guest(kvm, gpa, val, sizeof(*val));
}

/**
 * guest_translate - translate a guest virtual into a guest absolute address
 * @vcpu: virtual cpu
 * @gva: guest virtual address
 * @gpa: points to where guest physical (absolute) address should be stored
 * @asce: effective asce
 * @mode: indicates the access mode to be used
 * @prot: returns the type for protection exceptions
 *
 * Translate a guest virtual address into a guest absolute address by means
 * of dynamic address translation as specified by the architecture.
 * If the resulting absolute address is not available in the configuration
 * an addressing exception is indicated and @gpa will not be changed.
 *
 * Returns: - zero on success; @gpa contains the resulting absolute address
 *	    - a negative value if guest access failed due to e.g. broken
 *	      guest mapping
 *	    - a positive value if an access exception happened. In this case
 *	      the returned value is the program interruption code as defined
 *	      by the architecture
 */
static unsigned long guest_translate(struct kvm_vcpu *vcpu, unsigned long gva,
				     unsigned long *gpa, const union asce asce,
				     enum gacc_mode mode, enum prot_type *prot)
{
	union vaddress vaddr = {.addr = gva};
	union raddress raddr = {.addr = gva};
	union page_table_entry pte;
	int dat_protection = 0;
	int iep_protection = 0;
	union ctlreg0 ctlreg0;
	unsigned long ptr;
	int edat1, edat2, iep;

	ctlreg0.val = vcpu->arch.sie_block->gcr[0];
	edat1 = ctlreg0.edat && test_kvm_facility(vcpu->kvm, 8);
	edat2 = edat1 && test_kvm_facility(vcpu->kvm, 78);
	iep = ctlreg0.iep && test_kvm_facility(vcpu->kvm, 130);
	if (asce.r)
		goto real_address;
	ptr = asce.origin * PAGE_SIZE;
	switch (asce.dt) {
	case ASCE_TYPE_REGION1:
		if (vaddr.rfx01 > asce.tl)
			return PGM_REGION_FIRST_TRANS;
		ptr += vaddr.rfx * 8;
		break;
	case ASCE_TYPE_REGION2:
		if (vaddr.rfx)
			return PGM_ASCE_TYPE;
		if (vaddr.rsx01 > asce.tl)
			return PGM_REGION_SECOND_TRANS;
		ptr += vaddr.rsx * 8;
		break;
	case ASCE_TYPE_REGION3:
		if (vaddr.rfx || vaddr.rsx)
			return PGM_ASCE_TYPE;
		if (vaddr.rtx01 > asce.tl)
			return PGM_REGION_THIRD_TRANS;
		ptr += vaddr.rtx * 8;
		break;
	case ASCE_TYPE_SEGMENT:
		if (vaddr.rfx || vaddr.rsx || vaddr.rtx)
			return PGM_ASCE_TYPE;
		if (vaddr.sx01 > asce.tl)
			return PGM_SEGMENT_TRANSLATION;
		ptr += vaddr.sx * 8;
		break;
	}
	switch (asce.dt) {
	case ASCE_TYPE_REGION1:	{
		union region1_table_entry rfte;

		if (kvm_is_error_gpa(vcpu->kvm, ptr))
			return PGM_ADDRESSING;
		if (deref_table(vcpu->kvm, ptr, &rfte.val))
			return -EFAULT;
		if (rfte.i)
			return PGM_REGION_FIRST_TRANS;
		if (rfte.tt != TABLE_TYPE_REGION1)
			return PGM_TRANSLATION_SPEC;
		if (vaddr.rsx01 < rfte.tf || vaddr.rsx01 > rfte.tl)
			return PGM_REGION_SECOND_TRANS;
		if (edat1)
			dat_protection |= rfte.p;
		ptr = rfte.rto * PAGE_SIZE + vaddr.rsx * 8;
	}
		fallthrough;
	case ASCE_TYPE_REGION2: {
		union region2_table_entry rste;

		if (kvm_is_error_gpa(vcpu->kvm, ptr))
			return PGM_ADDRESSING;
		if (deref_table(vcpu->kvm, ptr, &rste.val))
			return -EFAULT;
		if (rste.i)
			return PGM_REGION_SECOND_TRANS;
		if (rste.tt != TABLE_TYPE_REGION2)
			return PGM_TRANSLATION_SPEC;
		if (vaddr.rtx01 < rste.tf || vaddr.rtx01 > rste.tl)
			return PGM_REGION_THIRD_TRANS;
		if (edat1)
			dat_protection |= rste.p;
		ptr = rste.rto * PAGE_SIZE + vaddr.rtx * 8;
	}
		fallthrough;
	case ASCE_TYPE_REGION3: {
		union region3_table_entry rtte;

		if (kvm_is_error_gpa(vcpu->kvm, ptr))
			return PGM_ADDRESSING;
		if (deref_table(vcpu->kvm, ptr, &rtte.val))
			return -EFAULT;
		if (rtte.i)
			return PGM_REGION_THIRD_TRANS;
		if (rtte.tt != TABLE_TYPE_REGION3)
			return PGM_TRANSLATION_SPEC;
		if (rtte.cr && asce.p && edat2)
			return PGM_TRANSLATION_SPEC;
		if (rtte.fc && edat2) {
			dat_protection |= rtte.fc1.p;
			iep_protection = rtte.fc1.iep;
			raddr.rfaa = rtte.fc1.rfaa;
			goto absolute_address;
		}
		if (vaddr.sx01 < rtte.fc0.tf)
			return PGM_SEGMENT_TRANSLATION;
		if (vaddr.sx01 > rtte.fc0.tl)
			return PGM_SEGMENT_TRANSLATION;
		if (edat1)
			dat_protection |= rtte.fc0.p;
		ptr = rtte.fc0.sto * PAGE_SIZE + vaddr.sx * 8;
	}
		fallthrough;
	case ASCE_TYPE_SEGMENT: {
		union segment_table_entry ste;

		if (kvm_is_error_gpa(vcpu->kvm, ptr))
			return PGM_ADDRESSING;
		if (deref_table(vcpu->kvm, ptr, &ste.val))
			return -EFAULT;
		if (ste.i)
			return PGM_SEGMENT_TRANSLATION;
		if (ste.tt != TABLE_TYPE_SEGMENT)
			return PGM_TRANSLATION_SPEC;
		if (ste.cs && asce.p)
			return PGM_TRANSLATION_SPEC;
		if (ste.fc && edat1) {
			dat_protection |= ste.fc1.p;
			iep_protection = ste.fc1.iep;
			raddr.sfaa = ste.fc1.sfaa;
			goto absolute_address;
		}
		dat_protection |= ste.fc0.p;
		ptr = ste.fc0.pto * (PAGE_SIZE / 2) + vaddr.px * 8;
	}
	}
	if (kvm_is_error_gpa(vcpu->kvm, ptr))
		return PGM_ADDRESSING;
	if (deref_table(vcpu->kvm, ptr, &pte.val))
		return -EFAULT;
	if (pte.i)
		return PGM_PAGE_TRANSLATION;
	if (pte.z)
		return PGM_TRANSLATION_SPEC;
	dat_protection |= pte.p;
	iep_protection = pte.iep;
	raddr.pfra = pte.pfra;
real_address:
	raddr.addr = kvm_s390_real_to_abs(vcpu, raddr.addr);
absolute_address:
	if (mode == GACC_STORE && dat_protection) {
		*prot = PROT_TYPE_DAT;
		return PGM_PROTECTION;
	}
	if (mode == GACC_IFETCH && iep_protection && iep) {
		*prot = PROT_TYPE_IEP;
		return PGM_PROTECTION;
	}
	if (kvm_is_error_gpa(vcpu->kvm, raddr.addr))
		return PGM_ADDRESSING;
	*gpa = raddr.addr;
	return 0;
}

static inline int is_low_address(unsigned long ga)
{
	/* Check for address ranges 0..511 and 4096..4607 */
	return (ga & ~0x11fful) == 0;
}

static int low_address_protection_enabled(struct kvm_vcpu *vcpu,
					  const union asce asce)
{
	union ctlreg0 ctlreg0 = {.val = vcpu->arch.sie_block->gcr[0]};
	psw_t *psw = &vcpu->arch.sie_block->gpsw;

	if (!ctlreg0.lap)
		return 0;
	if (psw_bits(*psw).dat && asce.p)
		return 0;
	return 1;
}

static int vm_check_access_key(struct kvm *kvm, u8 access_key,
			       enum gacc_mode mode, gpa_t gpa)
{
	u8 storage_key, access_control;
	bool fetch_protected;
	unsigned long hva;
	int r;

	if (access_key == 0)
		return 0;

	hva = gfn_to_hva(kvm, gpa_to_gfn(gpa));
	if (kvm_is_error_hva(hva))
		return PGM_ADDRESSING;

	mmap_read_lock(current->mm);
	r = get_guest_storage_key(current->mm, hva, &storage_key);
	mmap_read_unlock(current->mm);
	if (r)
		return r;
	access_control = FIELD_GET(_PAGE_ACC_BITS, storage_key);
	if (access_control == access_key)
		return 0;
	fetch_protected = storage_key & _PAGE_FP_BIT;
	if ((mode == GACC_FETCH || mode == GACC_IFETCH) && !fetch_protected)
		return 0;
	return PGM_PROTECTION;
}

static bool fetch_prot_override_applicable(struct kvm_vcpu *vcpu, enum gacc_mode mode,
					   union asce asce)
{
	psw_t *psw = &vcpu->arch.sie_block->gpsw;
	unsigned long override;

	if (mode == GACC_FETCH || mode == GACC_IFETCH) {
		/* check if fetch protection override enabled */
		override = vcpu->arch.sie_block->gcr[0];
		override &= CR0_FETCH_PROTECTION_OVERRIDE;
		/* not applicable if subject to DAT && private space */
		override = override && !(psw_bits(*psw).dat && asce.p);
		return override;
	}
	return false;
}

static bool fetch_prot_override_applies(unsigned long ga, unsigned int len)
{
	return ga < 2048 && ga + len <= 2048;
}

static bool storage_prot_override_applicable(struct kvm_vcpu *vcpu)
{
	/* check if storage protection override enabled */
	return vcpu->arch.sie_block->gcr[0] & CR0_STORAGE_PROTECTION_OVERRIDE;
}

static bool storage_prot_override_applies(u8 access_control)
{
	/* matches special storage protection override key (9) -> allow */
	return access_control == PAGE_SPO_ACC;
}

static int vcpu_check_access_key(struct kvm_vcpu *vcpu, u8 access_key,
				 enum gacc_mode mode, union asce asce, gpa_t gpa,
				 unsigned long ga, unsigned int len)
{
	u8 storage_key, access_control;
	unsigned long hva;
	int r;

	/* access key 0 matches any storage key -> allow */
	if (access_key == 0)
		return 0;
	/*
	 * caller needs to ensure that gfn is accessible, so we can
	 * assume that this cannot fail
	 */
	hva = gfn_to_hva(vcpu->kvm, gpa_to_gfn(gpa));
	mmap_read_lock(current->mm);
	r = get_guest_storage_key(current->mm, hva, &storage_key);
	mmap_read_unlock(current->mm);
	if (r)
		return r;
	access_control = FIELD_GET(_PAGE_ACC_BITS, storage_key);
	/* access key matches storage key -> allow */
	if (access_control == access_key)
		return 0;
	if (mode == GACC_FETCH || mode == GACC_IFETCH) {
		/* it is a fetch and fetch protection is off -> allow */
		if (!(storage_key & _PAGE_FP_BIT))
			return 0;
		if (fetch_prot_override_applicable(vcpu, mode, asce) &&
		    fetch_prot_override_applies(ga, len))
			return 0;
	}
	if (storage_prot_override_applicable(vcpu) &&
	    storage_prot_override_applies(access_control))
		return 0;
	return PGM_PROTECTION;
}

/**
 * guest_range_to_gpas() - Calculate guest physical addresses of page fragments
 * covering a logical range
 * @vcpu: virtual cpu
 * @ga: guest address, start of range
 * @ar: access register
 * @gpas: output argument, may be NULL
 * @len: length of range in bytes
 * @asce: address-space-control element to use for translation
 * @mode: access mode
 * @access_key: access key to mach the range's storage keys against
 *
 * Translate a logical range to a series of guest absolute addresses,
 * such that the concatenation of page fragments starting at each gpa make up
 * the whole range.
 * The translation is performed as if done by the cpu for the given @asce, @ar,
 * @mode and state of the @vcpu.
 * If the translation causes an exception, its program interruption code is
 * returned and the &struct kvm_s390_pgm_info pgm member of @vcpu is modified
 * such that a subsequent call to kvm_s390_inject_prog_vcpu() will inject
 * a correct exception into the guest.
 * The resulting gpas are stored into @gpas, unless it is NULL.
 *
 * Note: All fragments except the first one start at the beginning of a page.
 *	 When deriving the boundaries of a fragment from a gpa, all but the last
 *	 fragment end at the end of the page.
 *
 * Return:
 * * 0		- success
 * * <0		- translation could not be performed, for example if  guest
 *		  memory could not be accessed
 * * >0		- an access exception occurred. In this case the returned value
 *		  is the program interruption code and the contents of pgm may
 *		  be used to inject an exception into the guest.
 */
static int guest_range_to_gpas(struct kvm_vcpu *vcpu, unsigned long ga, u8 ar,
			       unsigned long *gpas, unsigned long len,
			       const union asce asce, enum gacc_mode mode,
			       u8 access_key)
{
	psw_t *psw = &vcpu->arch.sie_block->gpsw;
	unsigned int offset = offset_in_page(ga);
	unsigned int fragment_len;
	int lap_enabled, rc = 0;
	enum prot_type prot;
	unsigned long gpa;

	lap_enabled = low_address_protection_enabled(vcpu, asce);
	while (min(PAGE_SIZE - offset, len) > 0) {
		fragment_len = min(PAGE_SIZE - offset, len);
		ga = kvm_s390_logical_to_effective(vcpu, ga);
		if (mode == GACC_STORE && lap_enabled && is_low_address(ga))
			return trans_exc(vcpu, PGM_PROTECTION, ga, ar, mode,
					 PROT_TYPE_LA);
		if (psw_bits(*psw).dat) {
			rc = guest_translate(vcpu, ga, &gpa, asce, mode, &prot);
			if (rc < 0)
				return rc;
		} else {
			gpa = kvm_s390_real_to_abs(vcpu, ga);
			if (kvm_is_error_gpa(vcpu->kvm, gpa)) {
				rc = PGM_ADDRESSING;
				prot = PROT_NONE;
			}
		}
		if (rc)
			return trans_exc(vcpu, rc, ga, ar, mode, prot);
		rc = vcpu_check_access_key(vcpu, access_key, mode, asce, gpa, ga,
					   fragment_len);
		if (rc)
			return trans_exc(vcpu, rc, ga, ar, mode, PROT_TYPE_KEYC);
		if (gpas)
			*gpas++ = gpa;
		offset = 0;
		ga += fragment_len;
		len -= fragment_len;
	}
	return 0;
}

static int access_guest_page(struct kvm *kvm, enum gacc_mode mode, gpa_t gpa,
			     void *data, unsigned int len)
{
	const unsigned int offset = offset_in_page(gpa);
	const gfn_t gfn = gpa_to_gfn(gpa);
	int rc;

	if (mode == GACC_STORE)
		rc = kvm_write_guest_page(kvm, gfn, data, offset, len);
	else
		rc = kvm_read_guest_page(kvm, gfn, data, offset, len);
	return rc;
}

static int
access_guest_page_with_key(struct kvm *kvm, enum gacc_mode mode, gpa_t gpa,
			   void *data, unsigned int len, u8 access_key)
{
	struct kvm_memory_slot *slot;
	bool writable;
	gfn_t gfn;
	hva_t hva;
	int rc;

	gfn = gpa >> PAGE_SHIFT;
	slot = gfn_to_memslot(kvm, gfn);
	hva = gfn_to_hva_memslot_prot(slot, gfn, &writable);

	if (kvm_is_error_hva(hva))
		return PGM_ADDRESSING;
	/*
	 * Check if it's a ro memslot, even tho that can't occur (they're unsupported).
	 * Don't try to actually handle that case.
	 */
	if (!writable && mode == GACC_STORE)
		return -EOPNOTSUPP;
	hva += offset_in_page(gpa);
	if (mode == GACC_STORE)
		rc = copy_to_user_key((void __user *)hva, data, len, access_key);
	else
		rc = copy_from_user_key(data, (void __user *)hva, len, access_key);
	if (rc)
		return PGM_PROTECTION;
	if (mode == GACC_STORE)
		mark_page_dirty_in_slot(kvm, slot, gfn);
	return 0;
}

int access_guest_abs_with_key(struct kvm *kvm, gpa_t gpa, void *data,
			      unsigned long len, enum gacc_mode mode, u8 access_key)
{
	int offset = offset_in_page(gpa);
	int fragment_len;
	int rc;

	while (min(PAGE_SIZE - offset, len) > 0) {
		fragment_len = min(PAGE_SIZE - offset, len);
		rc = access_guest_page_with_key(kvm, mode, gpa, data, fragment_len, access_key);
		if (rc)
			return rc;
		offset = 0;
		len -= fragment_len;
		data += fragment_len;
		gpa += fragment_len;
	}
	return 0;
}

int access_guest_with_key(struct kvm_vcpu *vcpu, unsigned long ga, u8 ar,
			  void *data, unsigned long len, enum gacc_mode mode,
			  u8 access_key)
{
	psw_t *psw = &vcpu->arch.sie_block->gpsw;
	unsigned long nr_pages, idx;
	unsigned long gpa_array[2];
	unsigned int fragment_len;
	unsigned long *gpas;
	enum prot_type prot;
	int need_ipte_lock;
	union asce asce;
	bool try_storage_prot_override;
	bool try_fetch_prot_override;
	int rc;

	if (!len)
		return 0;
	ga = kvm_s390_logical_to_effective(vcpu, ga);
	rc = get_vcpu_asce(vcpu, &asce, ga, ar, mode);
	if (rc)
		return rc;
	nr_pages = (((ga & ~PAGE_MASK) + len - 1) >> PAGE_SHIFT) + 1;
	gpas = gpa_array;
	if (nr_pages > ARRAY_SIZE(gpa_array))
		gpas = vmalloc(array_size(nr_pages, sizeof(unsigned long)));
	if (!gpas)
		return -ENOMEM;
	try_fetch_prot_override = fetch_prot_override_applicable(vcpu, mode, asce);
	try_storage_prot_override = storage_prot_override_applicable(vcpu);
	need_ipte_lock = psw_bits(*psw).dat && !asce.r;
	if (need_ipte_lock)
		ipte_lock(vcpu->kvm);
	/*
	 * Since we do the access further down ultimately via a move instruction
	 * that does key checking and returns an error in case of a protection
	 * violation, we don't need to do the check during address translation.
	 * Skip it by passing access key 0, which matches any storage key,
	 * obviating the need for any further checks. As a result the check is
	 * handled entirely in hardware on access, we only need to take care to
	 * forego key protection checking if fetch protection override applies or
	 * retry with the special key 9 in case of storage protection override.
	 */
	rc = guest_range_to_gpas(vcpu, ga, ar, gpas, len, asce, mode, 0);
	if (rc)
		goto out_unlock;
	for (idx = 0; idx < nr_pages; idx++) {
		fragment_len = min(PAGE_SIZE - offset_in_page(gpas[idx]), len);
		if (try_fetch_prot_override && fetch_prot_override_applies(ga, fragment_len)) {
			rc = access_guest_page(vcpu->kvm, mode, gpas[idx],
					       data, fragment_len);
		} else {
			rc = access_guest_page_with_key(vcpu->kvm, mode, gpas[idx],
							data, fragment_len, access_key);
		}
		if (rc == PGM_PROTECTION && try_storage_prot_override)
			rc = access_guest_page_with_key(vcpu->kvm, mode, gpas[idx],
							data, fragment_len, PAGE_SPO_ACC);
		if (rc)
			break;
		len -= fragment_len;
		data += fragment_len;
		ga = kvm_s390_logical_to_effective(vcpu, ga + fragment_len);
	}
	if (rc > 0) {
		bool terminate = (mode == GACC_STORE) && (idx > 0);

		if (rc == PGM_PROTECTION)
			prot = PROT_TYPE_KEYC;
		else
			prot = PROT_NONE;
		rc = trans_exc_ending(vcpu, rc, ga, ar, mode, prot, terminate);
	}
out_unlock:
	if (need_ipte_lock)
		ipte_unlock(vcpu->kvm);
	if (nr_pages > ARRAY_SIZE(gpa_array))
		vfree(gpas);
	return rc;
}

int access_guest_real(struct kvm_vcpu *vcpu, unsigned long gra,
		      void *data, unsigned long len, enum gacc_mode mode)
{
	unsigned int fragment_len;
	unsigned long gpa;
	int rc = 0;

	while (len && !rc) {
		gpa = kvm_s390_real_to_abs(vcpu, gra);
		fragment_len = min(PAGE_SIZE - offset_in_page(gpa), len);
		rc = access_guest_page(vcpu->kvm, mode, gpa, data, fragment_len);
		len -= fragment_len;
		gra += fragment_len;
		data += fragment_len;
	}
	return rc;
}

/**
 * cmpxchg_guest_abs_with_key() - Perform cmpxchg on guest absolute address.
 * @kvm: Virtual machine instance.
 * @gpa: Absolute guest address of the location to be changed.
 * @len: Operand length of the cmpxchg, required: 1 <= len <= 16. Providing a
 *       non power of two will result in failure.
 * @old_addr: Pointer to old value. If the location at @gpa contains this value,
 *            the exchange will succeed. After calling cmpxchg_guest_abs_with_key()
 *            *@old_addr contains the value at @gpa before the attempt to
 *            exchange the value.
 * @new: The value to place at @gpa.
 * @access_key: The access key to use for the guest access.
 * @success: output value indicating if an exchange occurred.
 *
 * Atomically exchange the value at @gpa by @new, if it contains *@old.
 * Honors storage keys.
 *
 * Return: * 0: successful exchange
 *         * >0: a program interruption code indicating the reason cmpxchg could
 *               not be attempted
 *         * -EINVAL: address misaligned or len not power of two
 *         * -EAGAIN: transient failure (len 1 or 2)
 *         * -EOPNOTSUPP: read-only memslot (should never occur)
 */
int cmpxchg_guest_abs_with_key(struct kvm *kvm, gpa_t gpa, int len,
			       __uint128_t *old_addr, __uint128_t new,
			       u8 access_key, bool *success)
{
	gfn_t gfn = gpa_to_gfn(gpa);
	struct kvm_memory_slot *slot = gfn_to_memslot(kvm, gfn);
	bool writable;
	hva_t hva;
	int ret;

	if (!IS_ALIGNED(gpa, len))
		return -EINVAL;

	hva = gfn_to_hva_memslot_prot(slot, gfn, &writable);
	if (kvm_is_error_hva(hva))
		return PGM_ADDRESSING;
	/*
	 * Check if it's a read-only memslot, even though that cannot occur
	 * since those are unsupported.
	 * Don't try to actually handle that case.
	 */
	if (!writable)
		return -EOPNOTSUPP;

	hva += offset_in_page(gpa);
	/*
	 * The cmpxchg_user_key macro depends on the type of "old", so we need
	 * a case for each valid length and get some code duplication as long
	 * as we don't introduce a new macro.
	 */
	switch (len) {
	case 1: {
		u8 old;

		ret = cmpxchg_user_key((u8 __user *)hva, &old, *old_addr, new, access_key);
		*success = !ret && old == *old_addr;
		*old_addr = old;
		break;
	}
	case 2: {
		u16 old;

		ret = cmpxchg_user_key((u16 __user *)hva, &old, *old_addr, new, access_key);
		*success = !ret && old == *old_addr;
		*old_addr = old;
		break;
	}
	case 4: {
		u32 old;

		ret = cmpxchg_user_key((u32 __user *)hva, &old, *old_addr, new, access_key);
		*success = !ret && old == *old_addr;
		*old_addr = old;
		break;
	}
	case 8: {
		u64 old;

		ret = cmpxchg_user_key((u64 __user *)hva, &old, *old_addr, new, access_key);
		*success = !ret && old == *old_addr;
		*old_addr = old;
		break;
	}
	case 16: {
		__uint128_t old;

		ret = cmpxchg_user_key((__uint128_t __user *)hva, &old, *old_addr, new, access_key);
		*success = !ret && old == *old_addr;
		*old_addr = old;
		break;
	}
	default:
		return -EINVAL;
	}
	if (*success)
		mark_page_dirty_in_slot(kvm, slot, gfn);
	/*
	 * Assume that the fault is caused by protection, either key protection
	 * or user page write protection.
	 */
	if (ret == -EFAULT)
		ret = PGM_PROTECTION;
	return ret;
}

/**
 * guest_translate_address_with_key - translate guest logical into guest absolute address
 * @vcpu: virtual cpu
 * @gva: Guest virtual address
 * @ar: Access register
 * @gpa: Guest physical address
 * @mode: Translation access mode
 * @access_key: access key to mach the storage key with
 *
 * Parameter semantics are the same as the ones from guest_translate.
 * The memory contents at the guest address are not changed.
 *
 * Note: The IPTE lock is not taken during this function, so the caller
 * has to take care of this.
 */
int guest_translate_address_with_key(struct kvm_vcpu *vcpu, unsigned long gva, u8 ar,
				     unsigned long *gpa, enum gacc_mode mode,
				     u8 access_key)
{
	union asce asce;
	int rc;

	gva = kvm_s390_logical_to_effective(vcpu, gva);
	rc = get_vcpu_asce(vcpu, &asce, gva, ar, mode);
	if (rc)
		return rc;
	return guest_range_to_gpas(vcpu, gva, ar, gpa, 1, asce, mode,
				   access_key);
}

/**
 * check_gva_range - test a range of guest virtual addresses for accessibility
 * @vcpu: virtual cpu
 * @gva: Guest virtual address
 * @ar: Access register
 * @length: Length of test range
 * @mode: Translation access mode
 * @access_key: access key to mach the storage keys with
 */
int check_gva_range(struct kvm_vcpu *vcpu, unsigned long gva, u8 ar,
		    unsigned long length, enum gacc_mode mode, u8 access_key)
{
	union asce asce;
	int rc = 0;

	rc = get_vcpu_asce(vcpu, &asce, gva, ar, mode);
	if (rc)
		return rc;
	ipte_lock(vcpu->kvm);
	rc = guest_range_to_gpas(vcpu, gva, ar, NULL, length, asce, mode,
				 access_key);
	ipte_unlock(vcpu->kvm);

	return rc;
}

/**
 * check_gpa_range - test a range of guest physical addresses for accessibility
 * @kvm: virtual machine instance
 * @gpa: guest physical address
 * @length: length of test range
 * @mode: access mode to test, relevant for storage keys
 * @access_key: access key to mach the storage keys with
 */
int check_gpa_range(struct kvm *kvm, unsigned long gpa, unsigned long length,
		    enum gacc_mode mode, u8 access_key)
{
	unsigned int fragment_len;
	int rc = 0;

	while (length && !rc) {
		fragment_len = min(PAGE_SIZE - offset_in_page(gpa), length);
		rc = vm_check_access_key(kvm, access_key, mode, gpa);
		length -= fragment_len;
		gpa += fragment_len;
	}
	return rc;
}

/**
 * kvm_s390_check_low_addr_prot_real - check for low-address protection
 * @vcpu: virtual cpu
 * @gra: Guest real address
 *
 * Checks whether an address is subject to low-address protection and set
 * up vcpu->arch.pgm accordingly if necessary.
 *
 * Return: 0 if no protection exception, or PGM_PROTECTION if protected.
 */
int kvm_s390_check_low_addr_prot_real(struct kvm_vcpu *vcpu, unsigned long gra)
{
	union ctlreg0 ctlreg0 = {.val = vcpu->arch.sie_block->gcr[0]};

	if (!ctlreg0.lap || !is_low_address(gra))
		return 0;
	return trans_exc(vcpu, PGM_PROTECTION, gra, 0, GACC_STORE, PROT_TYPE_LA);
}

/**
 * kvm_s390_shadow_tables - walk the guest page table and create shadow tables
 * @sg: pointer to the shadow guest address space structure
 * @saddr: faulting address in the shadow gmap
 * @pgt: pointer to the beginning of the page table for the given address if
 *	 successful (return value 0), or to the first invalid DAT entry in
 *	 case of exceptions (return value > 0)
 * @dat_protection: referenced memory is write protected
 * @fake: pgt references contiguous guest memory block, not a pgtable
 */
static int kvm_s390_shadow_tables(struct gmap *sg, unsigned long saddr,
				  unsigned long *pgt, int *dat_protection,
				  int *fake)
{
	struct gmap *parent;
	union asce asce;
	union vaddress vaddr;
	unsigned long ptr;
	int rc;

	*fake = 0;
	*dat_protection = 0;
	parent = sg->parent;
	vaddr.addr = saddr;
	asce.val = sg->orig_asce;
	ptr = asce.origin * PAGE_SIZE;
	if (asce.r) {
		*fake = 1;
		ptr = 0;
		asce.dt = ASCE_TYPE_REGION1;
	}
	switch (asce.dt) {
	case ASCE_TYPE_REGION1:
		if (vaddr.rfx01 > asce.tl && !*fake)
			return PGM_REGION_FIRST_TRANS;
		break;
	case ASCE_TYPE_REGION2:
		if (vaddr.rfx)
			return PGM_ASCE_TYPE;
		if (vaddr.rsx01 > asce.tl)
			return PGM_REGION_SECOND_TRANS;
		break;
	case ASCE_TYPE_REGION3:
		if (vaddr.rfx || vaddr.rsx)
			return PGM_ASCE_TYPE;
		if (vaddr.rtx01 > asce.tl)
			return PGM_REGION_THIRD_TRANS;
		break;
	case ASCE_TYPE_SEGMENT:
		if (vaddr.rfx || vaddr.rsx || vaddr.rtx)
			return PGM_ASCE_TYPE;
		if (vaddr.sx01 > asce.tl)
			return PGM_SEGMENT_TRANSLATION;
		break;
	}

	switch (asce.dt) {
	case ASCE_TYPE_REGION1: {
		union region1_table_entry rfte;

		if (*fake) {
			ptr += vaddr.rfx * _REGION1_SIZE;
			rfte.val = ptr;
			goto shadow_r2t;
		}
		*pgt = ptr + vaddr.rfx * 8;
		rc = gmap_read_table(parent, ptr + vaddr.rfx * 8, &rfte.val);
		if (rc)
			return rc;
		if (rfte.i)
			return PGM_REGION_FIRST_TRANS;
		if (rfte.tt != TABLE_TYPE_REGION1)
			return PGM_TRANSLATION_SPEC;
		if (vaddr.rsx01 < rfte.tf || vaddr.rsx01 > rfte.tl)
			return PGM_REGION_SECOND_TRANS;
		if (sg->edat_level >= 1)
			*dat_protection |= rfte.p;
		ptr = rfte.rto * PAGE_SIZE;
shadow_r2t:
		rc = gmap_shadow_r2t(sg, saddr, rfte.val, *fake);
		if (rc)
			return rc;
	}
		fallthrough;
	case ASCE_TYPE_REGION2: {
		union region2_table_entry rste;

		if (*fake) {
			ptr += vaddr.rsx * _REGION2_SIZE;
			rste.val = ptr;
			goto shadow_r3t;
		}
		*pgt = ptr + vaddr.rsx * 8;
		rc = gmap_read_table(parent, ptr + vaddr.rsx * 8, &rste.val);
		if (rc)
			return rc;
		if (rste.i)
			return PGM_REGION_SECOND_TRANS;
		if (rste.tt != TABLE_TYPE_REGION2)
			return PGM_TRANSLATION_SPEC;
		if (vaddr.rtx01 < rste.tf || vaddr.rtx01 > rste.tl)
			return PGM_REGION_THIRD_TRANS;
		if (sg->edat_level >= 1)
			*dat_protection |= rste.p;
		ptr = rste.rto * PAGE_SIZE;
shadow_r3t:
		rste.p |= *dat_protection;
		rc = gmap_shadow_r3t(sg, saddr, rste.val, *fake);
		if (rc)
			return rc;
	}
		fallthrough;
	case ASCE_TYPE_REGION3: {
		union region3_table_entry rtte;

		if (*fake) {
			ptr += vaddr.rtx * _REGION3_SIZE;
			rtte.val = ptr;
			goto shadow_sgt;
		}
		*pgt = ptr + vaddr.rtx * 8;
		rc = gmap_read_table(parent, ptr + vaddr.rtx * 8, &rtte.val);
		if (rc)
			return rc;
		if (rtte.i)
			return PGM_REGION_THIRD_TRANS;
		if (rtte.tt != TABLE_TYPE_REGION3)
			return PGM_TRANSLATION_SPEC;
		if (rtte.cr && asce.p && sg->edat_level >= 2)
			return PGM_TRANSLATION_SPEC;
		if (rtte.fc && sg->edat_level >= 2) {
			*dat_protection |= rtte.fc0.p;
			*fake = 1;
			ptr = rtte.fc1.rfaa * _REGION3_SIZE;
			rtte.val = ptr;
			goto shadow_sgt;
		}
		if (vaddr.sx01 < rtte.fc0.tf || vaddr.sx01 > rtte.fc0.tl)
			return PGM_SEGMENT_TRANSLATION;
		if (sg->edat_level >= 1)
			*dat_protection |= rtte.fc0.p;
		ptr = rtte.fc0.sto * PAGE_SIZE;
shadow_sgt:
		rtte.fc0.p |= *dat_protection;
		rc = gmap_shadow_sgt(sg, saddr, rtte.val, *fake);
		if (rc)
			return rc;
	}
		fallthrough;
	case ASCE_TYPE_SEGMENT: {
		union segment_table_entry ste;

		if (*fake) {
			ptr += vaddr.sx * _SEGMENT_SIZE;
			ste.val = ptr;
			goto shadow_pgt;
		}
		*pgt = ptr + vaddr.sx * 8;
		rc = gmap_read_table(parent, ptr + vaddr.sx * 8, &ste.val);
		if (rc)
			return rc;
		if (ste.i)
			return PGM_SEGMENT_TRANSLATION;
		if (ste.tt != TABLE_TYPE_SEGMENT)
			return PGM_TRANSLATION_SPEC;
		if (ste.cs && asce.p)
			return PGM_TRANSLATION_SPEC;
		*dat_protection |= ste.fc0.p;
		if (ste.fc && sg->edat_level >= 1) {
			*fake = 1;
			ptr = ste.fc1.sfaa * _SEGMENT_SIZE;
			ste.val = ptr;
			goto shadow_pgt;
		}
		ptr = ste.fc0.pto * (PAGE_SIZE / 2);
shadow_pgt:
		ste.fc0.p |= *dat_protection;
		rc = gmap_shadow_pgt(sg, saddr, ste.val, *fake);
		if (rc)
			return rc;
	}
	}
	/* Return the parent address of the page table */
	*pgt = ptr;
	return 0;
}

/**
 * kvm_s390_shadow_fault - handle fault on a shadow page table
 * @vcpu: virtual cpu
 * @sg: pointer to the shadow guest address space structure
 * @saddr: faulting address in the shadow gmap
 * @datptr: will contain the address of the faulting DAT table entry, or of
 *	    the valid leaf, plus some flags
 *
 * Returns: - 0 if the shadow fault was successfully resolved
 *	    - > 0 (pgm exception code) on exceptions while faulting
 *	    - -EAGAIN if the caller can retry immediately
 *	    - -EFAULT when accessing invalid guest addresses
 *	    - -ENOMEM if out of memory
 */
int kvm_s390_shadow_fault(struct kvm_vcpu *vcpu, struct gmap *sg,
			  unsigned long saddr, unsigned long *datptr)
{
	union vaddress vaddr;
	union page_table_entry pte;
	unsigned long pgt = 0;
	int dat_protection, fake;
	int rc;

	mmap_read_lock(sg->mm);
	/*
	 * We don't want any guest-2 tables to change - so the parent
	 * tables/pointers we read stay valid - unshadowing is however
	 * always possible - only guest_table_lock protects us.
	 */
	ipte_lock(vcpu->kvm);

	rc = gmap_shadow_pgt_lookup(sg, saddr, &pgt, &dat_protection, &fake);
	if (rc)
		rc = kvm_s390_shadow_tables(sg, saddr, &pgt, &dat_protection,
					    &fake);

	vaddr.addr = saddr;
	if (fake) {
		pte.val = pgt + vaddr.px * PAGE_SIZE;
		goto shadow_page;
	}

	switch (rc) {
	case PGM_SEGMENT_TRANSLATION:
	case PGM_REGION_THIRD_TRANS:
	case PGM_REGION_SECOND_TRANS:
	case PGM_REGION_FIRST_TRANS:
		pgt |= PEI_NOT_PTE;
		break;
	case 0:
		pgt += vaddr.px * 8;
		rc = gmap_read_table(sg->parent, pgt, &pte.val);
	}
	if (datptr)
		*datptr = pgt | dat_protection * PEI_DAT_PROT;
	if (!rc && pte.i)
		rc = PGM_PAGE_TRANSLATION;
	if (!rc && pte.z)
		rc = PGM_TRANSLATION_SPEC;
shadow_page:
	pte.p |= dat_protection;
	if (!rc)
		rc = gmap_shadow_page(sg, saddr, __pte(pte.val));
	ipte_unlock(vcpu->kvm);
	mmap_read_unlock(sg->mm);
	return rc;
}

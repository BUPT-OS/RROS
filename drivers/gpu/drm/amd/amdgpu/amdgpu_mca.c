/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include "amdgpu_ras.h"
#include "amdgpu.h"
#include "amdgpu_mca.h"

#include "umc/umc_6_7_0_offset.h"
#include "umc/umc_6_7_0_sh_mask.h"

void amdgpu_mca_query_correctable_error_count(struct amdgpu_device *adev,
					      uint64_t mc_status_addr,
					      unsigned long *error_count)
{
	uint64_t mc_status = RREG64_PCIE(mc_status_addr);

	if (REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, Val) == 1 &&
	    REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, CECC) == 1)
		*error_count += 1;
}

void amdgpu_mca_query_uncorrectable_error_count(struct amdgpu_device *adev,
						uint64_t mc_status_addr,
						unsigned long *error_count)
{
	uint64_t mc_status = RREG64_PCIE(mc_status_addr);

	if ((REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, Val) == 1) &&
	    (REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, Deferred) == 1 ||
	    REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, UECC) == 1 ||
	    REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, PCC) == 1 ||
	    REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, UC) == 1 ||
	    REG_GET_FIELD(mc_status, MCA_UMC_UMC0_MCUMC_STATUST0, TCC) == 1))
		*error_count += 1;
}

void amdgpu_mca_reset_error_count(struct amdgpu_device *adev,
				  uint64_t mc_status_addr)
{
	WREG64_PCIE(mc_status_addr, 0x0ULL);
}

void amdgpu_mca_query_ras_error_count(struct amdgpu_device *adev,
				      uint64_t mc_status_addr,
				      void *ras_error_status)
{
	struct ras_err_data *err_data = (struct ras_err_data *)ras_error_status;

	amdgpu_mca_query_correctable_error_count(adev, mc_status_addr, &(err_data->ce_count));
	amdgpu_mca_query_uncorrectable_error_count(adev, mc_status_addr, &(err_data->ue_count));

	amdgpu_mca_reset_error_count(adev, mc_status_addr);
}

int amdgpu_mca_mp0_ras_sw_init(struct amdgpu_device *adev)
{
	int err;
	struct amdgpu_mca_ras_block *ras;

	if (!adev->mca.mp0.ras)
		return 0;

	ras = adev->mca.mp0.ras;

	err = amdgpu_ras_register_ras_block(adev, &ras->ras_block);
	if (err) {
		dev_err(adev->dev, "Failed to register mca.mp0 ras block!\n");
		return err;
	}

	strcpy(ras->ras_block.ras_comm.name, "mca.mp0");
	ras->ras_block.ras_comm.block = AMDGPU_RAS_BLOCK__MCA;
	ras->ras_block.ras_comm.type = AMDGPU_RAS_ERROR__MULTI_UNCORRECTABLE;
	adev->mca.mp0.ras_if = &ras->ras_block.ras_comm;

	return 0;
}

int amdgpu_mca_mp1_ras_sw_init(struct amdgpu_device *adev)
{
	int err;
	struct amdgpu_mca_ras_block *ras;

	if (!adev->mca.mp1.ras)
		return 0;

	ras = adev->mca.mp1.ras;

	err = amdgpu_ras_register_ras_block(adev, &ras->ras_block);
	if (err) {
		dev_err(adev->dev, "Failed to register mca.mp1 ras block!\n");
		return err;
	}

	strcpy(ras->ras_block.ras_comm.name, "mca.mp1");
	ras->ras_block.ras_comm.block = AMDGPU_RAS_BLOCK__MCA;
	ras->ras_block.ras_comm.type = AMDGPU_RAS_ERROR__MULTI_UNCORRECTABLE;
	adev->mca.mp1.ras_if = &ras->ras_block.ras_comm;

	return 0;
}

int amdgpu_mca_mpio_ras_sw_init(struct amdgpu_device *adev)
{
	int err;
	struct amdgpu_mca_ras_block *ras;

	if (!adev->mca.mpio.ras)
		return 0;

	ras = adev->mca.mpio.ras;

	err = amdgpu_ras_register_ras_block(adev, &ras->ras_block);
	if (err) {
		dev_err(adev->dev, "Failed to register mca.mpio ras block!\n");
		return err;
	}

	strcpy(ras->ras_block.ras_comm.name, "mca.mpio");
	ras->ras_block.ras_comm.block = AMDGPU_RAS_BLOCK__MCA;
	ras->ras_block.ras_comm.type = AMDGPU_RAS_ERROR__MULTI_UNCORRECTABLE;
	adev->mca.mpio.ras_if = &ras->ras_block.ras_comm;

	return 0;
}

/*
 * Copyright 2016 Red Hat Inc.
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
 * Authors: Ben Skeggs
 */
#include "priv.h"

const struct nvkm_mc_map
gk104_mc_reset[] = {
	{ 0x00000100, NVKM_ENGINE_FIFO },
	{ 0x00002000, NVKM_SUBDEV_PMU, 0, true },
	{}
};

const struct nvkm_intr_data
gk104_mc_intrs[] = {
	{ NVKM_ENGINE_DISP    , 0, 0, 0x04000000, true },
	{ NVKM_ENGINE_FIFO    , 0, 0, 0x00000100 },
	{ NVKM_SUBDEV_PRIVRING, 0, 0, 0x40000000, true },
	{ NVKM_SUBDEV_BUS     , 0, 0, 0x10000000, true },
	{ NVKM_SUBDEV_FB      , 0, 0, 0x08002000, true },
	{ NVKM_SUBDEV_LTC     , 0, 0, 0x02000000, true },
	{ NVKM_SUBDEV_PMU     , 0, 0, 0x01000000, true },
	{ NVKM_SUBDEV_GPIO    , 0, 0, 0x00200000, true },
	{ NVKM_SUBDEV_I2C     , 0, 0, 0x00200000, true },
	{ NVKM_SUBDEV_TIMER   , 0, 0, 0x00100000, true },
	{ NVKM_SUBDEV_THERM   , 0, 0, 0x00040000, true },
	{ NVKM_SUBDEV_TOP     , 0, 0, 0x00001000 },
	{ NVKM_SUBDEV_TOP     , 0, 0, 0xffffefff, true },
	{},
};

static const struct nvkm_mc_func
gk104_mc = {
	.init = nv50_mc_init,
	.intr = &gt215_mc_intr,
	.intrs = gk104_mc_intrs,
	.intr_nonstall = true,
	.reset = gk104_mc_reset,
	.device = &nv04_mc_device,
	.unk260 = gf100_mc_unk260,
};

int
gk104_mc_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst, struct nvkm_mc **pmc)
{
	return nvkm_mc_new_(&gk104_mc, device, type, inst, pmc);
}

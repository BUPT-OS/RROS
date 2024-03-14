/*
 * Copyright 2012 Red Hat Inc.
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
#include <core/engine.h>
#include <core/device.h>
#include <core/option.h>

#include <subdev/fb.h>

bool
nvkm_engine_chsw_load(struct nvkm_engine *engine)
{
	if (engine->func->chsw_load)
		return engine->func->chsw_load(engine);
	return false;
}

int
nvkm_engine_reset(struct nvkm_engine *engine)
{
	if (engine->func->reset)
		return engine->func->reset(engine);

	nvkm_subdev_fini(&engine->subdev, false);
	return nvkm_subdev_init(&engine->subdev);
}

void
nvkm_engine_unref(struct nvkm_engine **pengine)
{
	struct nvkm_engine *engine = *pengine;

	if (engine) {
		nvkm_subdev_unref(&engine->subdev);
		*pengine = NULL;
	}
}

struct nvkm_engine *
nvkm_engine_ref(struct nvkm_engine *engine)
{
	int ret;

	if (engine) {
		ret = nvkm_subdev_ref(&engine->subdev);
		if (ret)
			return ERR_PTR(ret);
	}

	return engine;
}

void
nvkm_engine_tile(struct nvkm_engine *engine, int region)
{
	struct nvkm_fb *fb = engine->subdev.device->fb;
	if (engine->func->tile)
		engine->func->tile(engine, region, &fb->tile.region[region]);
}

static void
nvkm_engine_intr(struct nvkm_subdev *subdev)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);
	if (engine->func->intr)
		engine->func->intr(engine);
}

static int
nvkm_engine_info(struct nvkm_subdev *subdev, u64 mthd, u64 *data)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);

	if (engine->func->info)
		return engine->func->info(engine, mthd, data);

	return -ENOSYS;
}

static int
nvkm_engine_fini(struct nvkm_subdev *subdev, bool suspend)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);
	if (engine->func->fini)
		return engine->func->fini(engine, suspend);
	return 0;
}

static int
nvkm_engine_init(struct nvkm_subdev *subdev)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);
	struct nvkm_fb *fb = subdev->device->fb;
	int ret = 0, i;

	if (engine->func->init)
		ret = engine->func->init(engine);

	for (i = 0; fb && i < fb->tile.regions; i++)
		nvkm_engine_tile(engine, i);
	return ret;
}

static int
nvkm_engine_oneinit(struct nvkm_subdev *subdev)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);

	if (engine->func->oneinit)
		return engine->func->oneinit(engine);

	return 0;
}

static int
nvkm_engine_preinit(struct nvkm_subdev *subdev)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);
	if (engine->func->preinit)
		engine->func->preinit(engine);
	return 0;
}

static void *
nvkm_engine_dtor(struct nvkm_subdev *subdev)
{
	struct nvkm_engine *engine = nvkm_engine(subdev);
	if (engine->func->dtor)
		return engine->func->dtor(engine);
	return engine;
}

const struct nvkm_subdev_func
nvkm_engine = {
	.dtor = nvkm_engine_dtor,
	.preinit = nvkm_engine_preinit,
	.oneinit = nvkm_engine_oneinit,
	.init = nvkm_engine_init,
	.fini = nvkm_engine_fini,
	.info = nvkm_engine_info,
	.intr = nvkm_engine_intr,
};

int
nvkm_engine_ctor(const struct nvkm_engine_func *func, struct nvkm_device *device,
		 enum nvkm_subdev_type type, int inst, bool enable, struct nvkm_engine *engine)
{
	engine->func = func;
	nvkm_subdev_ctor(&nvkm_engine, device, type, inst, &engine->subdev);
	refcount_set(&engine->subdev.use.refcount, 0);

	if (!nvkm_boolopt(device->cfgopt, engine->subdev.name, enable)) {
		nvkm_debug(&engine->subdev, "disabled\n");
		return -ENODEV;
	}

	spin_lock_init(&engine->lock);
	return 0;
}

int
nvkm_engine_new_(const struct nvkm_engine_func *func, struct nvkm_device *device,
		 enum nvkm_subdev_type type, int inst, bool enable,
		 struct nvkm_engine **pengine)
{
	if (!(*pengine = kzalloc(sizeof(**pengine), GFP_KERNEL)))
		return -ENOMEM;
	return nvkm_engine_ctor(func, device, type, inst, enable, *pengine);
}

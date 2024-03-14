/* SPDX-License-Identifier: MIT */

#ifndef __NOUVEAU_EXEC_H__
#define __NOUVEAU_EXEC_H__

#include <drm/drm_exec.h>

#include "nouveau_drv.h"
#include "nouveau_sched.h"

struct nouveau_exec_job_args {
	struct drm_file *file_priv;
	struct nouveau_sched_entity *sched_entity;

	struct drm_exec exec;
	struct nouveau_channel *chan;

	struct {
		struct drm_nouveau_sync *s;
		u32 count;
	} in_sync;

	struct {
		struct drm_nouveau_sync *s;
		u32 count;
	} out_sync;

	struct {
		struct drm_nouveau_exec_push *s;
		u32 count;
	} push;
};

struct nouveau_exec_job {
	struct nouveau_job base;
	struct nouveau_fence *fence;
	struct nouveau_channel *chan;

	struct {
		struct drm_nouveau_exec_push *s;
		u32 count;
	} push;
};

#define to_nouveau_exec_job(job)		\
		container_of((job), struct nouveau_exec_job, base)

int nouveau_exec_job_init(struct nouveau_exec_job **job,
			  struct nouveau_exec_job_args *args);

int nouveau_exec_ioctl_exec(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);

#endif

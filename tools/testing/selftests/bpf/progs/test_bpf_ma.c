// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2023. Huawei Technologies Co., Ltd */
#include <vmlinux.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_helpers.h>

#include "bpf_experimental.h"
#include "bpf_misc.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

struct generic_map_value {
	void *data;
};

char _license[] SEC("license") = "GPL";

const unsigned int data_sizes[] = {8, 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048, 4096};
const volatile unsigned int data_btf_ids[ARRAY_SIZE(data_sizes)] = {};

int err = 0;
int pid = 0;

#define DEFINE_ARRAY_WITH_KPTR(_size) \
	struct bin_data_##_size { \
		char data[_size - sizeof(void *)]; \
	}; \
	struct map_value_##_size { \
		struct bin_data_##_size __kptr * data; \
		/* To emit BTF info for bin_data_xx */ \
		struct bin_data_##_size not_used; \
	}; \
	struct { \
		__uint(type, BPF_MAP_TYPE_ARRAY); \
		__type(key, int); \
		__type(value, struct map_value_##_size); \
		__uint(max_entries, 128); \
	} array_##_size SEC(".maps");

static __always_inline void batch_alloc_free(struct bpf_map *map, unsigned int batch,
					     unsigned int idx)
{
	struct generic_map_value *value;
	unsigned int i, key;
	void *old, *new;

	for (i = 0; i < batch; i++) {
		key = i;
		value = bpf_map_lookup_elem(map, &key);
		if (!value) {
			err = 1;
			return;
		}
		new = bpf_obj_new_impl(data_btf_ids[idx], NULL);
		if (!new) {
			err = 2;
			return;
		}
		old = bpf_kptr_xchg(&value->data, new);
		if (old) {
			bpf_obj_drop(old);
			err = 3;
			return;
		}
	}
	for (i = 0; i < batch; i++) {
		key = i;
		value = bpf_map_lookup_elem(map, &key);
		if (!value) {
			err = 4;
			return;
		}
		old = bpf_kptr_xchg(&value->data, NULL);
		if (!old) {
			err = 5;
			return;
		}
		bpf_obj_drop(old);
	}
}

#define CALL_BATCH_ALLOC_FREE(size, batch, idx) \
	batch_alloc_free((struct bpf_map *)(&array_##size), batch, idx)

DEFINE_ARRAY_WITH_KPTR(8);
DEFINE_ARRAY_WITH_KPTR(16);
DEFINE_ARRAY_WITH_KPTR(32);
DEFINE_ARRAY_WITH_KPTR(64);
DEFINE_ARRAY_WITH_KPTR(96);
DEFINE_ARRAY_WITH_KPTR(128);
DEFINE_ARRAY_WITH_KPTR(192);
DEFINE_ARRAY_WITH_KPTR(256);
DEFINE_ARRAY_WITH_KPTR(512);
DEFINE_ARRAY_WITH_KPTR(1024);
DEFINE_ARRAY_WITH_KPTR(2048);
DEFINE_ARRAY_WITH_KPTR(4096);

SEC("fentry/" SYS_PREFIX "sys_nanosleep")
int test_bpf_mem_alloc_free(void *ctx)
{
	if ((u32)bpf_get_current_pid_tgid() != pid)
		return 0;

	/* Alloc 128 8-bytes objects in batch to trigger refilling,
	 * then free 128 8-bytes objects in batch to trigger freeing.
	 */
	CALL_BATCH_ALLOC_FREE(8, 128, 0);
	CALL_BATCH_ALLOC_FREE(16, 128, 1);
	CALL_BATCH_ALLOC_FREE(32, 128, 2);
	CALL_BATCH_ALLOC_FREE(64, 128, 3);
	CALL_BATCH_ALLOC_FREE(96, 128, 4);
	CALL_BATCH_ALLOC_FREE(128, 128, 5);
	CALL_BATCH_ALLOC_FREE(192, 128, 6);
	CALL_BATCH_ALLOC_FREE(256, 128, 7);
	CALL_BATCH_ALLOC_FREE(512, 64, 8);
	CALL_BATCH_ALLOC_FREE(1024, 32, 9);
	CALL_BATCH_ALLOC_FREE(2048, 16, 10);
	CALL_BATCH_ALLOC_FREE(4096, 8, 11);

	return 0;
}

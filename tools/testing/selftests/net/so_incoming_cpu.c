// SPDX-License-Identifier: GPL-2.0
/* Copyright Amazon.com Inc. or its affiliates. */
#define _GNU_SOURCE
#include <sched.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>

#include "../kselftest_harness.h"

#define CLIENT_PER_SERVER	32 /* More sockets, more reliable */
#define NR_SERVER		self->nproc
#define NR_CLIENT		(CLIENT_PER_SERVER * NR_SERVER)

FIXTURE(so_incoming_cpu)
{
	int nproc;
	int *servers;
	union {
		struct sockaddr addr;
		struct sockaddr_in in_addr;
	};
	socklen_t addrlen;
};

enum when_to_set {
	BEFORE_REUSEPORT,
	BEFORE_LISTEN,
	AFTER_LISTEN,
	AFTER_ALL_LISTEN,
};

FIXTURE_VARIANT(so_incoming_cpu)
{
	int when_to_set;
};

FIXTURE_VARIANT_ADD(so_incoming_cpu, before_reuseport)
{
	.when_to_set = BEFORE_REUSEPORT,
};

FIXTURE_VARIANT_ADD(so_incoming_cpu, before_listen)
{
	.when_to_set = BEFORE_LISTEN,
};

FIXTURE_VARIANT_ADD(so_incoming_cpu, after_listen)
{
	.when_to_set = AFTER_LISTEN,
};

FIXTURE_VARIANT_ADD(so_incoming_cpu, after_all_listen)
{
	.when_to_set = AFTER_ALL_LISTEN,
};

FIXTURE_SETUP(so_incoming_cpu)
{
	self->nproc = get_nprocs();
	ASSERT_LE(2, self->nproc);

	self->servers = malloc(sizeof(int) * NR_SERVER);
	ASSERT_NE(self->servers, NULL);

	self->in_addr.sin_family = AF_INET;
	self->in_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	self->in_addr.sin_port = htons(0);
	self->addrlen = sizeof(struct sockaddr_in);
}

FIXTURE_TEARDOWN(so_incoming_cpu)
{
	int i;

	for (i = 0; i < NR_SERVER; i++)
		close(self->servers[i]);

	free(self->servers);
}

void set_so_incoming_cpu(struct __test_metadata *_metadata, int fd, int cpu)
{
	int ret;

	ret = setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(int));
	ASSERT_EQ(ret, 0);
}

int create_server(struct __test_metadata *_metadata,
		  FIXTURE_DATA(so_incoming_cpu) *self,
		  const FIXTURE_VARIANT(so_incoming_cpu) *variant,
		  int cpu)
{
	int fd, ret;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	ASSERT_NE(fd, -1);

	if (variant->when_to_set == BEFORE_REUSEPORT)
		set_so_incoming_cpu(_metadata, fd, cpu);

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));
	ASSERT_EQ(ret, 0);

	ret = bind(fd, &self->addr, self->addrlen);
	ASSERT_EQ(ret, 0);

	if (variant->when_to_set == BEFORE_LISTEN)
		set_so_incoming_cpu(_metadata, fd, cpu);

	/* We don't use CLIENT_PER_SERVER here not to block
	 * this test at connect() if SO_INCOMING_CPU is broken.
	 */
	ret = listen(fd, NR_CLIENT);
	ASSERT_EQ(ret, 0);

	if (variant->when_to_set == AFTER_LISTEN)
		set_so_incoming_cpu(_metadata, fd, cpu);

	return fd;
}

void create_servers(struct __test_metadata *_metadata,
		    FIXTURE_DATA(so_incoming_cpu) *self,
		    const FIXTURE_VARIANT(so_incoming_cpu) *variant)
{
	int i, ret;

	for (i = 0; i < NR_SERVER; i++) {
		self->servers[i] = create_server(_metadata, self, variant, i);

		if (i == 0) {
			ret = getsockname(self->servers[i], &self->addr, &self->addrlen);
			ASSERT_EQ(ret, 0);
		}
	}

	if (variant->when_to_set == AFTER_ALL_LISTEN) {
		for (i = 0; i < NR_SERVER; i++)
			set_so_incoming_cpu(_metadata, self->servers[i], i);
	}
}

void create_clients(struct __test_metadata *_metadata,
		    FIXTURE_DATA(so_incoming_cpu) *self)
{
	cpu_set_t cpu_set;
	int i, j, fd, ret;

	for (i = 0; i < NR_SERVER; i++) {
		CPU_ZERO(&cpu_set);

		CPU_SET(i, &cpu_set);
		ASSERT_EQ(CPU_COUNT(&cpu_set), 1);
		ASSERT_NE(CPU_ISSET(i, &cpu_set), 0);

		/* Make sure SYN will be processed on the i-th CPU
		 * and finally distributed to the i-th listener.
		 */
		ret = sched_setaffinity(0, sizeof(cpu_set), &cpu_set);
		ASSERT_EQ(ret, 0);

		for (j = 0; j < CLIENT_PER_SERVER; j++) {
			fd  = socket(AF_INET, SOCK_STREAM, 0);
			ASSERT_NE(fd, -1);

			ret = connect(fd, &self->addr, self->addrlen);
			ASSERT_EQ(ret, 0);

			close(fd);
		}
	}
}

void verify_incoming_cpu(struct __test_metadata *_metadata,
			 FIXTURE_DATA(so_incoming_cpu) *self)
{
	int i, j, fd, cpu, ret, total = 0;
	socklen_t len = sizeof(int);

	for (i = 0; i < NR_SERVER; i++) {
		for (j = 0; j < CLIENT_PER_SERVER; j++) {
			/* If we see -EAGAIN here, SO_INCOMING_CPU is broken */
			fd = accept(self->servers[i], &self->addr, &self->addrlen);
			ASSERT_NE(fd, -1);

			ret = getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, &len);
			ASSERT_EQ(ret, 0);
			ASSERT_EQ(cpu, i);

			close(fd);
			total++;
		}
	}

	ASSERT_EQ(total, NR_CLIENT);
	TH_LOG("SO_INCOMING_CPU is very likely to be "
	       "working correctly with %d sockets.", total);
}

TEST_F(so_incoming_cpu, test1)
{
	create_servers(_metadata, self, variant);
	create_clients(_metadata, self);
	verify_incoming_cpu(_metadata, self);
}

TEST_F(so_incoming_cpu, test2)
{
	int server;

	create_servers(_metadata, self, variant);

	/* No CPU specified */
	server = create_server(_metadata, self, variant, -1);
	close(server);

	create_clients(_metadata, self);
	verify_incoming_cpu(_metadata, self);
}

TEST_F(so_incoming_cpu, test3)
{
	int server, client;

	create_servers(_metadata, self, variant);

	/* No CPU specified */
	server = create_server(_metadata, self, variant, -1);

	create_clients(_metadata, self);

	/* Never receive any requests */
	client = accept(server, &self->addr, &self->addrlen);
	ASSERT_EQ(client, -1);

	verify_incoming_cpu(_metadata, self);
}

TEST_HARNESS_MAIN

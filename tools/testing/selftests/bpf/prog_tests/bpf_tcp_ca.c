// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 Facebook */

#include <linux/err.h>
#include <netinet/tcp.h>
#include <test_progs.h>
#include "network_helpers.h"
#include "bpf_dctcp.skel.h"
#include "bpf_cubic.skel.h"
#include "bpf_tcp_nogpl.skel.h"
#include "tcp_ca_update.skel.h"
#include "bpf_dctcp_release.skel.h"
#include "tcp_ca_write_sk_pacing.skel.h"
#include "tcp_ca_incompl_cong_ops.skel.h"
#include "tcp_ca_unsupp_cong_op.skel.h"

#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif

static const unsigned int total_bytes = 10 * 1024 * 1024;
static int expected_stg = 0xeB9F;
static int stop, duration;

static int settcpca(int fd, const char *tcp_ca)
{
	int err;

	err = setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, tcp_ca, strlen(tcp_ca));
	if (CHECK(err == -1, "setsockopt(fd, TCP_CONGESTION)", "errno:%d\n",
		  errno))
		return -1;

	return 0;
}

static void *server(void *arg)
{
	int lfd = (int)(long)arg, err = 0, fd;
	ssize_t nr_sent = 0, bytes = 0;
	char batch[1500];

	fd = accept(lfd, NULL, NULL);
	while (fd == -1) {
		if (errno == EINTR)
			continue;
		err = -errno;
		goto done;
	}

	if (settimeo(fd, 0)) {
		err = -errno;
		goto done;
	}

	while (bytes < total_bytes && !READ_ONCE(stop)) {
		nr_sent = send(fd, &batch,
			       MIN(total_bytes - bytes, sizeof(batch)), 0);
		if (nr_sent == -1 && errno == EINTR)
			continue;
		if (nr_sent == -1) {
			err = -errno;
			break;
		}
		bytes += nr_sent;
	}

	CHECK(bytes != total_bytes, "send", "%zd != %u nr_sent:%zd errno:%d\n",
	      bytes, total_bytes, nr_sent, errno);

done:
	if (fd >= 0)
		close(fd);
	if (err) {
		WRITE_ONCE(stop, 1);
		return ERR_PTR(err);
	}
	return NULL;
}

static void do_test(const char *tcp_ca, const struct bpf_map *sk_stg_map)
{
	struct sockaddr_in6 sa6 = {};
	ssize_t nr_recv = 0, bytes = 0;
	int lfd = -1, fd = -1;
	pthread_t srv_thread;
	socklen_t addrlen = sizeof(sa6);
	void *thread_ret;
	char batch[1500];
	int err;

	WRITE_ONCE(stop, 0);

	lfd = socket(AF_INET6, SOCK_STREAM, 0);
	if (CHECK(lfd == -1, "socket", "errno:%d\n", errno))
		return;
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (CHECK(fd == -1, "socket", "errno:%d\n", errno)) {
		close(lfd);
		return;
	}

	if (settcpca(lfd, tcp_ca) || settcpca(fd, tcp_ca) ||
	    settimeo(lfd, 0) || settimeo(fd, 0))
		goto done;

	/* bind, listen and start server thread to accept */
	sa6.sin6_family = AF_INET6;
	sa6.sin6_addr = in6addr_loopback;
	err = bind(lfd, (struct sockaddr *)&sa6, addrlen);
	if (CHECK(err == -1, "bind", "errno:%d\n", errno))
		goto done;
	err = getsockname(lfd, (struct sockaddr *)&sa6, &addrlen);
	if (CHECK(err == -1, "getsockname", "errno:%d\n", errno))
		goto done;
	err = listen(lfd, 1);
	if (CHECK(err == -1, "listen", "errno:%d\n", errno))
		goto done;

	if (sk_stg_map) {
		err = bpf_map_update_elem(bpf_map__fd(sk_stg_map), &fd,
					  &expected_stg, BPF_NOEXIST);
		if (CHECK(err, "bpf_map_update_elem(sk_stg_map)",
			  "err:%d errno:%d\n", err, errno))
			goto done;
	}

	/* connect to server */
	err = connect(fd, (struct sockaddr *)&sa6, addrlen);
	if (CHECK(err == -1, "connect", "errno:%d\n", errno))
		goto done;

	if (sk_stg_map) {
		int tmp_stg;

		err = bpf_map_lookup_elem(bpf_map__fd(sk_stg_map), &fd,
					  &tmp_stg);
		if (CHECK(!err || errno != ENOENT,
			  "bpf_map_lookup_elem(sk_stg_map)",
			  "err:%d errno:%d\n", err, errno))
			goto done;
	}

	err = pthread_create(&srv_thread, NULL, server, (void *)(long)lfd);
	if (CHECK(err != 0, "pthread_create", "err:%d errno:%d\n", err, errno))
		goto done;

	/* recv total_bytes */
	while (bytes < total_bytes && !READ_ONCE(stop)) {
		nr_recv = recv(fd, &batch,
			       MIN(total_bytes - bytes, sizeof(batch)), 0);
		if (nr_recv == -1 && errno == EINTR)
			continue;
		if (nr_recv == -1)
			break;
		bytes += nr_recv;
	}

	CHECK(bytes != total_bytes, "recv", "%zd != %u nr_recv:%zd errno:%d\n",
	      bytes, total_bytes, nr_recv, errno);

	WRITE_ONCE(stop, 1);
	pthread_join(srv_thread, &thread_ret);
	CHECK(IS_ERR(thread_ret), "pthread_join", "thread_ret:%ld",
	      PTR_ERR(thread_ret));
done:
	close(lfd);
	close(fd);
}

static void test_cubic(void)
{
	struct bpf_cubic *cubic_skel;
	struct bpf_link *link;

	cubic_skel = bpf_cubic__open_and_load();
	if (CHECK(!cubic_skel, "bpf_cubic__open_and_load", "failed\n"))
		return;

	link = bpf_map__attach_struct_ops(cubic_skel->maps.cubic);
	if (!ASSERT_OK_PTR(link, "bpf_map__attach_struct_ops")) {
		bpf_cubic__destroy(cubic_skel);
		return;
	}

	do_test("bpf_cubic", NULL);

	bpf_link__destroy(link);
	bpf_cubic__destroy(cubic_skel);
}

static void test_dctcp(void)
{
	struct bpf_dctcp *dctcp_skel;
	struct bpf_link *link;

	dctcp_skel = bpf_dctcp__open_and_load();
	if (CHECK(!dctcp_skel, "bpf_dctcp__open_and_load", "failed\n"))
		return;

	link = bpf_map__attach_struct_ops(dctcp_skel->maps.dctcp);
	if (!ASSERT_OK_PTR(link, "bpf_map__attach_struct_ops")) {
		bpf_dctcp__destroy(dctcp_skel);
		return;
	}

	do_test("bpf_dctcp", dctcp_skel->maps.sk_stg_map);
	CHECK(dctcp_skel->bss->stg_result != expected_stg,
	      "Unexpected stg_result", "stg_result (%x) != expected_stg (%x)\n",
	      dctcp_skel->bss->stg_result, expected_stg);

	bpf_link__destroy(link);
	bpf_dctcp__destroy(dctcp_skel);
}

static char *err_str;
static bool found;

static int libbpf_debug_print(enum libbpf_print_level level,
			      const char *format, va_list args)
{
	const char *prog_name, *log_buf;

	if (level != LIBBPF_WARN ||
	    !strstr(format, "-- BEGIN PROG LOAD LOG --")) {
		vprintf(format, args);
		return 0;
	}

	prog_name = va_arg(args, char *);
	log_buf = va_arg(args, char *);
	if (!log_buf)
		goto out;
	if (err_str && strstr(log_buf, err_str) != NULL)
		found = true;
out:
	printf(format, prog_name, log_buf);
	return 0;
}

static void test_invalid_license(void)
{
	libbpf_print_fn_t old_print_fn;
	struct bpf_tcp_nogpl *skel;

	err_str = "struct ops programs must have a GPL compatible license";
	found = false;
	old_print_fn = libbpf_set_print(libbpf_debug_print);

	skel = bpf_tcp_nogpl__open_and_load();
	ASSERT_NULL(skel, "bpf_tcp_nogpl");
	ASSERT_EQ(found, true, "expected_err_msg");

	bpf_tcp_nogpl__destroy(skel);
	libbpf_set_print(old_print_fn);
}

static void test_dctcp_fallback(void)
{
	int err, lfd = -1, cli_fd = -1, srv_fd = -1;
	struct network_helper_opts opts = {
		.cc = "cubic",
	};
	struct bpf_dctcp *dctcp_skel;
	struct bpf_link *link = NULL;
	char srv_cc[16];
	socklen_t cc_len = sizeof(srv_cc);

	dctcp_skel = bpf_dctcp__open();
	if (!ASSERT_OK_PTR(dctcp_skel, "dctcp_skel"))
		return;
	strcpy(dctcp_skel->rodata->fallback, "cubic");
	if (!ASSERT_OK(bpf_dctcp__load(dctcp_skel), "bpf_dctcp__load"))
		goto done;

	link = bpf_map__attach_struct_ops(dctcp_skel->maps.dctcp);
	if (!ASSERT_OK_PTR(link, "dctcp link"))
		goto done;

	lfd = start_server(AF_INET6, SOCK_STREAM, "::1", 0, 0);
	if (!ASSERT_GE(lfd, 0, "lfd") ||
	    !ASSERT_OK(settcpca(lfd, "bpf_dctcp"), "lfd=>bpf_dctcp"))
		goto done;

	cli_fd = connect_to_fd_opts(lfd, &opts);
	if (!ASSERT_GE(cli_fd, 0, "cli_fd"))
		goto done;

	srv_fd = accept(lfd, NULL, 0);
	if (!ASSERT_GE(srv_fd, 0, "srv_fd"))
		goto done;
	ASSERT_STREQ(dctcp_skel->bss->cc_res, "cubic", "cc_res");
	ASSERT_EQ(dctcp_skel->bss->tcp_cdg_res, -ENOTSUPP, "tcp_cdg_res");
	/* All setsockopt(TCP_CONGESTION) in the recurred
	 * bpf_dctcp->init() should fail with -EBUSY.
	 */
	ASSERT_EQ(dctcp_skel->bss->ebusy_cnt, 3, "ebusy_cnt");

	err = getsockopt(srv_fd, SOL_TCP, TCP_CONGESTION, srv_cc, &cc_len);
	if (!ASSERT_OK(err, "getsockopt(srv_fd, TCP_CONGESTION)"))
		goto done;
	ASSERT_STREQ(srv_cc, "cubic", "srv_fd cc");

done:
	bpf_link__destroy(link);
	bpf_dctcp__destroy(dctcp_skel);
	if (lfd != -1)
		close(lfd);
	if (srv_fd != -1)
		close(srv_fd);
	if (cli_fd != -1)
		close(cli_fd);
}

static void test_rel_setsockopt(void)
{
	struct bpf_dctcp_release *rel_skel;
	libbpf_print_fn_t old_print_fn;

	err_str = "unknown func bpf_setsockopt";
	found = false;

	old_print_fn = libbpf_set_print(libbpf_debug_print);
	rel_skel = bpf_dctcp_release__open_and_load();
	libbpf_set_print(old_print_fn);

	ASSERT_ERR_PTR(rel_skel, "rel_skel");
	ASSERT_TRUE(found, "expected_err_msg");

	bpf_dctcp_release__destroy(rel_skel);
}

static void test_write_sk_pacing(void)
{
	struct tcp_ca_write_sk_pacing *skel;
	struct bpf_link *link;

	skel = tcp_ca_write_sk_pacing__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	link = bpf_map__attach_struct_ops(skel->maps.write_sk_pacing);
	ASSERT_OK_PTR(link, "attach_struct_ops");

	bpf_link__destroy(link);
	tcp_ca_write_sk_pacing__destroy(skel);
}

static void test_incompl_cong_ops(void)
{
	struct tcp_ca_incompl_cong_ops *skel;
	struct bpf_link *link;

	skel = tcp_ca_incompl_cong_ops__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	/* That cong_avoid() and cong_control() are missing is only reported at
	 * this point:
	 */
	link = bpf_map__attach_struct_ops(skel->maps.incompl_cong_ops);
	ASSERT_ERR_PTR(link, "attach_struct_ops");

	bpf_link__destroy(link);
	tcp_ca_incompl_cong_ops__destroy(skel);
}

static void test_unsupp_cong_op(void)
{
	libbpf_print_fn_t old_print_fn;
	struct tcp_ca_unsupp_cong_op *skel;

	err_str = "attach to unsupported member get_info";
	found = false;
	old_print_fn = libbpf_set_print(libbpf_debug_print);

	skel = tcp_ca_unsupp_cong_op__open_and_load();
	ASSERT_NULL(skel, "open_and_load");
	ASSERT_EQ(found, true, "expected_err_msg");

	tcp_ca_unsupp_cong_op__destroy(skel);
	libbpf_set_print(old_print_fn);
}

static void test_update_ca(void)
{
	struct tcp_ca_update *skel;
	struct bpf_link *link;
	int saved_ca1_cnt;
	int err;

	skel = tcp_ca_update__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open"))
		return;

	link = bpf_map__attach_struct_ops(skel->maps.ca_update_1);
	ASSERT_OK_PTR(link, "attach_struct_ops");

	do_test("tcp_ca_update", NULL);
	saved_ca1_cnt = skel->bss->ca1_cnt;
	ASSERT_GT(saved_ca1_cnt, 0, "ca1_ca1_cnt");

	err = bpf_link__update_map(link, skel->maps.ca_update_2);
	ASSERT_OK(err, "update_map");

	do_test("tcp_ca_update", NULL);
	ASSERT_EQ(skel->bss->ca1_cnt, saved_ca1_cnt, "ca2_ca1_cnt");
	ASSERT_GT(skel->bss->ca2_cnt, 0, "ca2_ca2_cnt");

	bpf_link__destroy(link);
	tcp_ca_update__destroy(skel);
}

static void test_update_wrong(void)
{
	struct tcp_ca_update *skel;
	struct bpf_link *link;
	int saved_ca1_cnt;
	int err;

	skel = tcp_ca_update__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open"))
		return;

	link = bpf_map__attach_struct_ops(skel->maps.ca_update_1);
	ASSERT_OK_PTR(link, "attach_struct_ops");

	do_test("tcp_ca_update", NULL);
	saved_ca1_cnt = skel->bss->ca1_cnt;
	ASSERT_GT(saved_ca1_cnt, 0, "ca1_ca1_cnt");

	err = bpf_link__update_map(link, skel->maps.ca_wrong);
	ASSERT_ERR(err, "update_map");

	do_test("tcp_ca_update", NULL);
	ASSERT_GT(skel->bss->ca1_cnt, saved_ca1_cnt, "ca2_ca1_cnt");

	bpf_link__destroy(link);
	tcp_ca_update__destroy(skel);
}

static void test_mixed_links(void)
{
	struct tcp_ca_update *skel;
	struct bpf_link *link, *link_nl;
	int err;

	skel = tcp_ca_update__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open"))
		return;

	link_nl = bpf_map__attach_struct_ops(skel->maps.ca_no_link);
	ASSERT_OK_PTR(link_nl, "attach_struct_ops_nl");

	link = bpf_map__attach_struct_ops(skel->maps.ca_update_1);
	ASSERT_OK_PTR(link, "attach_struct_ops");

	do_test("tcp_ca_update", NULL);
	ASSERT_GT(skel->bss->ca1_cnt, 0, "ca1_ca1_cnt");

	err = bpf_link__update_map(link, skel->maps.ca_no_link);
	ASSERT_ERR(err, "update_map");

	bpf_link__destroy(link);
	bpf_link__destroy(link_nl);
	tcp_ca_update__destroy(skel);
}

static void test_multi_links(void)
{
	struct tcp_ca_update *skel;
	struct bpf_link *link;

	skel = tcp_ca_update__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open"))
		return;

	link = bpf_map__attach_struct_ops(skel->maps.ca_update_1);
	ASSERT_OK_PTR(link, "attach_struct_ops_1st");
	bpf_link__destroy(link);

	/* A map should be able to be used to create links multiple
	 * times.
	 */
	link = bpf_map__attach_struct_ops(skel->maps.ca_update_1);
	ASSERT_OK_PTR(link, "attach_struct_ops_2nd");
	bpf_link__destroy(link);

	tcp_ca_update__destroy(skel);
}

static void test_link_replace(void)
{
	DECLARE_LIBBPF_OPTS(bpf_link_update_opts, opts);
	struct tcp_ca_update *skel;
	struct bpf_link *link;
	int err;

	skel = tcp_ca_update__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open"))
		return;

	link = bpf_map__attach_struct_ops(skel->maps.ca_update_1);
	ASSERT_OK_PTR(link, "attach_struct_ops_1st");
	bpf_link__destroy(link);

	link = bpf_map__attach_struct_ops(skel->maps.ca_update_2);
	ASSERT_OK_PTR(link, "attach_struct_ops_2nd");

	/* BPF_F_REPLACE with a wrong old map Fd. It should fail!
	 *
	 * With BPF_F_REPLACE, the link should be updated only if the
	 * old map fd given here matches the map backing the link.
	 */
	opts.old_map_fd = bpf_map__fd(skel->maps.ca_update_1);
	opts.flags = BPF_F_REPLACE;
	err = bpf_link_update(bpf_link__fd(link),
			      bpf_map__fd(skel->maps.ca_update_1),
			      &opts);
	ASSERT_ERR(err, "bpf_link_update_fail");

	/* BPF_F_REPLACE with a correct old map Fd. It should success! */
	opts.old_map_fd = bpf_map__fd(skel->maps.ca_update_2);
	err = bpf_link_update(bpf_link__fd(link),
			      bpf_map__fd(skel->maps.ca_update_1),
			      &opts);
	ASSERT_OK(err, "bpf_link_update_success");

	bpf_link__destroy(link);

	tcp_ca_update__destroy(skel);
}

void test_bpf_tcp_ca(void)
{
	if (test__start_subtest("dctcp"))
		test_dctcp();
	if (test__start_subtest("cubic"))
		test_cubic();
	if (test__start_subtest("invalid_license"))
		test_invalid_license();
	if (test__start_subtest("dctcp_fallback"))
		test_dctcp_fallback();
	if (test__start_subtest("rel_setsockopt"))
		test_rel_setsockopt();
	if (test__start_subtest("write_sk_pacing"))
		test_write_sk_pacing();
	if (test__start_subtest("incompl_cong_ops"))
		test_incompl_cong_ops();
	if (test__start_subtest("unsupp_cong_op"))
		test_unsupp_cong_op();
	if (test__start_subtest("update_ca"))
		test_update_ca();
	if (test__start_subtest("update_wrong"))
		test_update_wrong();
	if (test__start_subtest("mixed_links"))
		test_mixed_links();
	if (test__start_subtest("multi_links"))
		test_multi_links();
	if (test__start_subtest("link_replace"))
		test_link_replace();
}

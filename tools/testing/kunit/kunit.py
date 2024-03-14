#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# A thin wrapper on top of the KUnit Kernel
#
# Copyright (C) 2019, Google LLC.
# Author: Felix Guo <felixguoxiuping@gmail.com>
# Author: Brendan Higgins <brendanhiggins@google.com>

import argparse
import os
import re
import shlex
import sys
import time

assert sys.version_info >= (3, 7), "Python version is too old"

from dataclasses import dataclass
from enum import Enum, auto
from typing import Iterable, List, Optional, Sequence, Tuple

import kunit_json
import kunit_kernel
import kunit_parser
from kunit_printer import stdout

class KunitStatus(Enum):
	SUCCESS = auto()
	CONFIG_FAILURE = auto()
	BUILD_FAILURE = auto()
	TEST_FAILURE = auto()

@dataclass
class KunitResult:
	status: KunitStatus
	elapsed_time: float

@dataclass
class KunitConfigRequest:
	build_dir: str
	make_options: Optional[List[str]]

@dataclass
class KunitBuildRequest(KunitConfigRequest):
	jobs: int

@dataclass
class KunitParseRequest:
	raw_output: Optional[str]
	json: Optional[str]

@dataclass
class KunitExecRequest(KunitParseRequest):
	build_dir: str
	timeout: int
	filter_glob: str
	filter: str
	filter_action: Optional[str]
	kernel_args: Optional[List[str]]
	run_isolated: Optional[str]
	list_tests: bool
	list_tests_attr: bool

@dataclass
class KunitRequest(KunitExecRequest, KunitBuildRequest):
	pass


def get_kernel_root_path() -> str:
	path = sys.argv[0] if not __file__ else __file__
	parts = os.path.realpath(path).split('tools/testing/kunit')
	if len(parts) != 2:
		sys.exit(1)
	return parts[0]

def config_tests(linux: kunit_kernel.LinuxSourceTree,
		 request: KunitConfigRequest) -> KunitResult:
	stdout.print_with_timestamp('Configuring KUnit Kernel ...')

	config_start = time.time()
	success = linux.build_reconfig(request.build_dir, request.make_options)
	config_end = time.time()
	status = KunitStatus.SUCCESS if success else KunitStatus.CONFIG_FAILURE
	return KunitResult(status, config_end - config_start)

def build_tests(linux: kunit_kernel.LinuxSourceTree,
		request: KunitBuildRequest) -> KunitResult:
	stdout.print_with_timestamp('Building KUnit Kernel ...')

	build_start = time.time()
	success = linux.build_kernel(request.jobs,
				     request.build_dir,
				     request.make_options)
	build_end = time.time()
	status = KunitStatus.SUCCESS if success else KunitStatus.BUILD_FAILURE
	return KunitResult(status, build_end - build_start)

def config_and_build_tests(linux: kunit_kernel.LinuxSourceTree,
			   request: KunitBuildRequest) -> KunitResult:
	config_result = config_tests(linux, request)
	if config_result.status != KunitStatus.SUCCESS:
		return config_result

	return build_tests(linux, request)

def _list_tests(linux: kunit_kernel.LinuxSourceTree, request: KunitExecRequest) -> List[str]:
	args = ['kunit.action=list']

	if request.kernel_args:
		args.extend(request.kernel_args)

	output = linux.run_kernel(args=args,
			   timeout=request.timeout,
			   filter_glob=request.filter_glob,
			   filter=request.filter,
			   filter_action=request.filter_action,
			   build_dir=request.build_dir)
	lines = kunit_parser.extract_tap_lines(output)
	# Hack! Drop the dummy TAP version header that the executor prints out.
	lines.pop()

	# Filter out any extraneous non-test output that might have gotten mixed in.
	return [l for l in output if re.match(r'^[^\s.]+\.[^\s.]+$', l)]

def _list_tests_attr(linux: kunit_kernel.LinuxSourceTree, request: KunitExecRequest) -> Iterable[str]:
	args = ['kunit.action=list_attr']

	if request.kernel_args:
		args.extend(request.kernel_args)

	output = linux.run_kernel(args=args,
			   timeout=request.timeout,
			   filter_glob=request.filter_glob,
			   filter=request.filter,
			   filter_action=request.filter_action,
			   build_dir=request.build_dir)
	lines = kunit_parser.extract_tap_lines(output)
	# Hack! Drop the dummy TAP version header that the executor prints out.
	lines.pop()

	# Filter out any extraneous non-test output that might have gotten mixed in.
	return lines

def _suites_from_test_list(tests: List[str]) -> List[str]:
	"""Extracts all the suites from an ordered list of tests."""
	suites = []  # type: List[str]
	for t in tests:
		parts = t.split('.', maxsplit=2)
		if len(parts) != 2:
			raise ValueError(f'internal KUnit error, test name should be of the form "<suite>.<test>", got "{t}"')
		suite, _ = parts
		if not suites or suites[-1] != suite:
			suites.append(suite)
	return suites

def exec_tests(linux: kunit_kernel.LinuxSourceTree, request: KunitExecRequest) -> KunitResult:
	filter_globs = [request.filter_glob]
	if request.list_tests:
		output = _list_tests(linux, request)
		for line in output:
			print(line.rstrip())
		return KunitResult(status=KunitStatus.SUCCESS, elapsed_time=0.0)
	if request.list_tests_attr:
		attr_output = _list_tests_attr(linux, request)
		for line in attr_output:
			print(line.rstrip())
		return KunitResult(status=KunitStatus.SUCCESS, elapsed_time=0.0)
	if request.run_isolated:
		tests = _list_tests(linux, request)
		if request.run_isolated == 'test':
			filter_globs = tests
		elif request.run_isolated == 'suite':
			filter_globs = _suites_from_test_list(tests)
			# Apply the test-part of the user's glob, if present.
			if '.' in request.filter_glob:
				test_glob = request.filter_glob.split('.', maxsplit=2)[1]
				filter_globs = [g + '.'+ test_glob for g in filter_globs]

	metadata = kunit_json.Metadata(arch=linux.arch(), build_dir=request.build_dir, def_config='kunit_defconfig')

	test_counts = kunit_parser.TestCounts()
	exec_time = 0.0
	for i, filter_glob in enumerate(filter_globs):
		stdout.print_with_timestamp('Starting KUnit Kernel ({}/{})...'.format(i+1, len(filter_globs)))

		test_start = time.time()
		run_result = linux.run_kernel(
			args=request.kernel_args,
			timeout=request.timeout,
			filter_glob=filter_glob,
			filter=request.filter,
			filter_action=request.filter_action,
			build_dir=request.build_dir)

		_, test_result = parse_tests(request, metadata, run_result)
		# run_kernel() doesn't block on the kernel exiting.
		# That only happens after we get the last line of output from `run_result`.
		# So exec_time here actually contains parsing + execution time, which is fine.
		test_end = time.time()
		exec_time += test_end - test_start

		test_counts.add_subtest_counts(test_result.counts)

	if len(filter_globs) == 1 and test_counts.crashed > 0:
		bd = request.build_dir
		print('The kernel seems to have crashed; you can decode the stack traces with:')
		print('$ scripts/decode_stacktrace.sh {}/vmlinux {} < {} | tee {}/decoded.log | {} parse'.format(
				bd, bd, kunit_kernel.get_outfile_path(bd), bd, sys.argv[0]))

	kunit_status = _map_to_overall_status(test_counts.get_status())
	return KunitResult(status=kunit_status, elapsed_time=exec_time)

def _map_to_overall_status(test_status: kunit_parser.TestStatus) -> KunitStatus:
	if test_status in (kunit_parser.TestStatus.SUCCESS, kunit_parser.TestStatus.SKIPPED):
		return KunitStatus.SUCCESS
	return KunitStatus.TEST_FAILURE

def parse_tests(request: KunitParseRequest, metadata: kunit_json.Metadata, input_data: Iterable[str]) -> Tuple[KunitResult, kunit_parser.Test]:
	parse_start = time.time()

	if request.raw_output:
		# Treat unparsed results as one passing test.
		fake_test = kunit_parser.Test()
		fake_test.status = kunit_parser.TestStatus.SUCCESS
		fake_test.counts.passed = 1

		output: Iterable[str] = input_data
		if request.raw_output == 'all':
			pass
		elif request.raw_output == 'kunit':
			output = kunit_parser.extract_tap_lines(output)
		for line in output:
			print(line.rstrip())
		parse_time = time.time() - parse_start
		return KunitResult(KunitStatus.SUCCESS, parse_time), fake_test


	# Actually parse the test results.
	test = kunit_parser.parse_run_tests(input_data)
	parse_time = time.time() - parse_start

	if request.json:
		json_str = kunit_json.get_json_result(
					test=test,
					metadata=metadata)
		if request.json == 'stdout':
			print(json_str)
		else:
			with open(request.json, 'w') as f:
				f.write(json_str)
			stdout.print_with_timestamp("Test results stored in %s" %
				os.path.abspath(request.json))

	if test.status != kunit_parser.TestStatus.SUCCESS:
		return KunitResult(KunitStatus.TEST_FAILURE, parse_time), test

	return KunitResult(KunitStatus.SUCCESS, parse_time), test

def run_tests(linux: kunit_kernel.LinuxSourceTree,
	      request: KunitRequest) -> KunitResult:
	run_start = time.time()

	config_result = config_tests(linux, request)
	if config_result.status != KunitStatus.SUCCESS:
		return config_result

	build_result = build_tests(linux, request)
	if build_result.status != KunitStatus.SUCCESS:
		return build_result

	exec_result = exec_tests(linux, request)

	run_end = time.time()

	stdout.print_with_timestamp((
		'Elapsed time: %.3fs total, %.3fs configuring, %.3fs ' +
		'building, %.3fs running\n') % (
				run_end - run_start,
				config_result.elapsed_time,
				build_result.elapsed_time,
				exec_result.elapsed_time))
	return exec_result

# Problem:
# $ kunit.py run --json
# works as one would expect and prints the parsed test results as JSON.
# $ kunit.py run --json suite_name
# would *not* pass suite_name as the filter_glob and print as json.
# argparse will consider it to be another way of writing
# $ kunit.py run --json=suite_name
# i.e. it would run all tests, and dump the json to a `suite_name` file.
# So we hackily automatically rewrite --json => --json=stdout
pseudo_bool_flag_defaults = {
		'--json': 'stdout',
		'--raw_output': 'kunit',
}
def massage_argv(argv: Sequence[str]) -> Sequence[str]:
	def massage_arg(arg: str) -> str:
		if arg not in pseudo_bool_flag_defaults:
			return arg
		return  f'{arg}={pseudo_bool_flag_defaults[arg]}'
	return list(map(massage_arg, argv))

def get_default_jobs() -> int:
	return len(os.sched_getaffinity(0))

def add_common_opts(parser: argparse.ArgumentParser) -> None:
	parser.add_argument('--build_dir',
			    help='As in the make command, it specifies the build '
			    'directory.',
			    type=str, default='.kunit', metavar='DIR')
	parser.add_argument('--make_options',
			    help='X=Y make option, can be repeated.',
			    action='append', metavar='X=Y')
	parser.add_argument('--alltests',
			    help='Run all KUnit tests via tools/testing/kunit/configs/all_tests.config',
			    action='store_true')
	parser.add_argument('--kunitconfig',
			     help='Path to Kconfig fragment that enables KUnit tests.'
			     ' If given a directory, (e.g. lib/kunit), "/.kunitconfig" '
			     'will get  automatically appended. If repeated, the files '
			     'blindly concatenated, which might not work in all cases.',
			     action='append', metavar='PATHS')
	parser.add_argument('--kconfig_add',
			     help='Additional Kconfig options to append to the '
			     '.kunitconfig, e.g. CONFIG_KASAN=y. Can be repeated.',
			    action='append', metavar='CONFIG_X=Y')

	parser.add_argument('--arch',
			    help=('Specifies the architecture to run tests under. '
				  'The architecture specified here must match the '
				  'string passed to the ARCH make param, '
				  'e.g. i386, x86_64, arm, um, etc. Non-UML '
				  'architectures run on QEMU.'),
			    type=str, default='um', metavar='ARCH')

	parser.add_argument('--cross_compile',
			    help=('Sets make\'s CROSS_COMPILE variable; it should '
				  'be set to a toolchain path prefix (the prefix '
				  'of gcc and other tools in your toolchain, for '
				  'example `sparc64-linux-gnu-` if you have the '
				  'sparc toolchain installed on your system, or '
				  '`$HOME/toolchains/microblaze/gcc-9.2.0-nolibc/microblaze-linux/bin/microblaze-linux-` '
				  'if you have downloaded the microblaze toolchain '
				  'from the 0-day website to a directory in your '
				  'home directory called `toolchains`).'),
			    metavar='PREFIX')

	parser.add_argument('--qemu_config',
			    help=('Takes a path to a path to a file containing '
				  'a QemuArchParams object.'),
			    type=str, metavar='FILE')

	parser.add_argument('--qemu_args',
			    help='Additional QEMU arguments, e.g. "-smp 8"',
			    action='append', metavar='')

def add_build_opts(parser: argparse.ArgumentParser) -> None:
	parser.add_argument('--jobs',
			    help='As in the make command, "Specifies  the number of '
			    'jobs (commands) to run simultaneously."',
			    type=int, default=get_default_jobs(), metavar='N')

def add_exec_opts(parser: argparse.ArgumentParser) -> None:
	parser.add_argument('--timeout',
			    help='maximum number of seconds to allow for all tests '
			    'to run. This does not include time taken to build the '
			    'tests.',
			    type=int,
			    default=300,
			    metavar='SECONDS')
	parser.add_argument('filter_glob',
			    help='Filter which KUnit test suites/tests run at '
			    'boot-time, e.g. list* or list*.*del_test',
			    type=str,
			    nargs='?',
			    default='',
			    metavar='filter_glob')
	parser.add_argument('--filter',
			    help='Filter KUnit tests with attributes, '
			    'e.g. module=example or speed>slow',
			    type=str,
				default='')
	parser.add_argument('--filter_action',
			    help='If set to skip, filtered tests will be skipped, '
				'e.g. --filter_action=skip. Otherwise they will not run.',
			    type=str,
				choices=['skip'])
	parser.add_argument('--kernel_args',
			    help='Kernel command-line parameters. Maybe be repeated',
			     action='append', metavar='')
	parser.add_argument('--run_isolated', help='If set, boot the kernel for each '
			    'individual suite/test. This is can be useful for debugging '
			    'a non-hermetic test, one that might pass/fail based on '
			    'what ran before it.',
			    type=str,
			    choices=['suite', 'test'])
	parser.add_argument('--list_tests', help='If set, list all tests that will be '
			    'run.',
			    action='store_true')
	parser.add_argument('--list_tests_attr', help='If set, list all tests and test '
			    'attributes.',
			    action='store_true')

def add_parse_opts(parser: argparse.ArgumentParser) -> None:
	parser.add_argument('--raw_output', help='If set don\'t parse output from kernel. '
			    'By default, filters to just KUnit output. Use '
			    '--raw_output=all to show everything',
			     type=str, nargs='?', const='all', default=None, choices=['all', 'kunit'])
	parser.add_argument('--json',
			    nargs='?',
			    help='Prints parsed test results as JSON to stdout or a file if '
			    'a filename is specified. Does nothing if --raw_output is set.',
			    type=str, const='stdout', default=None, metavar='FILE')


def tree_from_args(cli_args: argparse.Namespace) -> kunit_kernel.LinuxSourceTree:
	"""Returns a LinuxSourceTree based on the user's arguments."""
	# Allow users to specify multiple arguments in one string, e.g. '-smp 8'
	qemu_args: List[str] = []
	if cli_args.qemu_args:
		for arg in cli_args.qemu_args:
			qemu_args.extend(shlex.split(arg))

	kunitconfigs = cli_args.kunitconfig if cli_args.kunitconfig else []
	if cli_args.alltests:
		# Prepend so user-specified options take prio if we ever allow
		# --kunitconfig options to have differing options.
		kunitconfigs = [kunit_kernel.ALL_TESTS_CONFIG_PATH] + kunitconfigs

	return kunit_kernel.LinuxSourceTree(cli_args.build_dir,
			kunitconfig_paths=kunitconfigs,
			kconfig_add=cli_args.kconfig_add,
			arch=cli_args.arch,
			cross_compile=cli_args.cross_compile,
			qemu_config_path=cli_args.qemu_config,
			extra_qemu_args=qemu_args)


def run_handler(cli_args: argparse.Namespace) -> None:
	if not os.path.exists(cli_args.build_dir):
		os.mkdir(cli_args.build_dir)

	linux = tree_from_args(cli_args)
	request = KunitRequest(build_dir=cli_args.build_dir,
					make_options=cli_args.make_options,
					jobs=cli_args.jobs,
					raw_output=cli_args.raw_output,
					json=cli_args.json,
					timeout=cli_args.timeout,
					filter_glob=cli_args.filter_glob,
					filter=cli_args.filter,
					filter_action=cli_args.filter_action,
					kernel_args=cli_args.kernel_args,
					run_isolated=cli_args.run_isolated,
					list_tests=cli_args.list_tests,
					list_tests_attr=cli_args.list_tests_attr)
	result = run_tests(linux, request)
	if result.status != KunitStatus.SUCCESS:
		sys.exit(1)


def config_handler(cli_args: argparse.Namespace) -> None:
	if cli_args.build_dir and (
			not os.path.exists(cli_args.build_dir)):
		os.mkdir(cli_args.build_dir)

	linux = tree_from_args(cli_args)
	request = KunitConfigRequest(build_dir=cli_args.build_dir,
						make_options=cli_args.make_options)
	result = config_tests(linux, request)
	stdout.print_with_timestamp((
		'Elapsed time: %.3fs\n') % (
			result.elapsed_time))
	if result.status != KunitStatus.SUCCESS:
		sys.exit(1)


def build_handler(cli_args: argparse.Namespace) -> None:
	linux = tree_from_args(cli_args)
	request = KunitBuildRequest(build_dir=cli_args.build_dir,
					make_options=cli_args.make_options,
					jobs=cli_args.jobs)
	result = config_and_build_tests(linux, request)
	stdout.print_with_timestamp((
		'Elapsed time: %.3fs\n') % (
			result.elapsed_time))
	if result.status != KunitStatus.SUCCESS:
		sys.exit(1)


def exec_handler(cli_args: argparse.Namespace) -> None:
	linux = tree_from_args(cli_args)
	exec_request = KunitExecRequest(raw_output=cli_args.raw_output,
					build_dir=cli_args.build_dir,
					json=cli_args.json,
					timeout=cli_args.timeout,
					filter_glob=cli_args.filter_glob,
					filter=cli_args.filter,
					filter_action=cli_args.filter_action,
					kernel_args=cli_args.kernel_args,
					run_isolated=cli_args.run_isolated,
					list_tests=cli_args.list_tests,
					list_tests_attr=cli_args.list_tests_attr)
	result = exec_tests(linux, exec_request)
	stdout.print_with_timestamp((
		'Elapsed time: %.3fs\n') % (result.elapsed_time))
	if result.status != KunitStatus.SUCCESS:
		sys.exit(1)


def parse_handler(cli_args: argparse.Namespace) -> None:
	if cli_args.file is None:
		sys.stdin.reconfigure(errors='backslashreplace')  # type: ignore
		kunit_output = sys.stdin  # type: Iterable[str]
	else:
		with open(cli_args.file, 'r', errors='backslashreplace') as f:
			kunit_output = f.read().splitlines()
	# We know nothing about how the result was created!
	metadata = kunit_json.Metadata()
	request = KunitParseRequest(raw_output=cli_args.raw_output,
					json=cli_args.json)
	result, _ = parse_tests(request, metadata, kunit_output)
	if result.status != KunitStatus.SUCCESS:
		sys.exit(1)


subcommand_handlers_map = {
	'run': run_handler,
	'config': config_handler,
	'build': build_handler,
	'exec': exec_handler,
	'parse': parse_handler
}


def main(argv: Sequence[str]) -> None:
	parser = argparse.ArgumentParser(
			description='Helps writing and running KUnit tests.')
	subparser = parser.add_subparsers(dest='subcommand')

	# The 'run' command will config, build, exec, and parse in one go.
	run_parser = subparser.add_parser('run', help='Runs KUnit tests.')
	add_common_opts(run_parser)
	add_build_opts(run_parser)
	add_exec_opts(run_parser)
	add_parse_opts(run_parser)

	config_parser = subparser.add_parser('config',
						help='Ensures that .config contains all of '
						'the options in .kunitconfig')
	add_common_opts(config_parser)

	build_parser = subparser.add_parser('build', help='Builds a kernel with KUnit tests')
	add_common_opts(build_parser)
	add_build_opts(build_parser)

	exec_parser = subparser.add_parser('exec', help='Run a kernel with KUnit tests')
	add_common_opts(exec_parser)
	add_exec_opts(exec_parser)
	add_parse_opts(exec_parser)

	# The 'parse' option is special, as it doesn't need the kernel source
	# (therefore there is no need for a build_dir, hence no add_common_opts)
	# and the '--file' argument is not relevant to 'run', so isn't in
	# add_parse_opts()
	parse_parser = subparser.add_parser('parse',
					    help='Parses KUnit results from a file, '
					    'and parses formatted results.')
	add_parse_opts(parse_parser)
	parse_parser.add_argument('file',
				  help='Specifies the file to read results from.',
				  type=str, nargs='?', metavar='input_file')

	cli_args = parser.parse_args(massage_argv(argv))

	if get_kernel_root_path():
		os.chdir(get_kernel_root_path())

	subcomand_handler = subcommand_handlers_map.get(cli_args.subcommand, None)

	if subcomand_handler is None:
		parser.print_help()
		return

	subcomand_handler(cli_args)


if __name__ == '__main__':
	main(sys.argv[1:])

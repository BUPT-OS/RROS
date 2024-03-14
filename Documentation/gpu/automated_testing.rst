.. SPDX-License-Identifier: GPL-2.0+

=========================================
Automated testing of the DRM subsystem
=========================================

Introduction
============

Making sure that changes to the core or drivers don't introduce regressions can
be very time-consuming when lots of different hardware configurations need to
be tested. Moreover, it isn't practical for each person interested in this
testing to have to acquire and maintain what can be a considerable amount of
hardware.

Also, it is desirable for developers to check for regressions in their code by
themselves, instead of relying on the maintainers to find them and then
reporting back.

There are facilities in gitlab.freedesktop.org to automatically test Mesa that
can be used as well for testing the DRM subsystem. This document explains how
people interested in testing it can use this shared infrastructure to save
quite some time and effort.


Relevant files
==============

drivers/gpu/drm/ci/gitlab-ci.yml
--------------------------------

This is the root configuration file for GitLab CI. Among other less interesting
bits, it specifies the specific version of the scripts to be used. There are
some variables that can be modified to change the behavior of the pipeline:

DRM_CI_PROJECT_PATH
    Repository that contains the Mesa software infrastructure for CI

DRM_CI_COMMIT_SHA
    A particular revision to use from that repository

UPSTREAM_REPO
    URL to git repository containing the target branch

TARGET_BRANCH
    Branch to which this branch is to be merged into

IGT_VERSION
    Revision of igt-gpu-tools being used, from
    https://gitlab.freedesktop.org/drm/igt-gpu-tools

drivers/gpu/drm/ci/testlist.txt
-------------------------------

IGT tests to be run on all drivers (unless mentioned in a driver's \*-skips.txt
file, see below).

drivers/gpu/drm/ci/${DRIVER_NAME}-${HW_REVISION}-fails.txt
----------------------------------------------------------

Lists the known failures for a given driver on a specific hardware revision.

drivers/gpu/drm/ci/${DRIVER_NAME}-${HW_REVISION}-flakes.txt
-----------------------------------------------------------

Lists the tests that for a given driver on a specific hardware revision are
known to behave unreliably. These tests won't cause a job to fail regardless of
the result. They will still be run.

drivers/gpu/drm/ci/${DRIVER_NAME}-${HW_REVISION}-skips.txt
-----------------------------------------------------------

Lists the tests that won't be run for a given driver on a specific hardware
revision. These are usually tests that interfere with the running of the test
list due to hanging the machine, causing OOM, taking too long, etc.


How to enable automated testing on your tree
============================================

1. Create a Linux tree in https://gitlab.freedesktop.org/ if you don't have one
yet

2. In your kernel repo's configuration (eg.
https://gitlab.freedesktop.org/janedoe/linux/-/settings/ci_cd), change the
CI/CD configuration file from .gitlab-ci.yml to
drivers/gpu/drm/ci/gitlab-ci.yml.

3. Next time you push to this repository, you will see a CI pipeline being
created (eg. https://gitlab.freedesktop.org/janedoe/linux/-/pipelines)

4. The various jobs will be run and when the pipeline is finished, all jobs
should be green unless a regression has been found.


How to update test expectations
===============================

If your changes to the code fix any tests, you will have to remove one or more
lines from one or more of the files in
drivers/gpu/drm/ci/${DRIVER_NAME}_*_fails.txt, for each of the test platforms
affected by the change.


How to expand coverage
======================

If your code changes make it possible to run more tests (by solving reliability
issues, for example), you can remove tests from the flakes and/or skips lists,
and then the expected results if there are any known failures.

If there is a need for updating the version of IGT being used (maybe you have
added more tests to it), update the IGT_VERSION variable at the top of the
gitlab-ci.yml file.


How to test your changes to the scripts
=======================================

For testing changes to the scripts in the drm-ci repo, change the
DRM_CI_PROJECT_PATH and DRM_CI_COMMIT_SHA variables in
drivers/gpu/drm/ci/gitlab-ci.yml to match your fork of the project (eg.
janedoe/drm-ci). This fork needs to be in https://gitlab.freedesktop.org/.


How to incorporate external fixes in your testing
=================================================

Often, regressions in other trees will prevent testing changes local to the
tree under test. These fixes will be automatically merged in during the build
jobs from a branch in the target tree that is named as
${TARGET_BRANCH}-external-fixes.

If the pipeline is not in a merge request and a branch with the same name
exists in the local tree, commits from that branch will be merged in as well.


How to deal with automated testing labs that may be down
========================================================

If a hardware farm is down and thus causing pipelines to fail that would
otherwise pass, one can disable all jobs that would be submitted to that farm
by editing the file at
https://gitlab.freedesktop.org/gfx-ci/lab-status/-/blob/main/lab-status.yml.

.. _drm-client-usage-stats:

======================
DRM client usage stats
======================

DRM drivers can choose to export partly standardised text output via the
`fops->show_fdinfo()` as part of the driver specific file operations registered
in the `struct drm_driver` object registered with the DRM core.

One purpose of this output is to enable writing as generic as practically
feasible `top(1)` like userspace monitoring tools.

Given the differences between various DRM drivers the specification of the
output is split between common and driver specific parts. Having said that,
wherever possible effort should still be made to standardise as much as
possible.

File format specification
=========================

- File shall contain one key value pair per one line of text.
- Colon character (`:`) must be used to delimit keys and values.
- All keys shall be prefixed with `drm-`.
- Whitespace between the delimiter and first non-whitespace character shall be
  ignored when parsing.
- Keys are not allowed to contain whitespace characters.
- Numerical key value pairs can end with optional unit string.
- Data type of the value is fixed as defined in the specification.

Key types
---------

1. Mandatory, fully standardised.
2. Optional, fully standardised.
3. Driver specific.

Data types
----------

- <uint> - Unsigned integer without defining the maximum value.
- <keystr> - String excluding any above defined reserved characters or whitespace.
- <valstr> - String.

Mandatory fully standardised keys
---------------------------------

- drm-driver: <valstr>

String shall contain the name this driver registered as via the respective
`struct drm_driver` data structure.

Optional fully standardised keys
--------------------------------

Identification
^^^^^^^^^^^^^^

- drm-pdev: <aaaa:bb.cc.d>

For PCI devices this should contain the PCI slot address of the device in
question.

- drm-client-id: <uint>

Unique value relating to the open DRM file descriptor used to distinguish
duplicated and shared file descriptors. Conceptually the value should map 1:1
to the in kernel representation of `struct drm_file` instances.

Uniqueness of the value shall be either globally unique, or unique within the
scope of each device, in which case `drm-pdev` shall be present as well.

Userspace should make sure to not double account any usage statistics by using
the above described criteria in order to associate data to individual clients.

Utilization
^^^^^^^^^^^

- drm-engine-<keystr>: <uint> ns

GPUs usually contain multiple execution engines. Each shall be given a stable
and unique name (keystr), with possible values documented in the driver specific
documentation.

Value shall be in specified time units which the respective GPU engine spent
busy executing workloads belonging to this client.

Values are not required to be constantly monotonic if it makes the driver
implementation easier, but are required to catch up with the previously reported
larger value within a reasonable period. Upon observing a value lower than what
was previously read, userspace is expected to stay with that larger previous
value until a monotonic update is seen.

- drm-engine-capacity-<keystr>: <uint>

Engine identifier string must be the same as the one specified in the
drm-engine-<keystr> tag and shall contain a greater than zero number in case the
exported engine corresponds to a group of identical hardware engines.

In the absence of this tag parser shall assume capacity of one. Zero capacity
is not allowed.

- drm-cycles-<keystr>: <uint>

Engine identifier string must be the same as the one specified in the
drm-engine-<keystr> tag and shall contain the number of busy cycles for the given
engine.

Values are not required to be constantly monotonic if it makes the driver
implementation easier, but are required to catch up with the previously reported
larger value within a reasonable period. Upon observing a value lower than what
was previously read, userspace is expected to stay with that larger previous
value until a monotonic update is seen.

- drm-maxfreq-<keystr>: <uint> [Hz|MHz|KHz]

Engine identifier string must be the same as the one specified in the
drm-engine-<keystr> tag and shall contain the maximum frequency for the given
engine.  Taken together with drm-cycles-<keystr>, this can be used to calculate
percentage utilization of the engine, whereas drm-engine-<keystr> only reflects
time active without considering what frequency the engine is operating as a
percentage of its maximum frequency.

Memory
^^^^^^

- drm-memory-<region>: <uint> [KiB|MiB]

Each possible memory type which can be used to store buffer objects by the
GPU in question shall be given a stable and unique name to be returned as the
string here.  The name "memory" is reserved to refer to normal system memory.

Value shall reflect the amount of storage currently consumed by the buffer
objects belong to this client, in the respective memory region.

Default unit shall be bytes with optional unit specifiers of 'KiB' or 'MiB'
indicating kibi- or mebi-bytes.

- drm-shared-<region>: <uint> [KiB|MiB]

The total size of buffers that are shared with another file (ie. have more
than a single handle).

- drm-total-<region>: <uint> [KiB|MiB]

The total size of buffers that including shared and private memory.

- drm-resident-<region>: <uint> [KiB|MiB]

The total size of buffers that are resident in the specified region.

- drm-purgeable-<region>: <uint> [KiB|MiB]

The total size of buffers that are purgeable.

- drm-active-<region>: <uint> [KiB|MiB]

The total size of buffers that are active on one or more engines.

Implementation Details
======================

Drivers should use drm_show_fdinfo() in their `struct file_operations`, and
implement &drm_driver.show_fdinfo if they wish to provide any stats which
are not provided by drm_show_fdinfo().  But even driver specific stats should
be documented above and where possible, aligned with other drivers.

Driver specific implementations
-------------------------------

:ref:`i915-usage-stats`

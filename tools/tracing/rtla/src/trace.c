// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/sendfile.h>
#include <tracefs.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "trace.h"
#include "utils.h"

/*
 * enable_tracer_by_name - enable a tracer on the given instance
 */
int enable_tracer_by_name(struct tracefs_instance *inst, const char *tracer_name)
{
	enum tracefs_tracers tracer;
	int retval;

	tracer = TRACEFS_TRACER_CUSTOM;

	debug_msg("Enabling %s tracer\n", tracer_name);

	retval = tracefs_tracer_set(inst, tracer, tracer_name);
	if (retval < 0) {
		if (errno == ENODEV)
			err_msg("Tracer %s not found!\n", tracer_name);

		err_msg("Failed to enable the %s tracer\n", tracer_name);
		return -1;
	}

	return 0;
}

/*
 * disable_tracer - set nop tracer to the insta
 */
void disable_tracer(struct tracefs_instance *inst)
{
	enum tracefs_tracers t = TRACEFS_TRACER_NOP;
	int retval;

	retval = tracefs_tracer_set(inst, t);
	if (retval < 0)
		err_msg("Oops, error disabling tracer\n");
}

/*
 * create_instance - create a trace instance with *instance_name
 */
struct tracefs_instance *create_instance(char *instance_name)
{
	return tracefs_instance_create(instance_name);
}

/*
 * destroy_instance - remove a trace instance and free the data
 */
void destroy_instance(struct tracefs_instance *inst)
{
	tracefs_instance_destroy(inst);
	tracefs_instance_free(inst);
}

/*
 * save_trace_to_file - save the trace output of the instance to the file
 */
int save_trace_to_file(struct tracefs_instance *inst, const char *filename)
{
	const char *file = "trace";
	mode_t mode = 0644;
	char buffer[4096];
	int out_fd, in_fd;
	int retval = -1;

	in_fd = tracefs_instance_file_open(inst, file, O_RDONLY);
	if (in_fd < 0) {
		err_msg("Failed to open trace file\n");
		return -1;
	}

	out_fd = creat(filename, mode);
	if (out_fd < 0) {
		err_msg("Failed to create output file %s\n", filename);
		goto out_close_in;
	}

	do {
		retval = read(in_fd, buffer, sizeof(buffer));
		if (retval <= 0)
			goto out_close;

		retval = write(out_fd, buffer, retval);
		if (retval < 0)
			goto out_close;
	} while (retval > 0);

	retval = 0;
out_close:
	close(out_fd);
out_close_in:
	close(in_fd);
	return retval;
}

/*
 * collect_registered_events - call the existing callback function for the event
 *
 * If an event has a registered callback function, call it.
 * Otherwise, ignore the event.
 */
int
collect_registered_events(struct tep_event *event, struct tep_record *record,
			  int cpu, void *context)
{
	struct trace_instance *trace = context;
	struct trace_seq *s = trace->seq;

	if (!event->handler)
		return 0;

	event->handler(s, record, event, context);

	return 0;
}

/*
 * trace_instance_destroy - destroy and free a rtla trace instance
 */
void trace_instance_destroy(struct trace_instance *trace)
{
	if (trace->inst) {
		disable_tracer(trace->inst);
		destroy_instance(trace->inst);
		trace->inst = NULL;
	}

	if (trace->seq) {
		free(trace->seq);
		trace->seq = NULL;
	}

	if (trace->tep) {
		tep_free(trace->tep);
		trace->tep = NULL;
	}
}

/*
 * trace_instance_init - create an rtla trace instance
 *
 * It is more than the tracefs instance, as it contains other
 * things required for the tracing, such as the local events and
 * a seq file.
 *
 * Note that the trace instance is returned disabled. This allows
 * the tool to apply some other configs, like setting priority
 * to the kernel threads, before starting generating trace entries.
 */
int trace_instance_init(struct trace_instance *trace, char *tool_name)
{
	trace->seq = calloc(1, sizeof(*trace->seq));
	if (!trace->seq)
		goto out_err;

	trace_seq_init(trace->seq);

	trace->inst = create_instance(tool_name);
	if (!trace->inst)
		goto out_err;

	trace->tep = tracefs_local_events(NULL);
	if (!trace->tep)
		goto out_err;

	/*
	 * Let the main enable the record after setting some other
	 * things such as the priority of the tracer's threads.
	 */
	tracefs_trace_off(trace->inst);

	return 0;

out_err:
	trace_instance_destroy(trace);
	return 1;
}

/*
 * trace_instance_start - start tracing a given rtla instance
 */
int trace_instance_start(struct trace_instance *trace)
{
	return tracefs_trace_on(trace->inst);
}

/*
 * trace_events_free - free a list of trace events
 */
static void trace_events_free(struct trace_events *events)
{
	struct trace_events *tevent = events;
	struct trace_events *free_event;

	while (tevent) {
		free_event = tevent;

		tevent = tevent->next;

		if (free_event->filter)
			free(free_event->filter);
		if (free_event->trigger)
			free(free_event->trigger);
		free(free_event->system);
		free(free_event);
	}
}

/*
 * trace_event_alloc - alloc and parse a single trace event
 */
struct trace_events *trace_event_alloc(const char *event_string)
{
	struct trace_events *tevent;

	tevent = calloc(1, sizeof(*tevent));
	if (!tevent)
		return NULL;

	tevent->system = strdup(event_string);
	if (!tevent->system) {
		free(tevent);
		return NULL;
	}

	tevent->event = strstr(tevent->system, ":");
	if (tevent->event) {
		*tevent->event = '\0';
		tevent->event = &tevent->event[1];
	}

	return tevent;
}

/*
 * trace_event_add_filter - record an event filter
 */
int trace_event_add_filter(struct trace_events *event, char *filter)
{
	if (event->filter)
		free(event->filter);

	event->filter = strdup(filter);
	if (!event->filter)
		return 1;

	return 0;
}

/*
 * trace_event_add_trigger - record an event trigger action
 */
int trace_event_add_trigger(struct trace_events *event, char *trigger)
{
	if (event->trigger)
		free(event->trigger);

	event->trigger = strdup(trigger);
	if (!event->trigger)
		return 1;

	return 0;
}

/*
 * trace_event_disable_filter - disable an event filter
 */
static void trace_event_disable_filter(struct trace_instance *instance,
				       struct trace_events *tevent)
{
	char filter[1024];
	int retval;

	if (!tevent->filter)
		return;

	if (!tevent->filter_enabled)
		return;

	debug_msg("Disabling %s:%s filter %s\n", tevent->system,
		  tevent->event ? : "*", tevent->filter);

	snprintf(filter, 1024, "!%s\n", tevent->filter);

	retval = tracefs_event_file_write(instance->inst, tevent->system,
					  tevent->event, "filter", filter);
	if (retval < 0)
		err_msg("Error disabling %s:%s filter %s\n", tevent->system,
			tevent->event ? : "*", tevent->filter);
}

/*
 * trace_event_save_hist - save the content of an event hist
 *
 * If the trigger is a hist: one, save the content of the hist file.
 */
static void trace_event_save_hist(struct trace_instance *instance,
				  struct trace_events *tevent)
{
	int retval, index, out_fd;
	mode_t mode = 0644;
	char path[1024];
	char *hist;

	if (!tevent)
		return;

	/* trigger enables hist */
	if (!tevent->trigger)
		return;

	/* is this a hist: trigger? */
	retval = strncmp(tevent->trigger, "hist:", strlen("hist:"));
	if (retval)
		return;

	snprintf(path, 1024, "%s_%s_hist.txt", tevent->system, tevent->event);

	printf("  Saving event %s:%s hist to %s\n", tevent->system, tevent->event, path);

	out_fd = creat(path, mode);
	if (out_fd < 0) {
		err_msg("  Failed to create %s output file\n", path);
		return;
	}

	hist = tracefs_event_file_read(instance->inst, tevent->system, tevent->event, "hist", 0);
	if (!hist) {
		err_msg("  Failed to read %s:%s hist file\n", tevent->system, tevent->event);
		goto out_close;
	}

	index = 0;
	do {
		index += write(out_fd, &hist[index], strlen(hist) - index);
	} while (index < strlen(hist));

	free(hist);
out_close:
	close(out_fd);
}

/*
 * trace_event_disable_trigger - disable an event trigger
 */
static void trace_event_disable_trigger(struct trace_instance *instance,
					struct trace_events *tevent)
{
	char trigger[1024];
	int retval;

	if (!tevent->trigger)
		return;

	if (!tevent->trigger_enabled)
		return;

	debug_msg("Disabling %s:%s trigger %s\n", tevent->system,
		  tevent->event ? : "*", tevent->trigger);

	trace_event_save_hist(instance, tevent);

	snprintf(trigger, 1024, "!%s\n", tevent->trigger);

	retval = tracefs_event_file_write(instance->inst, tevent->system,
					  tevent->event, "trigger", trigger);
	if (retval < 0)
		err_msg("Error disabling %s:%s trigger %s\n", tevent->system,
			tevent->event ? : "*", tevent->trigger);
}

/*
 * trace_events_disable - disable all trace events
 */
void trace_events_disable(struct trace_instance *instance,
			  struct trace_events *events)
{
	struct trace_events *tevent = events;

	if (!events)
		return;

	while (tevent) {
		debug_msg("Disabling event %s:%s\n", tevent->system, tevent->event ? : "*");
		if (tevent->enabled) {
			trace_event_disable_filter(instance, tevent);
			trace_event_disable_trigger(instance, tevent);
			tracefs_event_disable(instance->inst, tevent->system, tevent->event);
		}

		tevent->enabled = 0;
		tevent = tevent->next;
	}
}

/*
 * trace_event_enable_filter - enable an event filter associated with an event
 */
static int trace_event_enable_filter(struct trace_instance *instance,
				     struct trace_events *tevent)
{
	char filter[1024];
	int retval;

	if (!tevent->filter)
		return 0;

	if (!tevent->event) {
		err_msg("Filter %s applies only for single events, not for all %s:* events\n",
			tevent->filter, tevent->system);
		return 1;
	}

	snprintf(filter, 1024, "%s\n", tevent->filter);

	debug_msg("Enabling %s:%s filter %s\n", tevent->system,
		  tevent->event ? : "*", tevent->filter);

	retval = tracefs_event_file_write(instance->inst, tevent->system,
					  tevent->event, "filter", filter);
	if (retval < 0) {
		err_msg("Error enabling %s:%s filter %s\n", tevent->system,
			tevent->event ? : "*", tevent->filter);
		return 1;
	}

	tevent->filter_enabled = 1;
	return 0;
}

/*
 * trace_event_enable_trigger - enable an event trigger associated with an event
 */
static int trace_event_enable_trigger(struct trace_instance *instance,
				      struct trace_events *tevent)
{
	char trigger[1024];
	int retval;

	if (!tevent->trigger)
		return 0;

	if (!tevent->event) {
		err_msg("Trigger %s applies only for single events, not for all %s:* events\n",
			tevent->trigger, tevent->system);
		return 1;
	}

	snprintf(trigger, 1024, "%s\n", tevent->trigger);

	debug_msg("Enabling %s:%s trigger %s\n", tevent->system,
		  tevent->event ? : "*", tevent->trigger);

	retval = tracefs_event_file_write(instance->inst, tevent->system,
					  tevent->event, "trigger", trigger);
	if (retval < 0) {
		err_msg("Error enabling %s:%s trigger %s\n", tevent->system,
			tevent->event ? : "*", tevent->trigger);
		return 1;
	}

	tevent->trigger_enabled = 1;

	return 0;
}

/*
 * trace_events_enable - enable all events
 */
int trace_events_enable(struct trace_instance *instance,
			struct trace_events *events)
{
	struct trace_events *tevent = events;
	int retval;

	while (tevent) {
		debug_msg("Enabling event %s:%s\n", tevent->system, tevent->event ? : "*");
		retval = tracefs_event_enable(instance->inst, tevent->system, tevent->event);
		if (retval < 0) {
			err_msg("Error enabling event %s:%s\n", tevent->system,
				tevent->event ? : "*");
			return 1;
		}

		retval = trace_event_enable_filter(instance, tevent);
		if (retval)
			return 1;

		retval = trace_event_enable_trigger(instance, tevent);
		if (retval)
			return 1;

		tevent->enabled = 1;
		tevent = tevent->next;
	}

	return 0;
}

/*
 * trace_events_destroy - disable and free all trace events
 */
void trace_events_destroy(struct trace_instance *instance,
			  struct trace_events *events)
{
	if (!events)
		return;

	trace_events_disable(instance, events);
	trace_events_free(events);
}

int trace_is_off(struct trace_instance *tool, struct trace_instance *trace)
{
	/*
	 * The tool instance is always present, it is the one used to collect
	 * data.
	 */
	if (!tracefs_trace_is_on(tool->inst))
		return 1;

	/*
	 * The trace instance is only enabled when -t is set. IOW, when the system
	 * is tracing.
	 */
	if (trace && !tracefs_trace_is_on(trace->inst))
		return 1;

	return 0;
}

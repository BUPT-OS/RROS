%define api.pure full
%parse-param {void *_parse_state}
%parse-param {void *scanner}
%lex-param {void* scanner}
%locations

%{

#define YYDEBUG 1

#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/zalloc.h>
#include "pmu.h"
#include "pmus.h"
#include "evsel.h"
#include "parse-events.h"
#include "parse-events-bison.h"

int parse_events_lex(YYSTYPE * yylval_param, YYLTYPE * yylloc_param , void *yyscanner);
void parse_events_error(YYLTYPE *loc, void *parse_state, void *scanner, char const *msg);

#define PE_ABORT(val) \
do { \
	if (val == -ENOMEM) \
		YYNOMEM; \
	YYABORT; \
} while (0)

static struct list_head* alloc_list(void)
{
	struct list_head *list;

	list = malloc(sizeof(*list));
	if (!list)
		return NULL;

	INIT_LIST_HEAD(list);
	return list;
}

static void free_list_evsel(struct list_head* list_evsel)
{
	struct evsel *evsel, *tmp;

	list_for_each_entry_safe(evsel, tmp, list_evsel, core.node) {
		list_del_init(&evsel->core.node);
		evsel__delete(evsel);
	}
	free(list_evsel);
}

%}

%token PE_START_EVENTS PE_START_TERMS
%token PE_VALUE PE_VALUE_SYM_HW PE_VALUE_SYM_SW PE_TERM
%token PE_VALUE_SYM_TOOL
%token PE_EVENT_NAME
%token PE_RAW PE_NAME
%token PE_MODIFIER_EVENT PE_MODIFIER_BP PE_BP_COLON PE_BP_SLASH
%token PE_LEGACY_CACHE
%token PE_PREFIX_MEM
%token PE_ERROR
%token PE_DRV_CFG_TERM
%token PE_TERM_HW
%type <num> PE_VALUE
%type <num> PE_VALUE_SYM_HW
%type <num> PE_VALUE_SYM_SW
%type <num> PE_VALUE_SYM_TOOL
%type <num> PE_TERM
%type <num> value_sym
%type <str> PE_RAW
%type <str> PE_NAME
%type <str> PE_LEGACY_CACHE
%type <str> PE_MODIFIER_EVENT
%type <str> PE_MODIFIER_BP
%type <str> PE_EVENT_NAME
%type <str> PE_DRV_CFG_TERM
%type <str> name_or_raw name_or_legacy
%destructor { free ($$); } <str>
%type <term> event_term
%destructor { parse_events_term__delete ($$); } <term>
%type <list_terms> event_config
%type <list_terms> opt_event_config
%type <list_terms> opt_pmu_config
%destructor { parse_events_terms__delete ($$); } <list_terms>
%type <list_evsel> event_pmu
%type <list_evsel> event_legacy_symbol
%type <list_evsel> event_legacy_cache
%type <list_evsel> event_legacy_mem
%type <list_evsel> event_legacy_tracepoint
%type <list_evsel> event_legacy_numeric
%type <list_evsel> event_legacy_raw
%type <list_evsel> event_def
%type <list_evsel> event_mod
%type <list_evsel> event_name
%type <list_evsel> event
%type <list_evsel> events
%type <list_evsel> group_def
%type <list_evsel> group
%type <list_evsel> groups
%destructor { free_list_evsel ($$); } <list_evsel>
%type <tracepoint_name> tracepoint_name
%type <hardware_term> PE_TERM_HW
%destructor { free ($$.str); } <hardware_term>

%union
{
	char *str;
	u64 num;
	struct list_head *list_evsel;
	struct list_head *list_terms;
	struct parse_events_term *term;
	struct tracepoint_name {
		char *sys;
		char *event;
	} tracepoint_name;
	struct hardware_term {
		char *str;
		u64 num;
	} hardware_term;
}
%%

start:
PE_START_EVENTS start_events
|
PE_START_TERMS  start_terms

start_events: groups
{
	struct parse_events_state *parse_state = _parse_state;

	/* frees $1 */
	parse_events_update_lists($1, &parse_state->list);
}

groups:
groups ',' group
{
	struct list_head *list  = $1;
	struct list_head *group = $3;

	/* frees $3 */
	parse_events_update_lists(group, list);
	$$ = list;
}
|
groups ',' event
{
	struct list_head *list  = $1;
	struct list_head *event = $3;

	/* frees $3 */
	parse_events_update_lists(event, list);
	$$ = list;
}
|
group
|
event

group:
group_def ':' PE_MODIFIER_EVENT
{
	struct list_head *list = $1;
	int err;

	err = parse_events__modifier_group(list, $3);
	free($3);
	if (err) {
		struct parse_events_state *parse_state = _parse_state;
		struct parse_events_error *error = parse_state->error;

		parse_events_error__handle(error, @3.first_column,
					   strdup("Bad modifier"), NULL);
		free_list_evsel(list);
		YYABORT;
	}
	$$ = list;
}
|
group_def

group_def:
PE_NAME '{' events '}'
{
	struct list_head *list = $3;

	/* Takes ownership of $1. */
	parse_events__set_leader($1, list);
	$$ = list;
}
|
'{' events '}'
{
	struct list_head *list = $2;

	parse_events__set_leader(NULL, list);
	$$ = list;
}

events:
events ',' event
{
	struct list_head *event = $3;
	struct list_head *list  = $1;

	/* frees $3 */
	parse_events_update_lists(event, list);
	$$ = list;
}
|
event

event: event_mod

event_mod:
event_name PE_MODIFIER_EVENT
{
	struct list_head *list = $1;
	int err;

	/*
	 * Apply modifier on all events added by single event definition
	 * (there could be more events added for multiple tracepoint
	 * definitions via '*?'.
	 */
	err = parse_events__modifier_event(list, $2, false);
	free($2);
	if (err) {
		struct parse_events_state *parse_state = _parse_state;
		struct parse_events_error *error = parse_state->error;

		parse_events_error__handle(error, @2.first_column,
					   strdup("Bad modifier"), NULL);
		free_list_evsel(list);
		YYABORT;
	}
	$$ = list;
}
|
event_name

event_name:
PE_EVENT_NAME event_def
{
	int err;

	err = parse_events_name($2, $1);
	free($1);
	if (err) {
		free_list_evsel($2);
		YYNOMEM;
	}
	$$ = $2;
}
|
event_def

event_def: event_pmu |
	   event_legacy_symbol |
	   event_legacy_cache sep_dc |
	   event_legacy_mem sep_dc |
	   event_legacy_tracepoint sep_dc |
	   event_legacy_numeric sep_dc |
	   event_legacy_raw sep_dc

event_pmu:
PE_NAME opt_pmu_config
{
	struct parse_events_state *parse_state = _parse_state;
	struct list_head *list = NULL, *orig_terms = NULL, *terms= NULL;
	char *pattern = NULL;

#define CLEANUP						\
	do {						\
		parse_events_terms__delete($2);		\
		parse_events_terms__delete(orig_terms);	\
		free(list);				\
		free($1);				\
		free(pattern);				\
	} while(0)

	if (parse_events_copy_term_list($2, &orig_terms)) {
		CLEANUP;
		YYNOMEM;
	}

	list = alloc_list();
	if (!list) {
		CLEANUP;
		YYNOMEM;
	}
	/* Attempt to add to list assuming $1 is a PMU name. */
	if (parse_events_add_pmu(parse_state, list, $1, $2, /*auto_merge_stats=*/false, &@1)) {
		struct perf_pmu *pmu = NULL;
		int ok = 0;

		/* Failure to add, try wildcard expansion of $1 as a PMU name. */
		if (asprintf(&pattern, "%s*", $1) < 0) {
			CLEANUP;
			YYNOMEM;
		}

		while ((pmu = perf_pmus__scan(pmu)) != NULL) {
			const char *name = pmu->name;

			if (parse_events__filter_pmu(parse_state, pmu))
				continue;

			if (!strncmp(name, "uncore_", 7) &&
			    strncmp($1, "uncore_", 7))
				name += 7;
			if (!perf_pmu__match(pattern, name, $1) ||
			    !perf_pmu__match(pattern, pmu->alias_name, $1)) {
				bool auto_merge_stats = perf_pmu__auto_merge_stats(pmu);

				if (parse_events_copy_term_list(orig_terms, &terms)) {
					CLEANUP;
					YYNOMEM;
				}
				if (!parse_events_add_pmu(parse_state, list, pmu->name, terms,
							  auto_merge_stats, &@1)) {
					ok++;
					parse_state->wild_card_pmus = true;
				}
				parse_events_terms__delete(terms);
			}
		}

		if (!ok) {
			/* Failure to add, assume $1 is an event name. */
			zfree(&list);
			ok = !parse_events_multi_pmu_add(parse_state, $1, $2, &list, &@1);
			$2 = NULL;
		}
		if (!ok) {
			struct parse_events_error *error = parse_state->error;
			char *help;

			if (asprintf(&help, "Unable to find PMU or event on a PMU of '%s'", $1) < 0)
				help = NULL;
			parse_events_error__handle(error, @1.first_column,
						   strdup("Bad event or PMU"),
						   help);
			CLEANUP;
			YYABORT;
		}
	}
	$$ = list;
	list = NULL;
	CLEANUP;
#undef CLEANUP
}
|
PE_NAME sep_dc
{
	struct list_head *list;
	int err;

	err = parse_events_multi_pmu_add(_parse_state, $1, NULL, &list, &@1);
	if (err < 0) {
		struct parse_events_state *parse_state = _parse_state;
		struct parse_events_error *error = parse_state->error;
		char *help;

		if (asprintf(&help, "Unable to find event on a PMU of '%s'", $1) < 0)
			help = NULL;
		parse_events_error__handle(error, @1.first_column, strdup("Bad event name"), help);
		free($1);
		PE_ABORT(err);
	}
	free($1);
	$$ = list;
}

value_sym:
PE_VALUE_SYM_HW
|
PE_VALUE_SYM_SW

event_legacy_symbol:
value_sym '/' event_config '/'
{
	struct list_head *list;
	int type = $1 >> 16;
	int config = $1 & 255;
	int err;
	bool wildcard = (type == PERF_TYPE_HARDWARE || type == PERF_TYPE_HW_CACHE);

	list = alloc_list();
	if (!list)
		YYNOMEM;
	err = parse_events_add_numeric(_parse_state, list, type, config, $3, wildcard);
	parse_events_terms__delete($3);
	if (err) {
		free_list_evsel(list);
		PE_ABORT(err);
	}
	$$ = list;
}
|
value_sym sep_slash_slash_dc
{
	struct list_head *list;
	int type = $1 >> 16;
	int config = $1 & 255;
	bool wildcard = (type == PERF_TYPE_HARDWARE || type == PERF_TYPE_HW_CACHE);
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;
	err = parse_events_add_numeric(_parse_state, list, type, config, /*head_config=*/NULL, wildcard);
	if (err)
		PE_ABORT(err);
	$$ = list;
}
|
PE_VALUE_SYM_TOOL sep_slash_slash_dc
{
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;
	err = parse_events_add_tool(_parse_state, list, $1);
	if (err)
		YYNOMEM;
	$$ = list;
}

event_legacy_cache:
PE_LEGACY_CACHE opt_event_config
{
	struct parse_events_state *parse_state = _parse_state;
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;

	err = parse_events_add_cache(list, &parse_state->idx, $1, parse_state, $2);

	parse_events_terms__delete($2);
	free($1);
	if (err) {
		free_list_evsel(list);
		PE_ABORT(err);
	}
	$$ = list;
}

event_legacy_mem:
PE_PREFIX_MEM PE_VALUE PE_BP_SLASH PE_VALUE PE_BP_COLON PE_MODIFIER_BP opt_event_config
{
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;

	err = parse_events_add_breakpoint(_parse_state, list,
					  $2, $6, $4, $7);
	parse_events_terms__delete($7);
	free($6);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}
|
PE_PREFIX_MEM PE_VALUE PE_BP_SLASH PE_VALUE opt_event_config
{
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;

	err = parse_events_add_breakpoint(_parse_state, list,
					  $2, NULL, $4, $5);
	parse_events_terms__delete($5);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}
|
PE_PREFIX_MEM PE_VALUE PE_BP_COLON PE_MODIFIER_BP opt_event_config
{
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;

	err = parse_events_add_breakpoint(_parse_state, list,
					  $2, $4, 0, $5);
	parse_events_terms__delete($5);
	free($4);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}
|
PE_PREFIX_MEM PE_VALUE opt_event_config
{
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;
	err = parse_events_add_breakpoint(_parse_state, list,
					  $2, NULL, 0, $3);
	parse_events_terms__delete($3);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}

event_legacy_tracepoint:
tracepoint_name opt_event_config
{
	struct parse_events_state *parse_state = _parse_state;
	struct parse_events_error *error = parse_state->error;
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;
	if (error)
		error->idx = @1.first_column;

	err = parse_events_add_tracepoint(list, &parse_state->idx, $1.sys, $1.event,
					error, $2, &@1);

	parse_events_terms__delete($2);
	free($1.sys);
	free($1.event);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}

tracepoint_name:
PE_NAME ':' PE_NAME
{
	struct tracepoint_name tracepoint = {$1, $3};

	$$ = tracepoint;
}

event_legacy_numeric:
PE_VALUE ':' PE_VALUE opt_event_config
{
	struct list_head *list;
	int err;

	list = alloc_list();
	if (!list)
		YYNOMEM;
	err = parse_events_add_numeric(_parse_state, list, (u32)$1, $3, $4,
				       /*wildcard=*/false);
	parse_events_terms__delete($4);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}

event_legacy_raw:
PE_RAW opt_event_config
{
	struct list_head *list;
	int err;
	u64 num;

	list = alloc_list();
	if (!list)
		YYNOMEM;
	errno = 0;
	num = strtoull($1 + 1, NULL, 16);
	/* Given the lexer will only give [a-fA-F0-9]+ a failure here should be impossible. */
	if (errno)
		YYABORT;
	free($1);
	err = parse_events_add_numeric(_parse_state, list, PERF_TYPE_RAW, num, $2,
				       /*wildcard=*/false);
	parse_events_terms__delete($2);
	if (err) {
		free(list);
		PE_ABORT(err);
	}
	$$ = list;
}

opt_event_config:
'/' event_config '/'
{
	$$ = $2;
}
|
'/' '/'
{
	$$ = NULL;
}
|
{
	$$ = NULL;
}

opt_pmu_config:
'/' event_config '/'
{
	$$ = $2;
}
|
'/' '/'
{
	$$ = NULL;
}

start_terms: event_config
{
	struct parse_events_state *parse_state = _parse_state;
	if (parse_state->terms) {
		parse_events_terms__delete ($1);
		YYABORT;
	}
	parse_state->terms = $1;
}

event_config:
event_config ',' event_term
{
	struct list_head *head = $1;
	struct parse_events_term *term = $3;

	if (!head) {
		parse_events_term__delete(term);
		YYABORT;
	}
	list_add_tail(&term->list, head);
	$$ = $1;
}
|
event_term
{
	struct list_head *head = malloc(sizeof(*head));
	struct parse_events_term *term = $1;

	if (!head)
		YYNOMEM;
	INIT_LIST_HEAD(head);
	list_add_tail(&term->list, head);
	$$ = head;
}

name_or_raw: PE_RAW | PE_NAME | PE_LEGACY_CACHE

name_or_legacy: PE_NAME | PE_LEGACY_CACHE

event_term:
PE_RAW
{
	struct parse_events_term *term;
	int err = parse_events_term__str(&term, PARSE_EVENTS__TERM_TYPE_RAW,
					 strdup("raw"), $1, &@1, &@1);

	if (err) {
		free($1);
		PE_ABORT(err);
	}
	$$ = term;
}
|
name_or_raw '=' name_or_legacy
{
	struct parse_events_term *term;
	int err = parse_events_term__str(&term, PARSE_EVENTS__TERM_TYPE_USER, $1, $3, &@1, &@3);

	if (err) {
		free($1);
		free($3);
		PE_ABORT(err);
	}
	$$ = term;
}
|
name_or_raw '=' PE_VALUE
{
	struct parse_events_term *term;
	int err = parse_events_term__num(&term, PARSE_EVENTS__TERM_TYPE_USER,
					 $1, $3, /*novalue=*/false, &@1, &@3);

	if (err) {
		free($1);
		PE_ABORT(err);
	}
	$$ = term;
}
|
name_or_raw '=' PE_TERM_HW
{
	struct parse_events_term *term;
	int err = parse_events_term__str(&term, PARSE_EVENTS__TERM_TYPE_USER,
					 $1, $3.str, &@1, &@3);

	if (err) {
		free($1);
		free($3.str);
		PE_ABORT(err);
	}
	$$ = term;
}
|
PE_LEGACY_CACHE
{
	struct parse_events_term *term;
	int err = parse_events_term__num(&term, PARSE_EVENTS__TERM_TYPE_LEGACY_CACHE,
					 $1, /*num=*/1, /*novalue=*/true, &@1, /*loc_val=*/NULL);

	if (err) {
		free($1);
		PE_ABORT(err);
	}
	$$ = term;
}
|
PE_NAME
{
	struct parse_events_term *term;
	int err = parse_events_term__num(&term, PARSE_EVENTS__TERM_TYPE_USER,
					 $1, /*num=*/1, /*novalue=*/true, &@1, /*loc_val=*/NULL);

	if (err) {
		free($1);
		PE_ABORT(err);
	}
	$$ = term;
}
|
PE_TERM_HW
{
	struct parse_events_term *term;
	int err = parse_events_term__num(&term, PARSE_EVENTS__TERM_TYPE_HARDWARE,
					 $1.str, $1.num & 255, /*novalue=*/false,
					 &@1, /*loc_val=*/NULL);

	if (err) {
		free($1.str);
		PE_ABORT(err);
	}
	$$ = term;
}
|
PE_TERM '=' name_or_legacy
{
	struct parse_events_term *term;
	int err = parse_events_term__str(&term, (enum parse_events__term_type)$1,
					/*config=*/NULL, $3, &@1, &@3);

	if (err) {
		free($3);
		PE_ABORT(err);
	}
	$$ = term;
}
|
PE_TERM '=' PE_TERM_HW
{
	struct parse_events_term *term;
	int err = parse_events_term__str(&term, (enum parse_events__term_type)$1,
					 /*config=*/NULL, $3.str, &@1, &@3);

	if (err) {
		free($3.str);
		PE_ABORT(err);
	}
	$$ = term;
}
|
PE_TERM '=' PE_TERM
{
	struct parse_events_term *term;
	int err = parse_events_term__term(&term,
					  (enum parse_events__term_type)$1,
					  (enum parse_events__term_type)$3,
					  &@1, &@3);

	if (err)
		PE_ABORT(err);

	$$ = term;
}
|
PE_TERM '=' PE_VALUE
{
	struct parse_events_term *term;
	int err = parse_events_term__num(&term, (enum parse_events__term_type)$1,
					 /*config=*/NULL, $3, /*novalue=*/false, &@1, &@3);

	if (err)
		PE_ABORT(err);

	$$ = term;
}
|
PE_TERM
{
	struct parse_events_term *term;
	int err = parse_events_term__num(&term, (enum parse_events__term_type)$1,
					/*config=*/NULL, /*num=*/1, /*novalue=*/true,
					&@1, /*loc_val=*/NULL);

	if (err)
		PE_ABORT(err);

	$$ = term;
}
|
PE_DRV_CFG_TERM
{
	struct parse_events_term *term;
	char *config = strdup($1);
	int err;

	if (!config)
		YYNOMEM;
	err = parse_events_term__str(&term, PARSE_EVENTS__TERM_TYPE_DRV_CFG, config, $1, &@1, NULL);
	if (err) {
		free($1);
		free(config);
		PE_ABORT(err);
	}
	$$ = term;
}

sep_dc: ':' |

sep_slash_slash_dc: '/' '/' | ':' |

%%

void parse_events_error(YYLTYPE *loc, void *parse_state,
			void *scanner __maybe_unused,
			char const *msg __maybe_unused)
{
	parse_events_evlist_error(parse_state, loc->last_column, "parser error");
}

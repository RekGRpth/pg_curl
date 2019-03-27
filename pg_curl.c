#include <postgres.h>
#include <fmgr.h>

#include <access/htup_details.h>
#include <access/htup.h>
#include <catalog/dependency.h>
#include <catalog/namespace.h>
#include <catalog/pg_type.h>
#include <commands/extension.h>
#include <curl/curl.h>
#include <funcapi.h>
#include <lib/stringinfo.h>
#include <limits.h>	/* INT_MAX */
#include <mb/pg_wchar.h>
#include <nodes/pg_list.h>
#include <regex.h>
#include <signal.h> /* SIGINT */
#include <stdlib.h>
#include <string.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
#include <utils/varlena.h>

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

CURL *curl = NULL;
pqsigfunc pgsql_interrupt_handler = NULL;
int pg_curl_interrupt_requested = 0;

static inline void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }

void _PG_init(void) {
    if (curl_global_init(CURL_GLOBAL_ALL)) ereport(ERROR, (errmsg("curl_global_init")));
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
    pg_curl_interrupt_requested = 0;
}

void _PG_fini(void) {
    (pqsigfunc)pqsignal(SIGINT, pgsql_interrupt_handler);
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    (void)curl_global_cleanup();
}

Datum pg_curl_easy_init(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_init); Datum pg_curl_easy_init(PG_FUNCTION_ARGS) {
    if (curl) (void)curl_easy_cleanup(curl);
    curl = curl_easy_init();
    PG_RETURN_BOOL(curl != NULL);
}

Datum pg_curl_easy_reset(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_reset); Datum pg_curl_easy_reset(PG_FUNCTION_ARGS) {
    if (curl) (void)curl_easy_reset(curl);
    PG_RETURN_VOID();
}

Datum pg_curl_easy_setopt(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt); Datum pg_curl_easy_setopt(PG_FUNCTION_ARGS) {
    CURLcode res;
    CURLoption option = CURLOPT_LASTENTRY;
    char *option_str;
    char *parameter_str = NULL;
    int len = sizeof("CURLOPT_") - 1;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("pg_curl_easy_setopt: PG_ARGISNULL(0)"), errhint("arg option must not null!")));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("pg_curl_easy_setopt: PG_ARGISNULL(1)"), errhint("arg parameter must not null!")));
    if (!curl) ereport(ERROR, (errmsg("pg_curl_easy_setopt: !curl"), errhint("call pg_curl_easy_init before!")));
    option_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (strlen(option_str) <= len) ereport(ERROR, (errmsg("pg_curl_easy_setopt: strlen"), errhint("arg option length must greater!")));
    if (pg_strncasecmp(option_str, "CURLOPT_", len)) ereport(ERROR, (errmsg("pg_curl_easy_setopt: pg_strncasecmp"), errhint("arg option must starts with CURLOPT_")));
    switch (option_str[len++]) {
        case 'u':
        case 'U': {
            switch (option_str[len++]) {
                case 'r':
                case 'R': {
                    switch (option_str[len++]) {
                        case 'l':
                        case 'L': {
                            option = CURLOPT_URL;
                            parameter_str = text_to_cstring(PG_GETARG_TEXT_P(1));
                        } break;
                    }
                } break;
            }
        } break;
    }
    if (option == CURLOPT_LASTENTRY) ereport(ERROR, (errmsg("pg_curl_easy_setopt: option == CURLOPT_LASTENTRY"), errhint("unsupported option %s", option_str)));
    if ((res = curl_easy_setopt(curl, option, parameter_str)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt: %s", curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_perform(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_perform); Datum pg_curl_easy_perform(PG_FUNCTION_ARGS) {
    CURLcode res;
    if (!curl) ereport(ERROR, (errmsg("pg_curl_easy_perform: !curl"), errhint("call pg_curl_easy_init before!")));
    if ((res = curl_easy_perform(curl)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_perform: %s", curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_cleanup); Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS) {
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    PG_RETURN_VOID();
}

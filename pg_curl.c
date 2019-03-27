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
StringInfoData data;
pqsigfunc pgsql_interrupt_handler = NULL;
int pg_curl_interrupt_requested = 0;

static inline void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }

void _PG_init(void) {
    if (curl_global_init(CURL_GLOBAL_ALL)) ereport(ERROR, (errmsg("curl_global_init")));
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
    pg_curl_interrupt_requested = 0;
    (void)initStringInfo(&data);
}

void _PG_fini(void) {
    (pqsigfunc)pqsignal(SIGINT, pgsql_interrupt_handler);
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    (void)curl_global_cleanup();
    if (data.data) (void)pfree(data.data);
}

Datum pg_curl_easy_init(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_init); Datum pg_curl_easy_init(PG_FUNCTION_ARGS) {
    if (curl) (void)curl_easy_cleanup(curl);
    curl = curl_easy_init();
    (void)resetStringInfo(&data);
    PG_RETURN_BOOL(curl != NULL);
}

Datum pg_curl_easy_reset(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_reset); Datum pg_curl_easy_reset(PG_FUNCTION_ARGS) {
    if (curl) (void)curl_easy_reset(curl);
    (void)resetStringInfo(&data);
    PG_RETURN_VOID();
}

Datum pg_curl_easy_setopt_str(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt_str); Datum pg_curl_easy_setopt_str(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLoption option = CURLOPT_LASTENTRY;
    char *option_str;
    char *parameter_str;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("pg_curl_easy_setopt_str: PG_ARGISNULL(0)"), errhint("arg option must not null!")));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("pg_curl_easy_setopt_str: PG_ARGISNULL(1)"), errhint("arg parameter must not null!")));
    if (!curl) {
        curl = curl_easy_init();
        (void)resetStringInfo(&data);
    }
    option_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (!pg_strncasecmp(option_str, "CURLOPT_URL", sizeof("CURLOPT_URL") - 1)) {
        option = CURLOPT_URL;
    }
    if (option == CURLOPT_LASTENTRY) ereport(ERROR, (errmsg("pg_curl_easy_setopt_str: option == CURLOPT_LASTENTRY"), errhint("unsupported option %s", option_str)));
    parameter_str = text_to_cstring(PG_GETARG_TEXT_P(1));
    if ((res = curl_easy_setopt(curl, option, parameter_str)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %s): %s", option_str, parameter_str, curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt_long); Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLoption option = CURLOPT_LASTENTRY;
    char *option_str;
    long parameter_long;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("pg_curl_easy_setopt_long: PG_ARGISNULL(0)"), errhint("arg option must not null!")));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("pg_curl_easy_setopt_long: PG_ARGISNULL(1)"), errhint("arg parameter must not null!")));
    if (!curl) {
        curl = curl_easy_init();
        (void)resetStringInfo(&data);
    }
    option_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (!pg_strncasecmp(option_str, "CURLOPT_URL", sizeof("CURLOPT_URL") - 1)) {
        option = CURLOPT_URL;
    }
    if (option == CURLOPT_LASTENTRY) ereport(ERROR, (errmsg("pg_curl_easy_setopt_long: option == CURLOPT_LASTENTRY"), errhint("unsupported option %s", option_str)));
    parameter_long = PG_GETARG_INT64(1);
    if ((res = curl_easy_setopt(curl, option, parameter_long)) != CURLE_OK) ereport(ERROR, (errmsg("pg_curl_easy_setopt_long(%s, %li): %s", option_str, parameter_long, curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

inline static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    (void)appendBinaryStringInfo((StringInfo)userp, (const char *)contents, (int)realsize);
    return realsize;
}

Datum pg_curl_easy_perform(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_perform); Datum pg_curl_easy_perform(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
//    if (!curl) ereport(ERROR, (errmsg("pg_curl_easy_perform: !curl"), errhint("call pg_curl_easy_init before!")));
    if (!curl) {
        curl = curl_easy_init();
        (void)resetStringInfo(&data);
    }
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_WRITEFUNCTION): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)(&data))) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_WRITEDATA): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_perform(curl)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_perform: %s", curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_getinfo_str(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_getinfo_str); Datum pg_curl_easy_getinfo_str(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
/*    CURLoption option = CURLOPT_LASTENTRY;
    CURLINFO info = CURLINFO_NONE;
    char *info_str;
    int len = sizeof("CURLINFO_") - 1;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str: PG_ARGISNULL(0)"), errhint("arg info must not null!")));
    if (!curl) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str: !curl"), errhint("call pg_curl_easy_init before!")));
    info_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (strlen(info_str) <= len) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str: strlen"), errhint("arg option length must greater!")));
    if (pg_strncasecmp(info_str, "CURLINFO_", len)) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str: pg_strncasecmp"), errhint("arg option must starts with CURLOPT_")));
    switch (info_str[len++]) {
        case 'c':
        case 'C': {
            switch (info_str[len++]) {
                case 'o':
                case 'O': {
                    switch (info_str[len++]) {
                        case 'l':
                        case 'L': {
                            info = CURLINFO_CONTENT_TYPE;
                        } break;
                    }
                } break;
            }
        } break;
    }
    if (option == CURLOPT_LASTENTRY) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str: option == CURLOPT_LASTENTRY"), errhint("unsupported option %s", option_str)));
    if (parameter_str) {
        if ((res = curl_easy_setopt(curl, option, parameter_str)) != CURLE_OK) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str(%s, %s): %s", option_str, parameter_str, curl_easy_strerror(res))));
    } else {
        if ((res = curl_easy_setopt(curl, option, parameter_long)) != CURLE_OK) ereport(ERROR, (errmsg("pg_curl_easy_getinfo_str(%s, %li): %s", option_str, parameter_long, curl_easy_strerror(res))));
    }*/
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_cleanup); Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS) {
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    PG_RETURN_VOID();
}

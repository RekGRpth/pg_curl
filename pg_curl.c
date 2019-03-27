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
StringInfoData read;
StringInfoData write;
pqsigfunc pgsql_interrupt_handler = NULL;
int pg_curl_interrupt_requested = 0;

static inline void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }

void _PG_init(void) {
    if (curl_global_init(CURL_GLOBAL_ALL)) ereport(ERROR, (errmsg("curl_global_init")));
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
    pg_curl_interrupt_requested = 0;
    (void)initStringInfo(&read);
    (void)initStringInfo(&write);
}

void _PG_fini(void) {
    (pqsigfunc)pqsignal(SIGINT, pgsql_interrupt_handler);
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    (void)curl_global_cleanup();
    if (read.data) (void)pfree(read.data);
    if (write.data) (void)pfree(write.data);
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

inline static size_t read_callback(void *buffer, size_t size, size_t nitems, void *instream) {
    size_t reqsize = size * nitems;
    StringInfo si = (StringInfo)instream;
    size_t remaining = si->len - si->cursor;
    size_t readsize = reqsize < remaining ? reqsize : remaining;
    memcpy(buffer, si->data + si->cursor, readsize);
    si->cursor += readsize;
    return readsize;
}

Datum pg_curl_easy_setopt_str(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt_str); Datum pg_curl_easy_setopt_str(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *option_str;
    char *parameter_str;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument option must not null!")));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument parameter must not null!")));
    if (!curl) curl = curl_easy_init();
    option_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (!pg_strncasecmp(option_str, "CURLOPT_READDATA", sizeof("CURLOPT_READDATA") - 1)) {
/*			appendBinaryStringInfo(&si_read, VARDATA(content_text), content_size);
			CURL_SETOPT(g_http_handle, CURLOPT_UPLOAD, 1);
			CURL_SETOPT(g_http_handle, CURLOPT_READFUNCTION, http_readback);
			CURL_SETOPT(g_http_handle, CURLOPT_READDATA, &si_read);
			CURL_SETOPT(g_http_handle, CURLOPT_INFILESIZE, content_size);*/
    }
    else if (!pg_strncasecmp(option_str, "CURLOPT_URL", sizeof("CURLOPT_URL") - 1)) option = CURLOPT_URL;
    else if (!pg_strncasecmp(option_str, "CURLOPT_USERAGENT", sizeof("CURLOPT_USERAGENT") - 1)) option = CURLOPT_USERAGENT;
    else if (!pg_strncasecmp(option_str, "CURLOPT_ACCEPT_ENCODING", sizeof("CURLOPT_ACCEPT_ENCODING") - 1)) option = CURLOPT_ACCEPT_ENCODING;
    else if (!pg_strncasecmp(option_str, "CURLOPT_CUSTOMREQUEST", sizeof("CURLOPT_CUSTOMREQUEST") - 1)) option = CURLOPT_CUSTOMREQUEST;
    else ereport(ERROR, (errmsg("unsupported option %s", option_str)));
    parameter_str = text_to_cstring(PG_GETARG_TEXT_P(1));
    if ((res = curl_easy_setopt(curl, option, parameter_str)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %s): %s", option_str, parameter_str, curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt_long); Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *option_str;
    long parameter_long;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument option must not null!")));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument parameter must not null!")));
    if (!curl) curl = curl_easy_init();
    option_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (!pg_strncasecmp(option_str, "CURLOPT_CONNECTTIMEOUT", sizeof("CURLOPT_CONNECTTIMEOUT") - 1)) option = CURLOPT_CONNECTTIMEOUT;
    else if (!pg_strncasecmp(option_str, "CURLOPT_TIMEOUT_MS", sizeof("CURLOPT_TIMEOUT_MS") - 1)) option = CURLOPT_TIMEOUT_MS;
    else if (!pg_strncasecmp(option_str, "CURLOPT_FORBID_REUSE", sizeof("CURLOPT_FORBID_REUSE") - 1)) option = CURLOPT_FORBID_REUSE;
    else if (!pg_strncasecmp(option_str, "CURLOPT_FOLLOWLOCATION", sizeof("CURLOPT_FOLLOWLOCATION") - 1)) option = CURLOPT_FOLLOWLOCATION;
    else if (!pg_strncasecmp(option_str, "CURLOPT_MAXREDIRS", sizeof("CURLOPT_MAXREDIRS") - 1)) option = CURLOPT_MAXREDIRS;
    else if (!pg_strncasecmp(option_str, "CURLOPT_POST", sizeof("CURLOPT_POST") - 1)) option = CURLOPT_POST;
    else if (!pg_strncasecmp(option_str, "CURLOPT_UPLOAD", sizeof("CURLOPT_UPLOAD") - 1)) option = CURLOPT_UPLOAD;
    else if (!pg_strncasecmp(option_str, "CURLOPT_INFILESIZE", sizeof("CURLOPT_INFILESIZE") - 1)) option = CURLOPT_INFILESIZE;
    else if (!pg_strncasecmp(option_str, "CURLOPT_NOBODY", sizeof("CURLOPT_NOBODY") - 1)) option = CURLOPT_NOBODY;
    else ereport(ERROR, (errmsg("unsupported option %s", option_str)));
    parameter_long = PG_GETARG_INT64(1);
    if ((res = curl_easy_setopt(curl, option, parameter_long)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %li): %s", option_str, parameter_long, curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

inline static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    (void)appendBinaryStringInfo((StringInfo)userp, (const char *)contents, (int)realsize);
    return realsize;
}

inline static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_interrupt_requested; }

Datum pg_curl_easy_perform(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_perform); Datum pg_curl_easy_perform(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    if (!curl) curl = curl_easy_init();
    (void)resetStringInfo(&write);
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_WRITEFUNCTION): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)(&write))) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_WRITEDATA): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_XFERINFOFUNCTION): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_NOPROGRESS): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_PROTOCOLS): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_perform(curl)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_perform: %s", curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_getinfo_str(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_getinfo_str); Datum pg_curl_easy_getinfo_str(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    char *info_str;
    char *str = NULL;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("argument info must not null!")));
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    info_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (!pg_strncasecmp(info_str, "CURLINFO_RESPONSE", sizeof("CURLINFO_RESPONSE") - 1)) { str = write.data; goto ret; }
    else if (!pg_strncasecmp(info_str, "CURLINFO_CONTENT_TYPE", sizeof("CURLINFO_CONTENT_TYPE") - 1)) info = CURLINFO_CONTENT_TYPE;
    else ereport(ERROR, (errmsg("unsupported option %s", info_str)));
    if ((res = curl_easy_getinfo(curl, info, &str)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_getinfo(%s): %s", info_str, curl_easy_strerror(res))));
    ret: if (!str) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(str));
}

Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_getinfo_long); Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    char *info_str;
    long lon;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("argument info must not null!")));
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    info_str = text_to_cstring(PG_GETARG_TEXT_P(0));
    if (!pg_strncasecmp(info_str, "CURLINFO_RESPONSE_CODE", sizeof("CURLINFO_RESPONSE_CODE") - 1)) info = CURLINFO_RESPONSE_CODE;
    else ereport(ERROR, (errmsg("unsupported option %s", info_str)));
    if ((res = curl_easy_getinfo(curl, info, &lon)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_getinfo(%s): %s", info_str, curl_easy_strerror(res))));
    PG_RETURN_INT64(lon);
}

Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_cleanup); Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS) {
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    PG_RETURN_VOID();
}

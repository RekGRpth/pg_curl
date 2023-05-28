#include <postgres.h>

#include <catalog/pg_type.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#if PG_VERSION_NUM < 90300
#include <libpq/pqsignal.h>
#endif
#include <signal.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/memutils.h>
#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif

#include <curl/curl.h>
#include <pthread.h>

#define EXTENSION(function) Datum (function)(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(function); Datum (function)(PG_FUNCTION_ARGS)

PG_MODULE_MAGIC;

typedef struct {
    CURL *curl;
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    curl_mime *mime;
#endif
#if PG_VERSION_NUM >= 90500
    MemoryContextCallback callback;
#endif
    StringInfoData data_in;
    StringInfoData data_out;
    StringInfoData debug;
    StringInfoData header_in;
    StringInfoData header_out;
    StringInfoData postfield;
    StringInfoData url;
    struct curl_slist *header;
    struct curl_slist *postquote;
    struct curl_slist *prequote;
    struct curl_slist *quote;
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    struct curl_slist *recipient;
#endif
} pg_curl_t;

typedef struct {
    MemoryContext context;
#if PG_VERSION_NUM >= 90500
    MemoryContextCallback callback;
#endif
    struct {
        int requested;
        pqsigfunc handler;
    } interrupt;
} pg_curl_global_t;

static bool pg_curl_transaction = true;
static pg_curl_global_t pg_curl_global = {0};
static pg_curl_t *pg_curl = NULL;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void pg_curl_interrupt_handler(int sig) { pg_curl_global.interrupt.requested = sig; }

#if CURL_AT_LEAST_VERSION(7, 12, 0)
static void *pg_curl_malloc_callback(size_t size) {
    void *res;
    pthread_mutex_lock(&mutex);
    PG_TRY(); {
        res = size ? MemoryContextAlloc(pg_curl_global.context, size) : NULL;
    } PG_CATCH(); {
        pthread_mutex_unlock(&mutex);
        PG_RE_THROW();
    } PG_END_TRY();
    pthread_mutex_unlock(&mutex);
    return res;
}

static void pg_curl_free_callback(void *ptr) {
    pthread_mutex_lock(&mutex);
    PG_TRY(); {
        if (ptr) pfree(ptr);
    } PG_CATCH(); {
        pthread_mutex_unlock(&mutex);
        PG_RE_THROW();
    } PG_END_TRY();
    pthread_mutex_unlock(&mutex);
}

static void *pg_curl_realloc_callback(void *ptr, size_t size) {
    void *res;
    pthread_mutex_lock(&mutex);
    PG_TRY(); {
        res = (ptr && size) ? repalloc(ptr, size) : (size ? MemoryContextAlloc(pg_curl_global.context, size) : ptr);
    } PG_CATCH(); {
        pthread_mutex_unlock(&mutex);
        PG_RE_THROW();
    } PG_END_TRY();
    pthread_mutex_unlock(&mutex);
    return res;
}

static char *pg_curl_strdup_callback(const char *str) {
    char *res;
    pthread_mutex_lock(&mutex);
    PG_TRY(); {
        res = MemoryContextStrdup(pg_curl_global.context, str);
    } PG_CATCH(); {
        pthread_mutex_unlock(&mutex);
        PG_RE_THROW();
    } PG_END_TRY();
    pthread_mutex_unlock(&mutex);
    return res;
}

static void *pg_curl_calloc_callback(size_t nmemb, size_t size) {
    void *res;
    pthread_mutex_lock(&mutex);
    PG_TRY(); {
        res = MemoryContextAllocZero(pg_curl_global.context, nmemb * size);
    } PG_CATCH(); {
        pthread_mutex_unlock(&mutex);
        PG_RE_THROW();
    } PG_END_TRY();
    pthread_mutex_unlock(&mutex);
    return res;
}
#endif

static void pg_curl_global_cleanup(void) {
    if (!pg_curl_global.context) return;
    pqsignal(SIGINT, pg_curl_global.interrupt.handler);
#if CURL_AT_LEAST_VERSION(7, 8, 0)
    curl_global_cleanup();
#endif
    pg_curl_global.context = NULL;
}

static void pg_curl_easy_cleanup(void *arg) {
    if (!pg_curl) return;
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    curl_mime_free(pg_curl->mime);
#endif
    curl_slist_free_all(pg_curl->header);
    curl_slist_free_all(pg_curl->postquote);
    curl_slist_free_all(pg_curl->prequote);
    curl_slist_free_all(pg_curl->quote);
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    curl_slist_free_all(pg_curl->recipient);
#endif
    curl_easy_cleanup(pg_curl->curl);
    pfree(pg_curl);
    pg_curl = NULL;
    pg_curl_global_cleanup();
}

static void pg_curl_global_init(void) {
    MemoryContext oldMemoryContext;
    if (pg_curl_global.context) return;
#if PG_VERSION_NUM >= 90500
    pg_curl_global.context = pg_curl_transaction ? TopTransactionContext : TopMemoryContext;
#else
    pg_curl_global.context = TopMemoryContext;
#endif
    oldMemoryContext = MemoryContextSwitchTo(pg_curl_global.context);
#if CURL_AT_LEAST_VERSION(7, 12, 0)
    if (curl_global_init_mem(CURL_GLOBAL_ALL, pg_curl_malloc_callback, pg_curl_free_callback, pg_curl_realloc_callback, pg_curl_strdup_callback, pg_curl_calloc_callback)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_global_init_mem")));
#elif CURL_AT_LEAST_VERSION(7, 8, 0)
    if (curl_global_init(CURL_GLOBAL_ALL)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_global_init")));
#endif
    pg_curl_global.interrupt.requested = 0;
    pg_curl_global.interrupt.handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
    MemoryContextSwitchTo(oldMemoryContext);
}

static void pg_curl_easy_init(void) {
    MemoryContext oldMemoryContext;
    if (pg_curl) return;
    pg_curl_global_init();
    oldMemoryContext = MemoryContextSwitchTo(pg_curl_global.context);
    pg_curl = MemoryContextAllocZero(pg_curl_global.context, sizeof(*pg_curl));
    if (!(pg_curl->curl = curl_easy_init())) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_easy_init")));
    initStringInfo(&pg_curl->data_in);
    initStringInfo(&pg_curl->data_out);
    initStringInfo(&pg_curl->debug);
    initStringInfo(&pg_curl->header_in);
    initStringInfo(&pg_curl->header_out);
    initStringInfo(&pg_curl->postfield);
    initStringInfo(&pg_curl->url);
#if PG_VERSION_NUM >= 90500
    pg_curl->callback.arg = pg_curl;
    pg_curl->callback.func = pg_curl_easy_cleanup;
    MemoryContextRegisterResetCallback(pg_curl_global.context, &pg_curl->callback);
#endif
    MemoryContextSwitchTo(oldMemoryContext);
}

EXTENSION(pg_curl_easy_header_reset) {
    pg_curl_easy_init();
    curl_slist_free_all(pg_curl->header);
    pg_curl->header = NULL;
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_mime_reset) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    pg_curl_easy_init();
    curl_mime_free(pg_curl->mime);
    pg_curl->mime = NULL;
    PG_RETURN_VOID();
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_mime_reset requires curl 7.56.0 or later")));
#endif
}

EXTENSION(pg_curl_easy_postquote_reset) {
    pg_curl_easy_init();
    curl_slist_free_all(pg_curl->postquote);
    pg_curl->postquote = NULL;
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_prequote_reset) {
    pg_curl_easy_init();
    curl_slist_free_all(pg_curl->prequote);
    pg_curl->prequote = NULL;
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_quote_reset) {
    pg_curl_easy_init();
    curl_slist_free_all(pg_curl->quote);
    pg_curl->quote = NULL;
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_recipient_reset) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    pg_curl_easy_init();
    curl_slist_free_all(pg_curl->recipient);
    pg_curl->recipient = NULL;
    PG_RETURN_VOID();
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_recipient_reset requires curl 7.20.0 or later")));
#endif
}

EXTENSION(pg_curl_easy_reset) {
    pg_curl_easy_init();
    pg_curl_easy_header_reset(fcinfo);
    pg_curl_easy_postquote_reset(fcinfo);
    pg_curl_easy_prequote_reset(fcinfo);
    pg_curl_easy_quote_reset(fcinfo);
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    pg_curl_easy_mime_reset(fcinfo);
#endif
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    pg_curl_easy_recipient_reset(fcinfo);
#endif
#if CURL_AT_LEAST_VERSION(7, 12, 1)
    curl_easy_reset(pg_curl->curl);
#endif
    resetStringInfo(&pg_curl->data_in);
    resetStringInfo(&pg_curl->data_out);
    resetStringInfo(&pg_curl->debug);
    resetStringInfo(&pg_curl->header_in);
    resetStringInfo(&pg_curl->header_out);
    resetStringInfo(&pg_curl->postfield);
    resetStringInfo(&pg_curl->url);
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_escape) {
#if CURL_AT_LEAST_VERSION(7, 15, 4)
    text *string;
    char *escape;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_escape requires argument string")));
    string = PG_GETARG_TEXT_PP(0);
    if (!(escape = curl_easy_escape(pg_curl->curl, VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string)))) PG_RETURN_NULL();
    PG_FREE_IF_COPY(string, 0);
    string = cstring_to_text(escape);
    curl_free(escape);
    PG_RETURN_TEXT_P(string);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_escape requires curl 7.15.4 or later")));
#endif
}

EXTENSION(pg_curl_easy_unescape) {
#if CURL_AT_LEAST_VERSION(7, 15, 4)
    text *url;
    char *unescape;
    int outlength;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_unescape requires argument url")));
    url = PG_GETARG_TEXT_PP(0);
    if (!(unescape = curl_easy_unescape(pg_curl->curl, VARDATA_ANY(url), VARSIZE_ANY_EXHDR(url), &outlength))) PG_RETURN_NULL();
    PG_FREE_IF_COPY(url, 0);
    url = cstring_to_text_with_len(unescape, outlength);
    curl_free(unescape);
    PG_RETURN_TEXT_P(url);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_unescape requires curl 7.15.4 or later")));
#endif
}

EXTENSION(pg_curl_header_append) {
    char *name, *value;
    StringInfoData buf;
    struct curl_slist *temp = pg_curl->header;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_header_append requires argument name")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_header_append requires argument value")));
    value = TextDatumGetCString(PG_GETARG_DATUM(1));
    initStringInfo(&buf);
    appendStringInfo(&buf, "%s: %s", name, value);
    if ((temp = curl_slist_append(temp, buf.data))) pg_curl->header = temp; else ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_slist_append")));
    pfree(name);
    pfree(value);
    pfree(buf.data);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_postquote_append) {
    char *command;
    struct curl_slist *temp = pg_curl->postquote;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_postquote_append requires argument command")));
    command = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, command))) pg_curl->postquote = temp; else ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_slist_append")));
    pfree(command);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_prequote_append) {
    char *command;
    struct curl_slist *temp = pg_curl->prequote;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_prequote_append requires argument command")));
    command = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, command))) pg_curl->prequote = temp; else ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_slist_append")));
    pfree(command);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_quote_append) {
    char *command;
    struct curl_slist *temp = pg_curl->quote;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_quote_append requires argument command")));
    command = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, command))) pg_curl->quote = temp; else ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_slist_append")));
    pfree(command);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_recipient_append) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    char *email;
    struct curl_slist *temp = pg_curl->recipient;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_recipient_append requires argument email")));
    email = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, email))) pg_curl->recipient = temp; else ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_slist_append")));
    pfree(email);
    PG_RETURN_BOOL(temp != NULL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_recipient_append requires curl 7.20.0 or later")));
#endif
}

static Datum pg_curl_mime_data_or_file(PG_FUNCTION_ARGS, curl_mimepart *part) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    char *name = NULL, *file = NULL, *type = NULL, *code = NULL, *head = NULL;
    CURLcode res = CURL_LAST;
    pg_curl_easy_init();
    if (!PG_ARGISNULL(1)) name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) file = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) type = TextDatumGetCString(PG_GETARG_DATUM(3));
    if (!PG_ARGISNULL(4)) code = TextDatumGetCString(PG_GETARG_DATUM(4));
    if (!PG_ARGISNULL(5)) head = TextDatumGetCString(PG_GETARG_DATUM(5));
    if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_name failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%s", name)));
    if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_filename failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%s", file)));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_type failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%s", type)));
    if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_encoder failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%s", code)));
    if (head) {
        struct curl_slist *headers = NULL;
        if (!(headers = curl_slist_append(headers, head))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_slist_append")));
        if ((res = curl_mime_headers(part, headers, true)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_headers failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%s", head)));
    }
    if (name) pfree(name);
    if (file) pfree(file);
    if (type) pfree(type);
    if (code) pfree(code);
    if (head) pfree(head);
    PG_RETURN_BOOL(res == CURLE_OK);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_mime_data requires curl 7.56.0 or later")));
#endif
}

EXTENSION(pg_curl_mime_data_text) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    CURLcode res = CURL_LAST;
    curl_mimepart *part;
    pg_curl_easy_init();
    if (!pg_curl->mime && !(pg_curl->mime = curl_mime_init(pg_curl->curl))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_mime_init")));
    if (!(part = curl_mime_addpart(pg_curl->mime))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_mime_addpart")));
    if (!PG_ARGISNULL(0)) {
        text *data = PG_GETARG_TEXT_PP(0);
        if ((res = curl_mime_data(part, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data))) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_data failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%*.*s", (int)VARSIZE_ANY_EXHDR(data), (int)VARSIZE_ANY_EXHDR(data), VARDATA_ANY(data))));
        PG_FREE_IF_COPY(data, 0);
    }
    return pg_curl_mime_data_or_file(fcinfo, part);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_mime_data requires curl 7.56.0 or later")));
#endif
}

EXTENSION(pg_curl_mime_data_bytea) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    CURLcode res = CURL_LAST;
    curl_mimepart *part;
    pg_curl_easy_init();
    if (!pg_curl->mime && !(pg_curl->mime = curl_mime_init(pg_curl->curl))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_mime_init")));
    if (!(part = curl_mime_addpart(pg_curl->mime))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_mime_addpart")));
    if (!PG_ARGISNULL(0)) {
        bytea *data = PG_GETARG_BYTEA_PP(0);
        if ((res = curl_mime_data(part, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data))) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_data failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%*.*s", (int)VARSIZE_ANY_EXHDR(data), (int)VARSIZE_ANY_EXHDR(data), VARDATA_ANY(data))));
        PG_FREE_IF_COPY(data, 0);
    }
    return pg_curl_mime_data_or_file(fcinfo, part);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_mime_data requires curl 7.56.0 or later")));
#endif
}

EXTENSION(pg_curl_mime_file) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    CURLcode res = CURL_LAST;
    curl_mimepart *part;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_mime_file requires argument data")));
    if (!pg_curl->mime && !(pg_curl->mime = curl_mime_init(pg_curl->curl))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_mime_init")));
    if (!(part = curl_mime_addpart(pg_curl->mime))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("!curl_mime_addpart")));
    if (!PG_ARGISNULL(0)) {
        char *data = TextDatumGetCString(PG_GETARG_DATUM(0));
        if ((res = curl_mime_filedata(part, data)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_mime_name failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%s", data)));
        pfree(data);
    }
    return pg_curl_mime_data_or_file(fcinfo, part);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_mime_file requires curl 7.56.0 or later")));
#endif
}

EXTENSION(pg_curl_easy_setopt_postfields) {
    CURLcode res = CURLE_OK;
    bytea *parameter;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_setopt_postfields requires argument parameter")));
    parameter = PG_GETARG_BYTEA_PP(0);
    resetStringInfo(&pg_curl->postfield);
    appendBinaryStringInfo(&pg_curl->postfield, VARDATA_ANY(parameter), VARSIZE_ANY_EXHDR(parameter));
    PG_FREE_IF_COPY(parameter, 0);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_url) {
    CURLcode res = CURLE_OK;
    text *parameter;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_setopt_url requires argument parameter")));
    parameter = PG_GETARG_TEXT_PP(0);
    resetStringInfo(&pg_curl->url);
    appendBinaryStringInfo(&pg_curl->url, VARDATA_ANY(parameter), VARSIZE_ANY_EXHDR(parameter));
    PG_FREE_IF_COPY(parameter, 0);
    PG_RETURN_BOOL(res == CURLE_OK);
}

static Datum pg_curl_postfield_or_url_append(PG_FUNCTION_ARGS, StringInfoData *buf) {
    char *escape;
    CURLcode res = CURLE_OK;
    text *name = PG_GETARG_TEXT_PP(0);
    pg_curl_easy_init();
    if (buf->len && buf->data[buf->len - 1] != '?') appendStringInfoChar(buf, '&');
    if (!(escape = curl_easy_escape(pg_curl->curl, VARDATA_ANY(name), VARSIZE_ANY_EXHDR(name)))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_escape failed")));
    appendStringInfoString(buf, escape);
    PG_FREE_IF_COPY(name, 0);
    if (!PG_ARGISNULL(1)) {
        text *value = PG_GETARG_TEXT_PP(1);
        appendStringInfoChar(buf, '=');
        if (VARSIZE_ANY_EXHDR(value)) {
            if (!(escape = curl_easy_escape(pg_curl->curl, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value)))) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_escape failed")));
            appendStringInfoString(buf, escape);
        }
        PG_FREE_IF_COPY(value, 1);
    }
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_postfield_append) {
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("pg_curl_postfield_append requires argument name")));
    return pg_curl_postfield_or_url_append(fcinfo, &pg_curl->postfield);
}

EXTENSION(pg_curl_url_append) {
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("pg_curl_url_append requires argument name")));
    return pg_curl_postfield_or_url_append(fcinfo, &pg_curl->url);
}

#if CURL_AT_LEAST_VERSION(7, 71, 0)
static Datum pg_curl_easy_setopt_blob(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURLE_OK;
    bytea *parameter;
    struct curl_blob blob;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_setopt_* requires argument parameter")));
    parameter = PG_GETARG_BYTEA_PP(0);
    blob.data = VARDATA_ANY(parameter);
    blob.flags = CURL_BLOB_COPY;
    blob.len = VARSIZE_ANY_EXHDR(parameter);
    if ((res = curl_easy_setopt(pg_curl->curl, option, &blob)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%i", option)));
    PG_FREE_IF_COPY(parameter, 0);
    PG_RETURN_BOOL(res == CURLE_OK);
}
#endif

static Datum pg_curl_easy_setopt_char(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURL_LAST;
    char *parameter;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_setopt_* requires argument parameter")));
    parameter = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((res = curl_easy_setopt(pg_curl->curl, option, parameter)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%i and %s", option, parameter)));
    pfree(parameter);
    PG_RETURN_BOOL(res == CURLE_OK);
}

static int pg_debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    pg_curl_t *curl = userptr;
    if (size) switch (type) {
        case CURLINFO_DATA_OUT: appendBinaryStringInfo(&curl->data_out, data, size); break;
        case CURLINFO_HEADER_OUT: appendBinaryStringInfo(&curl->header_out, data, size); break;
        case CURLINFO_TEXT: appendBinaryStringInfo(&curl->debug, data, size); break;
        default: break;
    }
    return 0;
}

EXTENSION(pg_curl_easy_setopt_abstract_unix_socket) {
#if CURL_AT_LEAST_VERSION(7, 53, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ABSTRACT_UNIX_SOCKET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_abstract_unix_socket requires curl 7.53.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_accept_encoding) {
#if CURL_AT_LEAST_VERSION(7, 21, 6)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ACCEPT_ENCODING);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_accept_encoding requires curl 7.21.6 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_cainfo_blob) {
#if CURL_AT_LEAST_VERSION(7, 77, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_CAINFO_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_cainfo_blob requires curl 7.77.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_cainfo) {
#if CURL_AT_LEAST_VERSION(7, 60, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CAINFO);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_cainfo requires curl 7.60.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_capath) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CAPATH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_capath requires curl 7.56.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_cookiefile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIEFILE); }
EXTENSION(pg_curl_easy_setopt_cookiejar) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIEJAR); }
EXTENSION(pg_curl_easy_setopt_cookielist) {
#if CURL_AT_LEAST_VERSION(7, 14, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIELIST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_cookielist requires curl 7.14.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_cookie) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIE); }
EXTENSION(pg_curl_easy_setopt_crlfile) {
#if CURL_AT_LEAST_VERSION(7, 19, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CRLFILE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_crlfile requires curl 7.19.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_customrequest) {
#if CURL_AT_LEAST_VERSION(7, 26, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CUSTOMREQUEST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_customrequest requires curl 7.26.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_default_protocol) {
#if CURL_AT_LEAST_VERSION(7, 45, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DEFAULT_PROTOCOL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_default_protocol requires curl 7.45.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_interface) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_INTERFACE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_dns_interface requires curl 7.33.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_local_ip4) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_LOCAL_IP4);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_dns_local_ip4 requires curl 7.33.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_local_ip6) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_LOCAL_IP6);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_dns_local_ip6 requires curl 7.33.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_servers) {
#if CURL_AT_LEAST_VERSION(7, 24, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_SERVERS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("url_easy_setopt_dns_servers requires curl 7.24.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_doh_url) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DOH_URL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_doh_url requires curl 7.62.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_egdsocket) {
#if CURL_AT_LEAST_VERSION(7, 84, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_egdsocket deprecated: since 7.84.0. Serves no purpose anymore")));
#else
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_EGDSOCKET);
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_account) {
#if CURL_AT_LEAST_VERSION(7, 13, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTP_ACCOUNT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_account requires curl 7.13.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_alternative_to_user) {
#if CURL_AT_LEAST_VERSION(7, 15, 5)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTP_ALTERNATIVE_TO_USER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_alternative_to_user requires curl 7.15.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftpport) {
#if CURL_AT_LEAST_VERSION(7, 19, 5)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTPPORT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftpport requires curl 7.19.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_interface) {
#if CURL_AT_LEAST_VERSION(7, 24, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_INTERFACE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_interface requires curl 7.24.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_issuercert_blob) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_ISSUERCERT_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_issuercert_blob requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_issuercert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ISSUERCERT); }
EXTENSION(pg_curl_easy_setopt_keypasswd) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_KEYPASSWD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_keypasswd requires curl 7.16.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_krblevel) {
#if CURL_AT_LEAST_VERSION(7, 16, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_KRBLEVEL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_krblevel requires curl 7.16.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_login_options) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_LOGIN_OPTIONS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_login_options requires curl 7.34.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_mail_auth) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_MAIL_AUTH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_mail_auth requires curl 7.25.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_mail_from) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_MAIL_FROM);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_mail_from requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_noproxy) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_NOPROXY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_noproxy requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_password) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PASSWORD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_password requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_pinnedpublickey) {
#if CURL_AT_LEAST_VERSION(7, 39, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PINNEDPUBLICKEY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_pinnedpublickey requires curl 7.39.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_pre_proxy) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PRE_PROXY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_pre_proxy requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_protocols_str) {
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROTOCOLS_STR);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_protocols_str requires curl 7.85.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_cainfo_blob) {
#if CURL_AT_LEAST_VERSION(7, 77, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_PROXY_CAINFO_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_cainfo_blob requires curl 7.77.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_cainfo) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CAINFO);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_cainfo requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_capath) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CAPATH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_capath requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_crlfile) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CRLFILE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_crlfile requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_issuercert_blob) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_PROXY_ISSUERCERT_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_issuercert_blob requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_issuercert) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_PROXY_ISSUERCERT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_issuercert requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_keypasswd) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_KEYPASSWD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_keypasswd requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxypassword) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYPASSWORD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxypassword requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_pinnedpublickey) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_PINNEDPUBLICKEY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_pinnedpublickey requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_service_name) {
#if CURL_AT_LEAST_VERSION(7, 43, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SERVICE_NAME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_service_name requires curl 7.43.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy) {
#if CURL_AT_LEAST_VERSION(7, 14, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy requires curl 7.14.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslcert_blob) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_PROXY_SSLCERT_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslcert_blob requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslcert) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLCERT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslcert requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslcerttype) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLCERTTYPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslcerttype requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_ssl_cipher_list) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSL_CIPHER_LIST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_ssl_cipher_list requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslkey_blob) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_PROXY_SSLKEY_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslkey_blob requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslkey) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLKEY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslkey requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslkeytype) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLKEYTYPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslkeytype requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tls13_ciphers) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLS13_CIPHERS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_tls13_ciphers requires curl 7.61.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_password) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_PASSWORD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_tlsauth_password requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_type) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_TYPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_tlsauth_type requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_username) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_USERNAME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_tlsauth_username requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyusername) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYUSERNAME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxyusername requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyuserpwd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYUSERPWD); }
EXTENSION(pg_curl_easy_setopt_random_file) {
#if CURL_AT_LEAST_VERSION(7, 84, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_random_file deprecated: since 7.84.0. Serves no purpose anymore")));
#else
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RANDOM_FILE);
#endif
}
EXTENSION(pg_curl_easy_setopt_range) {
#if CURL_AT_LEAST_VERSION(7, 18, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RANGE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_range requires curl 7.18.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_redir_protocols_str) {
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REDIR_PROTOCOLS_STR);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_redir_protocols_str requires curl 7.85.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_referer) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REFERER); }
EXTENSION(pg_curl_easy_setopt_request_target) {
#if CURL_AT_LEAST_VERSION(7, 55, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REQUEST_TARGET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_request_target requires curl 7.55.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_session_id) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_SESSION_ID);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_rtsp_session_id requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_stream_uri) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_STREAM_URI);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_rtsp_stream_uri requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_transport) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_TRANSPORT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_rtsp_transport requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_service_name) {
#if CURL_AT_LEAST_VERSION(7, 43, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SERVICE_NAME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_service_name requires curl 7.43.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_socks5_gssapi_service) {
#if CURL_AT_LEAST_VERSION(7, 49, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_socks5_gssapi_service deprecated: since 7.49.0. Use curl_easy_setopt_proxy_service_name")));
#elif CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SOCKS5_GSSAPI_SERVICE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_socks5_gssapi_service requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_host_public_key_md5) {
#if CURL_AT_LEAST_VERSION(7, 17, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_HOST_PUBLIC_KEY_MD5);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssh_host_public_key_md5 requires curl 7.17.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_knownhosts) {
#if CURL_AT_LEAST_VERSION(7, 19, 6)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_KNOWNHOSTS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssh_knownhosts requires curl 7.19.6 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_private_keyfile) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_PRIVATE_KEYFILE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssh_private_keyfile requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_public_keyfile) {
#if CURL_AT_LEAST_VERSION(7, 26, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_PUBLIC_KEYFILE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssh_public_keyfile requires curl 7.26.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_sslcert_blob) {
#if CURL_AT_LEAST_VERSION(7, 71, 10)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_SSLCERT_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_sslcert_blob requires curl 7.71.10 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_sslcert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLCERT); }
EXTENSION(pg_curl_easy_setopt_sslcerttype) {
#if CURL_AT_LEAST_VERSION(7, 9, 3)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLCERTTYPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_sslcerttype requires curl 7.9.3 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_cipher_list) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSL_CIPHER_LIST); }
EXTENSION(pg_curl_easy_setopt_sslengine) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLENGINE); }
EXTENSION(pg_curl_easy_setopt_sslkey_blob) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    return pg_curl_easy_setopt_blob(fcinfo, CURLOPT_SSLKEY_BLOB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_sslkey_blob requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_sslkey) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLKEY); }
EXTENSION(pg_curl_easy_setopt_sslkeytype) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLKEYTYPE); }
EXTENSION(pg_curl_easy_setopt_tls13_ciphers) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLS13_CIPHERS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tls13_ciphers requires curl 7.61.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tlsauth_password) {
#if CURL_AT_LEAST_VERSION(7, 21, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_PASSWORD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tlsauth_password requires curl 7.21.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tlsauth_type) {
#if CURL_AT_LEAST_VERSION(7, 21, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_TYPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tlsauth_type requires curl 7.21.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tlsauth_username) {
#if CURL_AT_LEAST_VERSION(7, 21, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_USERNAME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tlsauth_username requires curl 7.21.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_unix_socket_path) {
#if CURL_AT_LEAST_VERSION(7, 40, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_UNIX_SOCKET_PATH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_unix_socket_path requires curl 7.40.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_useragent) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERAGENT); }
EXTENSION(pg_curl_easy_setopt_username) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERNAME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_username requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_userpwd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERPWD); }
EXTENSION(pg_curl_easy_setopt_xoauth2_bearer) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_XOAUTH2_BEARER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_xoauth2_bearer requires curl 7.33.0 or later")));
#endif
}

static Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURL_LAST;
    long parameter;
    pg_curl_easy_init();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("curl_easy_setopt_* requires argument parameter")));
    parameter = PG_GETARG_INT64(0);
    if ((res = curl_easy_setopt(pg_curl->curl, option, parameter)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%i and %li", option, parameter)));
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_accepttimeout_ms) {
#if CURL_AT_LEAST_VERSION(7, 24, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_ACCEPTTIMEOUT_MS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_accepttimeout_ms requires curl 7.24.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_address_scope) {
#if CURL_AT_LEAST_VERSION(7, 19, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_ADDRESS_SCOPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_address_scope requires curl 7.19.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_append) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_APPEND);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_append requires curl 7.16.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_autoreferer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_AUTOREFERER); }
EXTENSION(pg_curl_easy_setopt_buffersize) {
#if CURL_AT_LEAST_VERSION(7, 10, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_BUFFERSIZE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_buffersize requires curl 7.10.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_certinfo) {
#if CURL_AT_LEAST_VERSION(7, 50, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CERTINFO);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_certinfo requires curl 7.50.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_connect_only) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECT_ONLY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_connect_only requires curl 7.15.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_connecttimeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECTTIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_connecttimeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECTTIMEOUT); }
EXTENSION(pg_curl_easy_setopt_cookiesession) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_COOKIESESSION); }
EXTENSION(pg_curl_easy_setopt_crlf) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CRLF); }
EXTENSION(pg_curl_easy_setopt_dirlistonly) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DIRLISTONLY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_dirlistonly requires curl 7.16.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_disallow_username_in_url) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DISALLOW_USERNAME_IN_URL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_disallow_username_in_url requires curl 7.61.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_cache_timeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_CACHE_TIMEOUT); }
EXTENSION(pg_curl_easy_setopt_dns_shuffle_addresses) {
#if CURL_AT_LEAST_VERSION(7, 60, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_SHUFFLE_ADDRESSES);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_dns_shuffle_addresses requires curl 7.60.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_use_global_cache) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_dns_use_global_cache deprecated: since 7.11.1. Use curl_easy_setopt_share")));
#else
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_USE_GLOBAL_CACHE);
#endif
}
EXTENSION(pg_curl_easy_setopt_expect_100_timeout_ms) {
#if CURL_AT_LEAST_VERSION(7, 36, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_EXPECT_100_TIMEOUT_MS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_expect_100_timeout_ms requires curl 7.36.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_failonerror) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FAILONERROR); }
EXTENSION(pg_curl_easy_setopt_filetime) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FILETIME); }
EXTENSION(pg_curl_easy_setopt_followlocation) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FOLLOWLOCATION); }
EXTENSION(pg_curl_easy_setopt_forbid_reuse) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FORBID_REUSE); }
EXTENSION(pg_curl_easy_setopt_fresh_connect) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FRESH_CONNECT); }
EXTENSION(pg_curl_easy_setopt_ftp_create_missing_dirs) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_CREATE_MISSING_DIRS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_create_missing_dirs requires curl 7.10.7 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_filemethod) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_FILEMETHOD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_filemethod requires curl 7.15.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_skip_pasv_ip) {
#if CURL_AT_LEAST_VERSION(7, 14, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_SKIP_PASV_IP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_skip_pasv_ip requires curl 7.14.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftpsslauth) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTPSSLAUTH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftpsslauth requires curl 7.12.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_ssl_ccc) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_SSL_CCC);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_ssl_ccc requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_use_eprt) {
#if CURL_AT_LEAST_VERSION(7, 10, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_EPRT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_use_eprt requires curl 7.10.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_use_epsv) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_EPSV); }
EXTENSION(pg_curl_easy_setopt_ftp_use_pret) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_PRET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ftp_use_pret requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_gssapi_delegation) {
#if CURL_AT_LEAST_VERSION(7, 22, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_GSSAPI_DELEGATION);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_gssapi_delegation requires curl 7.22.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_happy_eyeballs_timeout_ms) {
#if CURL_AT_LEAST_VERSION(7, 59, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_happy_eyeballs_timeout_ms requires curl 7.59.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_haproxyprotocol) {
#if CURL_AT_LEAST_VERSION(7, 60, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HAPROXYPROTOCOL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_haproxyprotocol requires curl 7.60.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_header) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HEADER); }
EXTENSION(pg_curl_easy_setopt_http09_allowed) {
#if CURL_AT_LEAST_VERSION(7, 64, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP09_ALLOWED);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_http09_allowed requires curl 7.64.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_httpauth) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPAUTH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_httpauth requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_http_content_decoding) {
#if CURL_AT_LEAST_VERSION(7, 16, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_CONTENT_DECODING);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_http_content_decoding requires curl 7.16.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_httpget) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPGET); }
EXTENSION(pg_curl_easy_setopt_httpproxytunnel) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPPROXYTUNNEL); }
EXTENSION(pg_curl_easy_setopt_http_transfer_decoding) {
#if CURL_AT_LEAST_VERSION(7, 16, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_TRANSFER_DECODING);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_http_transfer_decoding requires curl 7.16.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_http_version) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_VERSION); }
EXTENSION(pg_curl_easy_setopt_ignore_content_length) {
#if CURL_AT_LEAST_VERSION(7, 14, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_IGNORE_CONTENT_LENGTH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ignore_content_length requires curl 7.14.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ipresolve) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_IPRESOLVE); }
EXTENSION(pg_curl_easy_setopt_keep_sending_on_error) {
#if CURL_AT_LEAST_VERSION(7, 51, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_KEEP_SENDING_ON_ERROR);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_keep_sending_on_error requires curl 7.51.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_localportrange) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOCALPORTRANGE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_localportrange requires curl 7.15.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_localport) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOCALPORT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_localport requires curl 7.15.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_low_speed_limit) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOW_SPEED_LIMIT); }
EXTENSION(pg_curl_easy_setopt_low_speed_time) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOW_SPEED_TIME); }
EXTENSION(pg_curl_easy_setopt_maxconnects) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_MAXCONNECTS); }
EXTENSION(pg_curl_easy_setopt_maxfilesize) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_MAXFILESIZE); }
EXTENSION(pg_curl_easy_setopt_maxredirs) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_MAXREDIRS); }
EXTENSION(pg_curl_easy_setopt_netrc) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NETRC); }
EXTENSION(pg_curl_easy_setopt_new_directory_perms) {
#if CURL_AT_LEAST_VERSION(7, 16, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NEW_DIRECTORY_PERMS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_new_directory_perms requires curl 7.16.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_new_file_perms) {
#if CURL_AT_LEAST_VERSION(7, 16, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NEW_FILE_PERMS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_new_file_perms requires curl 7.16.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_nobody) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NOBODY); }
EXTENSION(pg_curl_easy_setopt_path_as_is) {
#if CURL_AT_LEAST_VERSION(7, 42, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PATH_AS_IS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_path_as_is requires curl 7.42.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_pipewait) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PIPEWAIT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_pipewait requires curl 7.34.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_port) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PORT); }
EXTENSION(pg_curl_easy_setopt_postredir) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_POSTREDIR);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_postredir requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_post) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_POST); }
EXTENSION(pg_curl_easy_setopt_protocols) {
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_protocols deprecated: since 7.85.0. Use curl_easy_setopt_protocols_str")));
#elif CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROTOCOLS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_protocols requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyauth) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYAUTH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxyauth requires curl 7.10.7 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyport) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYPORT); }
EXTENSION(pg_curl_easy_setopt_proxy_ssl_options) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_OPTIONS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_ssl_options requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_ssl_verifyhost) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_VERIFYHOST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_ssl_verifyhost requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_ssl_verifypeer) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_VERIFYPEER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_ssl_verifypeer requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslversion) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSLVERSION);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_sslversion requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_transfer_mode) {
#if CURL_AT_LEAST_VERSION(7, 18, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_TRANSFER_MODE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_proxy_transfer_mode requires curl 7.18.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_proxytype) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYTYPE); }
EXTENSION(pg_curl_easy_setopt_put) {
#if CURL_AT_LEAST_VERSION(7, 12, 1)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_put deprecated: since 7.12.1. Use curl_easy_setopt_upload")));
#else
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PUT);
#endif
}
EXTENSION(pg_curl_easy_setopt_redir_protocols) {
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_redir_protocols deprecated: since 7.85.0. Use curl_easy_setopt_redir_protocols_str")));
#elif CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_REDIR_PROTOCOLS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_redir_protocols requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_resume_from) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RESUME_FROM); }
EXTENSION(pg_curl_easy_setopt_rtsp_client_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_CLIENT_CSEQ);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_rtsp_client_cseq requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_request) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_REQUEST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_rtsp_request requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_server_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_SERVER_CSEQ);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_rtsp_server_cseq requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_sasl_ir) {
#if CURL_AT_LEAST_VERSION(7, 31, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SASL_IR);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_sasl_ir requires curl 7.31.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_server_response_timeout) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SERVER_RESPONSE_TIMEOUT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_server_response_timeout requires curl 7.10.8 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_socks5_auth) {
#if CURL_AT_LEAST_VERSION(7, 55, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SOCKS5_AUTH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_socks5_auth requires curl 7.55.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_socks5_gssapi_nec) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SOCKS5_GSSAPI_NEC);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_socks5_gssapi_nec requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_auth_types) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSH_AUTH_TYPES);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssh_auth_types requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_compression) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSH_COMPRESSION);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssh_compression requires curl 7.56.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_enable_alpn) {
#if CURL_AT_LEAST_VERSION(7, 36, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_ENABLE_ALPN);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssl_enable_alpn requires curl 7.36.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_enable_npn) {
#if CURL_AT_LEAST_VERSION(7, 86, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_setopt_ssl_enable_npn deprecated: since 7.86.0. Has no function")));
#elif CURL_AT_LEAST_VERSION(7, 36, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_ENABLE_NPN);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssl_enable_npn requires curl 7.36.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_falsestart) {
#if CURL_AT_LEAST_VERSION(7, 42, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_FALSESTART);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssl_falsestart requires curl 7.42.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_options) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_OPTIONS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssl_options requires curl 7.25.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_sessionid_cache) {
#if CURL_AT_LEAST_VERSION(7, 16, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_SESSIONID_CACHE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssl_sessionid_cache requires curl 7.16.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_verifyhost) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYHOST); }
EXTENSION(pg_curl_easy_setopt_ssl_verifypeer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYPEER); }
EXTENSION(pg_curl_easy_setopt_ssl_verifystatus) {
#if CURL_AT_LEAST_VERSION(7, 41, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYSTATUS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_ssl_verifystatus requires curl 7.41.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_sslversion) {
#if CURL_AT_LEAST_VERSION(7, 18, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSLVERSION);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_sslversion requires curl 7.18.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_stream_weight) {
#if CURL_AT_LEAST_VERSION(7, 46, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_STREAM_WEIGHT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_stream_weight requires curl 7.46.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_suppress_connect_headers) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SUPPRESS_CONNECT_HEADERS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_suppress_connect_headers requires curl 7.54.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_fastopen) {
#if CURL_AT_LEAST_VERSION(7, 49, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_FASTOPEN);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tcp_fastopen requires curl 7.49.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_keepalive) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPALIVE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tcp_keepalive requires curl 7.25.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_keepidle) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPIDLE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tcp_keepidle requires curl 7.25.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_keepintvl) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPINTVL); }
EXTENSION(pg_curl_easy_setopt_tcp_nodelay) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_NODELAY); }
EXTENSION(pg_curl_easy_setopt_tftp_blksize) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TFTP_BLKSIZE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tftp_blksize requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_tftp_no_options) {
#if CURL_AT_LEAST_VERSION(7, 48, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TFTP_NO_OPTIONS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_tftp_no_options requires curl 7.48.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_timecondition) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMECONDITION); }
EXTENSION(pg_curl_easy_setopt_timeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_timeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMEOUT); }
EXTENSION(pg_curl_easy_setopt_timevalue) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMEVALUE); }
EXTENSION(pg_curl_easy_setopt_transfer_encoding) {
#if CURL_AT_LEAST_VERSION(7, 21, 6)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TRANSFER_ENCODING);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_transfer_encoding requires curl 7.21.6 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_transfertext) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TRANSFERTEXT); }
EXTENSION(pg_curl_easy_setopt_unrestricted_auth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UNRESTRICTED_AUTH); }
EXTENSION(pg_curl_easy_setopt_upkeep_interval_ms) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPKEEP_INTERVAL_MS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_upkeep_interval_ms requires curl 7.62.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_upload_buffersize) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPLOAD_BUFFERSIZE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_upload_buffersize requires curl 7.62.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_upload) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPLOAD); }
EXTENSION(pg_curl_easy_setopt_use_ssl) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_USE_SSL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_use_ssl requires curl 7.16.5 or later")));
#endif
}
EXTENSION(pg_curl_easy_setopt_verbose) {
    CURLcode res = CURL_LAST;
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_DEBUGDATA, pg_curl)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_DEBUGDATA")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_DEBUGFUNCTION, pg_debug_callback)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_DEBUGFUNCTION")));
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_VERBOSE);
}
EXTENSION(pg_curl_easy_setopt_wildcardmatch) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_WILDCARDMATCH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_setopt_wildcardmatch requires curl 7.21.0 or later")));
#endif
}

#if CURL_AT_LEAST_VERSION(7, 32, 0)
static int pg_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_global.interrupt.requested; }
#endif

static size_t pg_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    pg_curl_t *curl = userdata;
    size *= nitems;
    if (size) appendBinaryStringInfo(&curl->header_in, buffer, size);
    return size;
}

static size_t pg_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    pg_curl_t *curl = userdata;
    size *= nmemb;
    if (size) appendBinaryStringInfo(&curl->data_in, ptr, size);
    return size;
}

EXTENSION(pg_curl_easy_perform) {
    char errbuf[CURL_ERROR_SIZE] = {0};
    CURLcode res = CURL_LAST;
    int try = PG_ARGISNULL(0) ? 1 : PG_GETARG_INT32(0);
    long sleep = PG_ARGISNULL(1) ? 1000000 : PG_GETARG_INT64(1);
    pg_curl_easy_init();
    if (try <= 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("curl_easy_perform invalid argument try %i", try), errhint("Argument try must be positive!")));
    if (sleep < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("curl_easy_perform invalid argument sleep %li", sleep), errhint("Argument sleep must be non-negative!")));
    resetStringInfo(&pg_curl->data_in);
    resetStringInfo(&pg_curl->data_out);
    resetStringInfo(&pg_curl->debug);
    resetStringInfo(&pg_curl->header_in);
    resetStringInfo(&pg_curl->header_out);
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_ERRORBUFFER, errbuf)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_ERRORBUFFER")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_HEADERDATA, pg_curl)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_HEADERDATA")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_HEADERFUNCTION, pg_header_callback)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_HEADERFUNCTION")));
    if (pg_curl->header && ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_HTTPHEADER, pg_curl->header)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_HTTPHEADER")));
    if (pg_curl->postquote && ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_POSTQUOTE, pg_curl->postquote)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_POSTQUOTE")));
    if (pg_curl->prequote && ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_PREQUOTE, pg_curl->prequote)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_PREQUOTE")));
    if (pg_curl->quote && ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_QUOTE, pg_curl->quote)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_QUOTE")));
#if CURL_AT_LEAST_VERSION(7, 32, 0)
    if (pg_curl->recipient && ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_MAIL_RCPT, pg_curl->recipient)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_MAIL_RCPT")));
#endif
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    if (pg_curl->mime && ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_MIMEPOST, pg_curl->mime)) != CURLE_OK)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_MIMEPOST")));
#endif
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_NOPROGRESS, 0L)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_NOPROGRESS and 0")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_NOSIGNAL, 1L)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_NOSIGNAL and 1")));
    if (pg_curl->postfield.len && (res = curl_easy_setopt(pg_curl->curl, CURLOPT_POSTFIELDSIZE, pg_curl->postfield.len)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_POSTFIELDSIZE")));
    if (pg_curl->postfield.len && (res = curl_easy_setopt(pg_curl->curl, CURLOPT_POSTFIELDS, pg_curl->postfield.data)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_POSTFIELDS")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_URL, pg_curl->url.data)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_URL")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_WRITEDATA, pg_curl)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_WRITEDATA")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_WRITEFUNCTION, pg_write_callback)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_WRITEFUNCTION")));
#if CURL_AT_LEAST_VERSION(7, 32, 0)
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_XFERINFODATA, pg_curl)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_XFERINFODATA")));
    if ((res = curl_easy_setopt(pg_curl->curl, CURLOPT_XFERINFOFUNCTION, pg_progress_callback)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_setopt failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("CURLOPT_XFERINFOFUNCTION")));
#endif
    pg_curl_global.interrupt.requested = 0;
    while (try--) switch (res = curl_easy_perform(pg_curl->curl)) {
        case CURLE_OK: try = 0; break;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_FAILED_INIT:
        case CURLE_URL_MALFORMAT:
        case CURLE_NOT_BUILT_IN:
        case CURLE_FUNCTION_NOT_FOUND:
        case CURLE_BAD_FUNCTION_ARGUMENT:
        case CURLE_UNKNOWN_OPTION:
        case CURLE_LDAP_INVALID_URL:
        case CURLE_ABORTED_BY_CALLBACK: try = 0; if (pg_curl_global.interrupt.handler && pg_curl_global.interrupt.requested) { (*pg_curl_global.interrupt.handler)(pg_curl_global.interrupt.requested); pg_curl_global.interrupt.requested = 0; } // fall through
        default: if (try) {
            if (strlen(errbuf)) ereport(WARNING, (errmsg("curl_easy_perform failed"), errdetail("%s and %s", curl_easy_strerror(res), errbuf), errcontext("try %i", try)));
            else ereport(WARNING, (errmsg("curl_easy_perform failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("try %i", try)));
            if (sleep) pg_usleep(sleep);
        } else {
            if (strlen(errbuf)) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("curl_easy_perform failed"), errdetail("%s and %s", curl_easy_strerror(res), errbuf)));
            else ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("curl_easy_perform failed"), errdetail("%s", curl_easy_strerror(res))));
        }
    }
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_getinfo_debug) {
    pg_curl_easy_init();
    if (!pg_curl->debug.len) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(pg_curl->debug.data, pg_curl->debug.len));
}

EXTENSION(pg_curl_easy_getinfo_header_in) {
    pg_curl_easy_init();
    if (!pg_curl->header_in.len) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(pg_curl->header_in.data, pg_curl->header_in.len));
}

EXTENSION(pg_curl_easy_getinfo_header_out) {
    pg_curl_easy_init();
    if (!pg_curl->header_out.len) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(pg_curl->header_out.data, pg_curl->header_out.len));
}

EXTENSION(pg_curl_easy_getinfo_data_in) {
    pg_curl_easy_init();
    if (!pg_curl->data_in.len) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(cstring_to_text_with_len(pg_curl->data_in.data, pg_curl->data_in.len));
}

EXTENSION(pg_curl_easy_getinfo_data_out) {
    pg_curl_easy_init();
    if (!pg_curl->data_out.len) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(cstring_to_text_with_len(pg_curl->data_out.data, pg_curl->data_out.len));
}

EXTENSION(pg_curl_easy_getinfo_headers) {
    pg_curl_easy_init();
    ereport(WARNING, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_getinfo_headers deprecated, use curl_easy_getinfo_header_in instead")));
    return pg_curl_easy_getinfo_header_in(fcinfo);
}

EXTENSION(pg_curl_easy_getinfo_response) {
    pg_curl_easy_init();
    ereport(WARNING, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_getinfo_response deprecated, use curl_easy_getinfo_data_in instead")));
    return pg_curl_easy_getinfo_data_in(fcinfo);
}

static Datum pg_curl_easy_getinfo_char(PG_FUNCTION_ARGS, CURLINFO info) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    CURLcode res = CURL_LAST;
    char *value = NULL;
    pg_curl_easy_init();
    if ((res = curl_easy_getinfo(pg_curl->curl, info, &value)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_getinfo failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%i", info)));
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(value));
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_* requires curl 7.4.1 or later")));
#endif
}

EXTENSION(pg_curl_easy_getinfo_content_type) {
#if CURL_AT_LEAST_VERSION(7, 9, 4)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_CONTENT_TYPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_content_type requires curl 7.9.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_effective_url) {
#if CURL_AT_LEAST_VERSION(7, 4, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_EFFECTIVE_URL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_effective_url requires curl 7.4.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_ftp_entry_path) {
#if CURL_AT_LEAST_VERSION(7, 15, 4)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_FTP_ENTRY_PATH);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_ftp_entry_path requires curl 7.15.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_local_ip) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_LOCAL_IP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_local_ip requires curl 7.21.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_primary_ip) {
#if CURL_AT_LEAST_VERSION(7, 19, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_PRIMARY_IP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_primary_ip requires curl 7.19.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_private) {
#if CURL_AT_LEAST_VERSION(7, 10, 3)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_PRIVATE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_private requires curl 7.10.3 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_redirect_url) {
#if CURL_AT_LEAST_VERSION(7, 18, 2)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_REDIRECT_URL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_redirect_url requires curl 7.18.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_session_id) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_RTSP_SESSION_ID);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_rtsp_session_id requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_scheme) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_SCHEME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_scheme requires curl 7.52.0 or later")));
#endif
}

static Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS, CURLINFO info) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    CURLcode res = CURL_LAST;
    long value;
    pg_curl_easy_init();
    if ((res = curl_easy_getinfo(pg_curl->curl, info, &value)) != CURLE_OK) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("curl_easy_getinfo failed"), errdetail("%s", curl_easy_strerror(res)), errcontext("%i", info)));
    PG_RETURN_INT64(value);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_* requires curl 7.4.1 or later")));
#endif
}

EXTENSION(pg_curl_easy_getinfo_activesocket) {
#if CURL_AT_LEAST_VERSION(7, 45, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_ACTIVESOCKET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_activesocket requires curl 7.45.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_condition_unmet) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_CONDITION_UNMET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_condition_unmet requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_filetime) {
#if CURL_AT_LEAST_VERSION(7, 5, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_FILETIME);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_filetime requires curl 7.5.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_header_size) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HEADER_SIZE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_header_size requires curl 7.4.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_httpauth_avail) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTPAUTH_AVAIL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_httpauth_avail requires curl 7.10.8 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_http_connectcode) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTP_CONNECTCODE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_http_connectcode requires curl 7.10.7 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_http_version) {
#if CURL_AT_LEAST_VERSION(7, 50, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTP_VERSION);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_http_version requires curl 7.50.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_lastsocket) {
#if CURL_AT_LEAST_VERSION(7, 45, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_getinfo_lastsocket deprecated: since 7.45.0. Use curl_easy_getinfo_activesocket")));
#elif CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_LASTSOCKET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_lastsocket requires curl 7.15.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_local_port) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_LOCAL_PORT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_local_port requires curl 7.21.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_num_connects) {
#if CURL_AT_LEAST_VERSION(7, 12, 3)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_NUM_CONNECTS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_num_connects requires curl 7.12.3 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_os_errno) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_OS_ERRNO);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_os_errno requires curl 7.12.2 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_primary_port) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PRIMARY_PORT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_primary_port requires curl 7.21.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_protocol) {
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    ereport(ERROR, (errcode(ERRCODE_WARNING_DEPRECATED_FEATURE), errmsg("curl_easy_getinfo_protocol deprecated: since 7.85.0. Use curl_easy_getinfo_scheme")));
#elif CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROTOCOL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_protocol requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_proxyauth_avail) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROXYAUTH_AVAIL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_proxyauth_avail requires curl 7.10.8 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_proxy_ssl_verifyresult) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROXY_SSL_VERIFYRESULT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_proxy_ssl_verifyresult requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_redirect_count) {
#if CURL_AT_LEAST_VERSION(7, 9, 7)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_REDIRECT_COUNT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_redirect_count requires curl 7.9.7 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_request_size) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_REQUEST_SIZE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_request_size requires curl 7.4.1 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_response_code) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RESPONSE_CODE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_response_code requires curl 7.10.8 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_client_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_CLIENT_CSEQ);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_rtsp_client_cseq requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_cseq_recv) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_CSEQ_RECV);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_rtsp_cseq_recv requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_server_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_SERVER_CSEQ);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_rtsp_server_cseq requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_easy_getinfo_ssl_verifyresult) {
#if CURL_AT_LEAST_VERSION(7, 5, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_SSL_VERIFYRESULT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_easy_getinfo_ssl_verifyresult requires curl 7.5.0 or later")));
#endif
}

EXTENSION(pg_curl_http_version_1_0) { PG_RETURN_INT64(CURL_HTTP_VERSION_1_0); }
EXTENSION(pg_curl_http_version_1_1) { PG_RETURN_INT64(CURL_HTTP_VERSION_1_1); }
EXTENSION(pg_curl_http_version_2_0) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    PG_RETURN_INT64(CURL_HTTP_VERSION_2_0);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("_curl_http_version_2_0 requires curl 7.33.0 or later")));
#endif
}
EXTENSION(pg_curl_http_version_2_prior_knowledge) {
#if CURL_AT_LEAST_VERSION(7, 49, 0)
    PG_RETURN_INT64(CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_http_version_2_prior_knowledge requires curl 7.49.0 or later")));
#endif
}
EXTENSION(pg_curl_http_version_2tls) {
#if CURL_AT_LEAST_VERSION(7, 47, 0)
    PG_RETURN_INT64(CURL_HTTP_VERSION_2TLS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_http_version_2tls requires curl 7.47.0 or later")));
#endif
}
EXTENSION(pg_curl_http_version_3) {
#if CURL_AT_LEAST_VERSION(7, 66, 0)
    PG_RETURN_INT64(CURL_HTTP_VERSION_3);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_http_version_3 requires curl 7.66.0 or later")));
#endif
}
EXTENSION(pg_curl_http_version_none) { PG_RETURN_INT64(CURL_HTTP_VERSION_NONE); }

EXTENSION(pg_curlusessl_none) {
#if CURL_AT_LEAST_VERSION(7, 11, 0)
    PG_RETURN_INT64(CURLUSESSL_NONE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlusessl_none requires curl 7.11.0 or later")));
#endif
}
EXTENSION(pg_curlusessl_try) {
#if CURL_AT_LEAST_VERSION(7, 11, 0)
    PG_RETURN_INT64(CURLUSESSL_TRY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlusessl_try requires curl 7.11.0 or later")));
#endif
}
EXTENSION(pg_curlusessl_control) {
#if CURL_AT_LEAST_VERSION(7, 11, 0)
    PG_RETURN_INT64(CURLUSESSL_CONTROL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlusessl_control requires curl 7.11.0 or later")));
#endif
}
EXTENSION(pg_curlusessl_all) {
#if CURL_AT_LEAST_VERSION(7, 11, 0)
    PG_RETURN_INT64(CURLUSESSL_ALL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlusessl_all requires curl 7.11.0 or later")));
#endif
}

EXTENSION(pg_curl_upkeep_interval_default) {
#ifdef CURL_UPKEEP_INTERVAL_DEFAULT
    PG_RETURN_INT64(CURL_UPKEEP_INTERVAL_DEFAULT);
#else
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("CURL_UPKEEP_INTERVAL_DEFAULT undefined")));
#endif
}

EXTENSION(pg_curl_timecond_none) { PG_RETURN_INT64(CURL_TIMECOND_NONE); }
EXTENSION(pg_curl_timecond_ifmodsince) { PG_RETURN_INT64(CURL_TIMECOND_IFMODSINCE); }
EXTENSION(pg_curl_timecond_ifunmodsince) { PG_RETURN_INT64(CURL_TIMECOND_IFUNMODSINCE); }

EXTENSION(pg_curl_sslversion_default) { PG_RETURN_INT64(CURL_SSLVERSION_DEFAULT); }
EXTENSION(pg_curl_sslversion_tlsv1) { PG_RETURN_INT64(CURL_SSLVERSION_TLSv1); }
EXTENSION(pg_curl_sslversion_sslv2) { PG_RETURN_INT64(CURL_SSLVERSION_SSLv2); }
EXTENSION(pg_curl_sslversion_sslv3) { PG_RETURN_INT64(CURL_SSLVERSION_SSLv3); }
EXTENSION(pg_curl_sslversion_tlsv1_0) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_0);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_tlsv1_0 requires curl 7.34.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_tlsv1_1) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_1);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_tlsv1_1 requires curl 7.34.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_tlsv1_2) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_2);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_tlsv1_2 requires curl 7.34.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_tlsv1_3) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_3);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_tlsv1_3 requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_max_default) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_DEFAULT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_max_default requires curl 7.54.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_0) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_0);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_max_tlsv1_0 requires curl 7.54.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_1) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_1);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_max_tlsv1_1 requires curl 7.54.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_2) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_2);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_max_tlsv1_2 requires curl 7.54.0 or later")));
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_3) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_3);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_sslversion_max_tlsv1_3 requires curl 7.54.0 or later")));
#endif
}

EXTENSION(pg_curlsslopt_allow_beast) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    PG_RETURN_INT64(CURLSSLOPT_ALLOW_BEAST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlsslopt_allow_beast requires curl 7.25.0 or later")));
#endif
}
EXTENSION(pg_curlsslopt_no_revoke) {
#if CURL_AT_LEAST_VERSION(7, 44, 0)
    PG_RETURN_INT64(CURLSSLOPT_NO_REVOKE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlsslopt_no_revoke requires curl 7.44.0 or later")));
#endif
}
EXTENSION(pg_curlsslopt_no_partialchain) {
#if CURL_AT_LEAST_VERSION(7, 68, 0)
    PG_RETURN_INT64(CURLSSLOPT_NO_PARTIALCHAIN);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlsslopt_no_partialchain requires curl 7.68.0 or later")));
#endif
}
EXTENSION(pg_curlsslopt_revoke_best_effort) {
#if CURL_AT_LEAST_VERSION(7, 70, 0)
    PG_RETURN_INT64(CURLSSLOPT_REVOKE_BEST_EFFORT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlsslopt_revoke_best_effort requires curl 7.70.0 or later")));
#endif
}
EXTENSION(pg_curlsslopt_native_ca) {
#if CURL_AT_LEAST_VERSION(7, 71, 0)
    PG_RETURN_INT64(CURLSSLOPT_NATIVE_CA);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlsslopt_native_ca requires curl 7.71.0 or later")));
#endif
}
EXTENSION(pg_curlsslopt_auto_client_cert) {
#if CURL_AT_LEAST_VERSION(7, 77, 0)
    PG_RETURN_INT64(CURLSSLOPT_AUTO_CLIENT_CERT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlsslopt_auto_client_cert requires curl 7.77.0 or later")));
#endif
}

EXTENSION(pg_curlssh_auth_publickey) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLSSH_AUTH_PUBLICKEY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlssh_auth_publickey requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curlssh_auth_password) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLSSH_AUTH_PASSWORD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlssh_auth_password requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curlssh_auth_host) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLSSH_AUTH_HOST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlssh_auth_host requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curlssh_auth_keyboard) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLSSH_AUTH_KEYBOARD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlssh_auth_keyboard requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curlssh_auth_agent) {
#if CURL_AT_LEAST_VERSION(7, 28, 0)
    PG_RETURN_INT64(CURLSSH_AUTH_AGENT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlssh_auth_agent requires curl 7.28.0 or later")));
#endif
}
EXTENSION(pg_curlssh_auth_any) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLSSH_AUTH_ANY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlssh_auth_any requires curl 7.16.1 or later")));
#endif
}

EXTENSION(pg_curlauth_basic) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_BASIC);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_basic requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curlauth_digest) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_DIGEST);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_digest requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curlauth_digest_ie) {
#if CURL_AT_LEAST_VERSION(7, 19, 3)
    PG_RETURN_INT64(CURLAUTH_DIGEST_IE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_digest_ie requires curl 7.19.3 or later")));
#endif
}
EXTENSION(pg_curlauth_bearer) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    PG_RETURN_INT64(CURLAUTH_BEARER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_bearer requires curl 7.61.0 or later")));
#endif
}
EXTENSION(pg_curlauth_negotiate) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_NEGOTIATE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_negotiate requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curlauth_ntlm) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_NTLM);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_ntlm requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curlauth_ntlm_wb) {
#if CURL_AT_LEAST_VERSION(7, 22, 0)
    PG_RETURN_INT64(CURLAUTH_NTLM_WB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_ntlm_wb requires curl 7.22.0 or later")));
#endif
}
EXTENSION(pg_curlauth_any) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_ANY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_any requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curlauth_anysafe) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_ANYSAFE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_anysafe requires curl 7.10.6 or later")));
#endif
}
EXTENSION(pg_curlauth_only) {
#if CURL_AT_LEAST_VERSION(7, 21, 3)
    PG_RETURN_INT64(CURLAUTH_ONLY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_only requires curl 7.21.3 or later")));
#endif
}
EXTENSION(pg_curlauth_aws_sigv4) {
#if CURL_AT_LEAST_VERSION(7, 75, 0)
    PG_RETURN_INT64(CURLAUTH_AWS_SIGV4);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_aws_sigv4 requires curl 7.75.0 or later")));
#endif
}
EXTENSION(pg_curlauth_gssapi) {
#ifdef CURLAUTH_GSSAPI
    PG_RETURN_INT64(CURLAUTH_GSSAPI);
#else
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("CURLAUTH_GSSAPI undefined")));
#endif
}
EXTENSION(pg_curlauth_none) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    PG_RETURN_INT64(CURLAUTH_NONE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlauth_none requires curl 7.10.6 or later")));
#endif
}

EXTENSION(pg_curl_rtspreq_options) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_OPTIONS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_options requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_describe) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_DESCRIBE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_describe requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_announce) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_ANNOUNCE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_announce requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_setup) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_SETUP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_setup requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_play) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_PLAY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_play requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_pause) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_PAUSE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("url_rtspreq_pause requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_teardown) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_TEARDOWN);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_teardown requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_get_parameter) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_GET_PARAMETER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_get_parameter requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_set_parameter) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_SET_PARAMETER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_set_parameter requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_record) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_RECORD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_record requires curl 7.20.0 or later")));
#endif
}
EXTENSION(pg_curl_rtspreq_receive) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    PG_RETURN_INT64(CURL_RTSPREQ_RECEIVE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_rtspreq_receive requires curl 7.20.0 or later")));
#endif
}

EXTENSION(pg_curlproto_dict) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_DICT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_dict requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_file) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_FILE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_file requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_ftp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_FTP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_ftp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_ftps) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_FTPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_ftps requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_gopher) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_GOPHER);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_gopher requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_http) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_HTTP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_http requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_https) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_HTTPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_https requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_imap) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_IMAP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_imap requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_imaps) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_IMAPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_imaps requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_ldap) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_LDAP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_ldap requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_ldaps) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_LDAPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_ldaps requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_pop3) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_POP3);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_pop3 requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_pop3s) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_POP3S);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_pop3s requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtmp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTMP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtmp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtmpe) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTMPE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtmpe requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtmps) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTMPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtmps requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtmpt) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTMPT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtmpt requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtmpte) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTMPTE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtmpte requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtmpts) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTMPTS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtmpts requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_rtsp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_RTSP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_rtsp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_scp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_SCP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_scp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_sftp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_SFTP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_sftp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_smb) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_SMB);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_smb requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_smbs) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_SMBS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_smbs requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_smtp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_SMTP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_smtp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_smtps) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_SMTPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_smtps requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_telnet) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_TELNET);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_telnet requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_tftp) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_TFTP);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_tftp requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlproto_all) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLPROTO_ALL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproto_all requires curl 7.19.4 or later")));
#endif
}

EXTENSION(pg_curlproxy_http) { PG_RETURN_INT64(CURLPROXY_HTTP); }
EXTENSION(pg_curlproxy_https) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    PG_RETURN_INT64(CURLPROXY_HTTPS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlproxy_https requires curl 7.52.0 or later")));
#endif
}
EXTENSION(pg_curlproxy_http_1_0) { PG_RETURN_INT64(CURLPROXY_HTTP_1_0); }
EXTENSION(pg_curlproxy_socks4) { PG_RETURN_INT64(CURLPROXY_SOCKS4); }
EXTENSION(pg_curlproxy_socks4a) { PG_RETURN_INT64(CURLPROXY_SOCKS4A); }
EXTENSION(pg_curlproxy_socks5) { PG_RETURN_INT64(CURLPROXY_SOCKS5); }
EXTENSION(pg_curlproxy_socks5_hostname) { PG_RETURN_INT64(CURLPROXY_SOCKS5_HOSTNAME); }

EXTENSION(pg_curl_redir_post_301) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    PG_RETURN_INT64(CURL_REDIR_POST_301);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_redir_post_301 requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_redir_post_302) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    PG_RETURN_INT64(CURL_REDIR_POST_302);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_redir_post_302 requires curl 7.19.1 or later")));
#endif
}
EXTENSION(pg_curl_redir_post_303) {
#if CURL_AT_LEAST_VERSION(7, 26, 0)
    PG_RETURN_INT64(CURL_REDIR_POST_303);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_redir_post_303 requires curl 7.26.0 or later")));
#endif
}
EXTENSION(pg_curl_redir_post_all) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    PG_RETURN_INT64(CURL_REDIR_POST_ALL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curl_redir_post_all requires curl 7.19.1 or later")));
#endif
}

EXTENSION(pg_curl_netrc_optional) { PG_RETURN_INT64(CURL_NETRC_OPTIONAL); }
EXTENSION(pg_curl_netrc_ignored) { PG_RETURN_INT64(CURL_NETRC_IGNORED); }
EXTENSION(pg_curl_netrc_required) { PG_RETURN_INT64(CURL_NETRC_REQUIRED); }

EXTENSION(pg_curl_ipresolve_whatever) { PG_RETURN_INT64(CURL_IPRESOLVE_WHATEVER); }
EXTENSION(pg_curl_ipresolve_v4) { PG_RETURN_INT64(CURL_IPRESOLVE_V4); }
EXTENSION(pg_curl_ipresolve_v6) { PG_RETURN_INT64(CURL_IPRESOLVE_V6); }

EXTENSION(pg_curl_het_default) {
#ifdef CURL_HET_DEFAULT
    PG_RETURN_INT64(CURL_HET_DEFAULT);
#else
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("CURL_HET_DEFAULT undefined")));
#endif
}

EXTENSION(pg_curlgssapi_delegation_flag) {
#if CURL_AT_LEAST_VERSION(7, 22, 0)
    PG_RETURN_INT64(CURLGSSAPI_DELEGATION_FLAG);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlgssapi_delegation_flag requires curl 7.22.0 or later")));
#endif
}
EXTENSION(pg_curlgssapi_delegation_policy_flag) {
#if CURL_AT_LEAST_VERSION(7, 22, 0)
    PG_RETURN_INT64(CURLGSSAPI_DELEGATION_POLICY_FLAG);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlgssapi_delegation_policy_flag requires curl 7.22.0 or later")));
#endif
}
EXTENSION(pg_curlgssapi_delegation_none) {
#if CURL_AT_LEAST_VERSION(7, 22, 0)
    PG_RETURN_INT64(CURLGSSAPI_DELEGATION_NONE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlgssapi_delegation_none requires curl 7.22.0 or later")));
#endif
}

EXTENSION(pg_curlftpssl_ccc_none) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLFTPSSL_CCC_NONE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpssl_ccc_none requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curlftpssl_ccc_passive) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLFTPSSL_CCC_PASSIVE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpssl_ccc_passive requires curl 7.16.1 or later")));
#endif
}
EXTENSION(pg_curlftpssl_ccc_active) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    PG_RETURN_INT64(CURLFTPSSL_CCC_ACTIVE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpssl_ccc_active requires curl 7.16.1 or later")));
#endif
}

EXTENSION(pg_curlftpauth_default) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    PG_RETURN_INT64(CURLFTPAUTH_DEFAULT);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpauth_default requires curl 7.12.2 or later")));
#endif
}
EXTENSION(pg_curlftpauth_ssl) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    PG_RETURN_INT64(CURLFTPAUTH_SSL);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpauth_ssl requires curl 7.12.2 or later")));
#endif
}
EXTENSION(pg_curlftpauth_tls) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    PG_RETURN_INT64(CURLFTPAUTH_TLS);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpauth_tls requires curl 7.12.2 or later")));
#endif
}

EXTENSION(pg_curlftpmethod_multicwd) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    PG_RETURN_INT64(CURLFTPMETHOD_MULTICWD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpmethod_multicwd requires curl 7.15.1 or later")));
#endif
}
EXTENSION(pg_curlftpmethod_nocwd) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    PG_RETURN_INT64(CURLFTPMETHOD_NOCWD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpmethod_nocwd requires curl 7.15.1 or later")));
#endif
}
EXTENSION(pg_curlftpmethod_singlecwd) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    PG_RETURN_INT64(CURLFTPMETHOD_SINGLECWD);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftpmethod_singlecwd requires curl 7.15.1 or later")));
#endif
}

EXTENSION(pg_curlftp_create_dir) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    PG_RETURN_INT64(CURLFTP_CREATE_DIR);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftp_create_dir requires curl 7.10.7 or later")));
#endif
}
EXTENSION(pg_curlftp_create_dir_retry) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLFTP_CREATE_DIR_RETRY);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftp_create_dir_retry requires curl 7.19.4 or later")));
#endif
}
EXTENSION(pg_curlftp_create_dir_none) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    PG_RETURN_INT64(CURLFTP_CREATE_DIR_NONE);
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("curlftp_create_dir_none requires curl 7.10.7 or later")));
#endif
}

EXTENSION(pg_curl_max_write_size) {
#ifdef CURL_MAX_WRITE_SIZE
    PG_RETURN_INT64(CURL_MAX_WRITE_SIZE);
#else
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("CURL_MAX_WRITE_SIZE undefined")));
#endif
}

#if PG_VERSION_NUM >= 90500
void _PG_init(void); void _PG_init(void) {
    DefineCustomBoolVariable("pg_curl.transaction", "pg_curl transaction", "Use transaction context?", &pg_curl_transaction, true, PGC_USERSET, 0, NULL, NULL, NULL);
}
#endif

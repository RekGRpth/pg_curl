#include <postgres.h>
#include <fmgr.h>

#include <catalog/pg_type.h>
#include <curl/curl.h>
#include <signal.h>
#include <utils/builtins.h>

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

CURL *curl = NULL;
StringInfoData read_buf;
StringInfoData write_buf;
pqsigfunc pgsql_interrupt_handler = NULL;
int pg_curl_interrupt_requested = 0;
struct curl_slist *header = NULL;
struct curl_slist *recipient = NULL;
curl_mime *mime;
bool has_mime;
char *encoding = NULL;

static inline void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }

static inline void *custom_calloc(size_t nmemb, size_t size) { return ((nmemb > 0) && (size > 0)) ? (palloc0)(nmemb * size) : NULL; }
static inline void *custom_malloc(size_t size) { return size ? (palloc)(size) : NULL; }
static inline char *custom_strdup(const char *ptr) { return (pstrdup)(ptr); }
static inline void *custom_realloc(void *ptr, size_t size) { return size ? ptr ? (repalloc)(ptr, size) : palloc(size) : ptr; }
static inline void custom_free(void *ptr) { if (ptr) (void)(pfree)(ptr); }

void _PG_init(void) {
    if (curl_global_init_mem(CURL_GLOBAL_ALL, custom_malloc, custom_free, custom_realloc, custom_strdup, custom_calloc)) ereport(ERROR, (errmsg("curl_global_init")));
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
    pg_curl_interrupt_requested = 0;
    (void)initStringInfo(&read_buf);
    (void)initStringInfo(&write_buf);
}

void _PG_fini(void) {
    (pqsigfunc)pqsignal(SIGINT, pgsql_interrupt_handler);
    if (curl) (void)curl_easy_cleanup(curl);
    (void)curl_mime_free(mime);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
    (void)curl_global_cleanup();
    (void)pfree(read_buf.data);
    (void)pfree(write_buf.data);
    if (encoding) (void)pfree(encoding);
}

Datum pg_curl_easy_init(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_init); Datum pg_curl_easy_init(PG_FUNCTION_ARGS) {
    if (curl) ereport(ERROR, (errmsg("already init!")));
    curl = curl_easy_init();
    if (!curl) ereport(ERROR, (errmsg("!curl")));
    mime = curl_mime_init(curl);
    if (!mime) ereport(ERROR, (errmsg("!mime")));
    has_mime = false;
    PG_RETURN_BOOL(curl != NULL);
}

Datum pg_curl_easy_reset(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_reset); Datum pg_curl_easy_reset(PG_FUNCTION_ARGS) {
    if (!curl) ereport(ERROR, (errmsg("!curl")));
    (void)curl_easy_reset(curl);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
    (void)curl_mime_free(mime);
    if (encoding) (void)pfree(encoding);
    encoding = NULL;
    mime = curl_mime_init(curl);
    if (!mime) ereport(ERROR, (errmsg("!mime")));
    has_mime = false;
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

Datum pg_curl_header_append(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_header_append); Datum pg_curl_header_append(PG_FUNCTION_ARGS) {
    char *name, *value;
    StringInfoData buf;
    struct curl_slist *temp = header;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument option must not null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument parameter must not null!")));
    value = TextDatumGetCString(PG_GETARG_DATUM(1));
    (void)initStringInfo(&buf);
    (void)appendStringInfo(&buf, "%s: %s", name, value);
    if ((temp = curl_slist_append(temp, buf.data))) header = temp;
    (void)pfree(name);
    (void)pfree(value);
    (void)pfree(buf.data);
    PG_RETURN_BOOL(temp != NULL);
}

Datum pg_curl_recipient_append(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_recipient_append); Datum pg_curl_recipient_append(PG_FUNCTION_ARGS) {
    char *name;
    struct curl_slist *temp = recipient;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument option must not null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, name))) recipient = temp;
    (void)pfree(name);
    PG_RETURN_BOOL(temp != NULL);
}

Datum pg_curl_mime_encoder(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_encoder); Datum pg_curl_mime_encoder(PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument encoding must not null!")));
    if (encoding) (void)pfree(encoding);
    encoding = TextDatumGetCString(PG_GETARG_DATUM(0));
    PG_RETURN_BOOL(true);
}

Datum pg_curl_mime_data(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_data); Datum pg_curl_mime_data(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *data;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument name must not null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, data, CURL_ZERO_TERMINATED)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", data, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(data);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_mime_filedata(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_filedata); Datum pg_curl_mime_filedata(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *filename, *base = NULL, *type = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument filename must not null!")));
    filename = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) base = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) type = TextDatumGetCString(PG_GETARG_DATUM(2));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_filedata(part, filename)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_filedata(%s): %s", filename, curl_easy_strerror(res))));
    if (base && ((res = curl_mime_filename(part, base)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_filename(%s): %s", base, curl_easy_strerror(res))));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(filename);
    if (base) (void)pfree(base);
    if (type) (void)pfree(type);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_mime_data_name(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_data_name); Datum pg_curl_mime_data_name(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *data, *name;
    curl_mimepart *part;
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("first argument data must not null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("second argument name must not null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(1));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, data, CURL_ZERO_TERMINATED)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", data, curl_easy_strerror(res))));
    if ((res = curl_mime_name(part, name)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_name(%s): %s", name, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(data);
    (void)pfree(name);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_mime_data_type(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_data_type); Datum pg_curl_mime_data_type(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *data, *type;
    curl_mimepart *part;
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("first argument data must not null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("second argument type must not null!")));
    type = TextDatumGetCString(PG_GETARG_DATUM(1));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, data, CURL_ZERO_TERMINATED)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", data, curl_easy_strerror(res))));
    if ((res = curl_mime_type(part, type)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(data);
    (void)pfree(type);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_setopt_char(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt_char); Datum pg_curl_easy_setopt_char(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *option_char, *parameter_char;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument option must not null!")));
    option_char = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument parameter must not null!")));
    parameter_char = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!pg_strncasecmp(option_char, "CURLOPT_READDATA", sizeof("CURLOPT_READDATA") - 1)) {
        long parameter_len = strlen(parameter_char);
        (void)appendBinaryStringInfo(&read_buf, parameter_char, parameter_len);
        if ((res = curl_easy_setopt(curl, CURLOPT_INFILESIZE, parameter_len)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_INFILESIZE): %s", curl_easy_strerror(res))));
        if ((res = curl_easy_setopt(curl, CURLOPT_READDATA, (void *)&read_buf)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_READDATA): %s", curl_easy_strerror(res))));
        if ((res = curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_READFUNCTION): %s", curl_easy_strerror(res))));
        if ((res = curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_UPLOAD): %s", curl_easy_strerror(res))));
        goto ret;
    }
    if (false);
    else if (!pg_strncasecmp(option_char, "CURLOPT_ACCEPT_ENCODING", sizeof("CURLOPT_ACCEPT_ENCODING") - 1)) option = CURLOPT_ACCEPT_ENCODING;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CAINFO", sizeof("CURLOPT_CAINFO") - 1)) option = CURLOPT_CAINFO;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CUSTOMREQUEST", sizeof("CURLOPT_CUSTOMREQUEST") - 1)) option = CURLOPT_CUSTOMREQUEST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_SERVERS", sizeof("CURLOPT_DNS_SERVERS") - 1)) option = CURLOPT_DNS_SERVERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_EGDSOCKET", sizeof("CURLOPT_EGDSOCKET") - 1)) option = CURLOPT_EGDSOCKET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAIL_AUTH", sizeof("CURLOPT_MAIL_AUTH") - 1)) option = CURLOPT_MAIL_AUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAIL_FROM", sizeof("CURLOPT_MAIL_FROM") - 1)) option = CURLOPT_MAIL_FROM;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PASSWORD", sizeof("CURLOPT_PASSWORD") - 1)) option = CURLOPT_PASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PRE_PROXY", sizeof("CURLOPT_PRE_PROXY") - 1)) option = CURLOPT_PRE_PROXY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_CAINFO", sizeof("CURLOPT_PROXY_CAINFO") - 1)) option = CURLOPT_PROXY_CAINFO;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYPASSWORD", sizeof("CURLOPT_PROXYPASSWORD") - 1)) option = CURLOPT_PROXYPASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY", sizeof("CURLOPT_PROXY") - 1)) option = CURLOPT_PROXY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSL_CIPHER_LIST", sizeof("CURLOPT_PROXY_SSL_CIPHER_LIST") - 1)) option = CURLOPT_PROXY_SSL_CIPHER_LIST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLS13_CIPHERS", sizeof("CURLOPT_PROXY_TLS13_CIPHERS") - 1)) option = CURLOPT_PROXY_TLS13_CIPHERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLSAUTH_PASSWORD", sizeof("CURLOPT_PROXY_TLSAUTH_PASSWORD") - 1)) option = CURLOPT_PROXY_TLSAUTH_PASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLSAUTH_TYPE", sizeof("CURLOPT_PROXY_TLSAUTH_TYPE") - 1)) option = CURLOPT_PROXY_TLSAUTH_TYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLSAUTH_USERNAME", sizeof("CURLOPT_PROXY_TLSAUTH_USERNAME") - 1)) option = CURLOPT_PROXY_TLSAUTH_USERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYUSERNAME", sizeof("CURLOPT_PROXYUSERNAME") - 1)) option = CURLOPT_PROXYUSERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RANDOM_FILE", sizeof("CURLOPT_RANDOM_FILE") - 1)) option = CURLOPT_RANDOM_FILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_REQUEST_TARGET", sizeof("CURLOPT_REQUEST_TARGET") - 1)) option = CURLOPT_REQUEST_TARGET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLCERT", sizeof("CURLOPT_SSLCERT") - 1)) option = CURLOPT_SSLCERT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLCERTTYPE", sizeof("CURLOPT_SSLCERTTYPE") - 1)) option = CURLOPT_SSLCERTTYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_CIPHER_LIST", sizeof("CURLOPT_SSL_CIPHER_LIST") - 1)) option = CURLOPT_SSL_CIPHER_LIST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLKEY", sizeof("CURLOPT_SSLKEY") - 1)) option = CURLOPT_SSLKEY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLS13_CIPHERS", sizeof("CURLOPT_TLS13_CIPHERS") - 1)) option = CURLOPT_TLS13_CIPHERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLSAUTH_PASSWORD", sizeof("CURLOPT_TLSAUTH_PASSWORD") - 1)) option = CURLOPT_TLSAUTH_PASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLSAUTH_TYPE", sizeof("CURLOPT_TLSAUTH_TYPE") - 1)) option = CURLOPT_TLSAUTH_TYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLSAUTH_USERNAME", sizeof("CURLOPT_TLSAUTH_USERNAME") - 1)) option = CURLOPT_TLSAUTH_USERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_URL", sizeof("CURLOPT_URL") - 1)) option = CURLOPT_URL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_USERAGENT", sizeof("CURLOPT_USERAGENT") - 1)) option = CURLOPT_USERAGENT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_USERNAME", sizeof("CURLOPT_USERNAME") - 1)) option = CURLOPT_USERNAME;
    else ereport(ERROR, (errmsg("unsupported option %s", option_char)));
    if ((res = curl_easy_setopt(curl, option, parameter_char)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %s): %s", option_char, parameter_char, curl_easy_strerror(res))));
ret:
    (void)pfree(option_char);
    (void)pfree(parameter_char);
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_setopt_long); Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *option_char;
    long parameter_long;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument option must not null!")));
    option_char = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument parameter must not null!")));
    parameter_long = PG_GETARG_INT64(1);
    if (false);
    else if (!pg_strncasecmp(option_char, "CURLOPT_APPEND", sizeof("CURLOPT_APPEND") - 1)) option = CURLOPT_APPEND;
    else if (!pg_strncasecmp(option_char, "CURLOPT_AUTOREFERER", sizeof("CURLOPT_AUTOREFERER") - 1)) option = CURLOPT_AUTOREFERER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CONNECTTIMEOUT", sizeof("CURLOPT_CONNECTTIMEOUT") - 1)) option = CURLOPT_CONNECTTIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DIRLISTONLY", sizeof("CURLOPT_DIRLISTONLY") - 1)) option = CURLOPT_DIRLISTONLY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_CACHE_TIMEOUT", sizeof("CURLOPT_DNS_CACHE_TIMEOUT") - 1)) option = CURLOPT_DNS_CACHE_TIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_USE_GLOBAL_CACHE", sizeof("CURLOPT_DNS_USE_GLOBAL_CACHE") - 1)) option = CURLOPT_DNS_USE_GLOBAL_CACHE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FAILONERROR", sizeof("CURLOPT_FAILONERROR") - 1)) option = CURLOPT_FAILONERROR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FILETIME", sizeof("CURLOPT_FILETIME") - 1)) option = CURLOPT_FILETIME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FOLLOWLOCATION", sizeof("CURLOPT_FOLLOWLOCATION") - 1)) option = CURLOPT_FOLLOWLOCATION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FORBID_REUSE", sizeof("CURLOPT_FORBID_REUSE") - 1)) option = CURLOPT_FORBID_REUSE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FRESH_CONNECT", sizeof("CURLOPT_FRESH_CONNECT") - 1)) option = CURLOPT_FRESH_CONNECT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_CREATE_MISSING_DIRS", sizeof("CURLOPT_FTP_CREATE_MISSING_DIRS") - 1)) option = CURLOPT_FTP_CREATE_MISSING_DIRS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_FILEMETHOD", sizeof("CURLOPT_FTP_FILEMETHOD") - 1)) option = CURLOPT_FTP_FILEMETHOD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HEADER", sizeof("CURLOPT_HEADER") - 1)) option = CURLOPT_HEADER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_INFILESIZE", sizeof("CURLOPT_INFILESIZE") - 1)) option = CURLOPT_INFILESIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_IPRESOLVE", sizeof("CURLOPT_IPRESOLVE") - 1)) option = CURLOPT_IPRESOLVE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_KEEP_SENDING_ON_ERROR", sizeof("CURLOPT_KEEP_SENDING_ON_ERROR") - 1)) option = CURLOPT_KEEP_SENDING_ON_ERROR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAXCONNECTS", sizeof("CURLOPT_MAXCONNECTS") - 1)) option = CURLOPT_MAXCONNECTS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAXREDIRS", sizeof("CURLOPT_MAXREDIRS") - 1)) option = CURLOPT_MAXREDIRS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NETRC", sizeof("CURLOPT_NETRC") - 1)) option = CURLOPT_NETRC;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NOBODY", sizeof("CURLOPT_NOBODY") - 1)) option = CURLOPT_NOBODY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_POST", sizeof("CURLOPT_POST") - 1)) option = CURLOPT_POST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYPORT", sizeof("CURLOPT_PROXYPORT") - 1)) option = CURLOPT_PROXYPORT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSLVERSION", sizeof("CURLOPT_PROXY_SSLVERSION") - 1)) option = CURLOPT_PROXY_SSLVERSION;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_PUT", sizeof("CURLOPT_PUT") - 1)) option = CURLOPT_PUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SERVER_RESPONSE_TIMEOUT", sizeof("CURLOPT_SERVER_RESPONSE_TIMEOUT") - 1)) option = CURLOPT_SERVER_RESPONSE_TIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_VERIFYHOST", sizeof("CURLOPT_SSL_VERIFYHOST") - 1)) option = CURLOPT_SSL_VERIFYHOST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_VERIFYPEER", sizeof("CURLOPT_SSL_VERIFYPEER") - 1)) option = CURLOPT_SSL_VERIFYPEER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLVERSION", sizeof("CURLOPT_SSLVERSION") - 1)) option = CURLOPT_SSLVERSION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_KEEPALIVE", sizeof("CURLOPT_TCP_KEEPALIVE") - 1)) option = CURLOPT_TCP_KEEPALIVE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_KEEPIDLE", sizeof("CURLOPT_TCP_KEEPIDLE") - 1)) option = CURLOPT_TCP_KEEPIDLE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TFTP_BLKSIZE", sizeof("CURLOPT_TFTP_BLKSIZE") - 1)) option = CURLOPT_TFTP_BLKSIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TFTP_NO_OPTIONS", sizeof("CURLOPT_TFTP_NO_OPTIONS") - 1)) option = CURLOPT_TFTP_NO_OPTIONS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMECONDITION", sizeof("CURLOPT_TIMECONDITION") - 1)) option = CURLOPT_TIMECONDITION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMEOUT_MS", sizeof("CURLOPT_TIMEOUT_MS") - 1)) option = CURLOPT_TIMEOUT_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMEOUT", sizeof("CURLOPT_TIMEOUT") - 1)) option = CURLOPT_TIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMEVALUE", sizeof("CURLOPT_TIMEVALUE") - 1)) option = CURLOPT_TIMEVALUE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TRANSFER_ENCODING", sizeof("CURLOPT_TRANSFER_ENCODING") - 1)) option = CURLOPT_TRANSFER_ENCODING;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TRANSFERTEXT", sizeof("CURLOPT_TRANSFERTEXT") - 1)) option = CURLOPT_TRANSFERTEXT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_UNRESTRICTED_AUTH", sizeof("CURLOPT_UNRESTRICTED_AUTH") - 1)) option = CURLOPT_UNRESTRICTED_AUTH;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_UPLOAD", sizeof("CURLOPT_UPLOAD") - 1)) option = CURLOPT_UPLOAD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_VERBOSE", sizeof("CURLOPT_VERBOSE") - 1)) option = CURLOPT_VERBOSE;
    else ereport(ERROR, (errmsg("unsupported option %s", option_char)));
    if ((res = curl_easy_setopt(curl, option, parameter_long)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %li): %s", option_char, parameter_long, curl_easy_strerror(res))));
    (void)pfree(option_char);
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
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if ((res = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_NOPROGRESS): %s", curl_easy_strerror(res))));
//    if ((res = curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_PROTOCOLS): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)(&write_buf))) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_WRITEDATA): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_WRITEFUNCTION): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_XFERINFOFUNCTION): %s", curl_easy_strerror(res))));
    if (header && ((res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_HTTPHEADER): %s", curl_easy_strerror(res))));
    if (recipient && ((res = curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipient)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_MAIL_RCPT): %s", curl_easy_strerror(res))));
    if (has_mime && ((res = curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_MIMEPOST): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_perform(curl)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_perform: %s", curl_easy_strerror(res))));
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_easy_getinfo_char(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_getinfo_char); Datum pg_curl_easy_getinfo_char(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    char *info_char;
    char *str = NULL;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("argument info must not null!")));
    info_char = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (false);
    else if (!pg_strncasecmp(info_char, "CURLINFO_CONTENT_TYPE", sizeof("CURLINFO_CONTENT_TYPE") - 1)) info = CURLINFO_CONTENT_TYPE;
    else if (!pg_strncasecmp(info_char, "CURLINFO_RESPONSE", sizeof("CURLINFO_RESPONSE") - 1)) { str = write_buf.data; goto ret; }
    else ereport(ERROR, (errmsg("unsupported option %s", info_char)));
    if ((res = curl_easy_getinfo(curl, info, &str)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_getinfo(%s): %s", info_char, curl_easy_strerror(res))));
ret:
    (void)pfree(info_char);
    if (!str) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(str));
}

Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_getinfo_long); Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    char *info_char;
    long lon;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("argument info must not null!")));
    info_char = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (false);
    else if (!pg_strncasecmp(info_char, "CURLINFO_RESPONSE_CODE", sizeof("CURLINFO_RESPONSE_CODE") - 1)) info = CURLINFO_RESPONSE_CODE;
    else ereport(ERROR, (errmsg("unsupported option %s", info_char)));
    if ((res = curl_easy_getinfo(curl, info, &lon)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_getinfo(%s): %s", info_char, curl_easy_strerror(res))));
    (void)pfree(info_char);
    PG_RETURN_INT64(lon);
}

Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_cleanup); Datum pg_curl_easy_cleanup(PG_FUNCTION_ARGS) {
    if (curl) {
        (void)curl_easy_cleanup(curl);
        curl = NULL;
    }
    (void)curl_mime_free(mime);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
    (void)resetStringInfo(&read_buf);
    (void)resetStringInfo(&write_buf);
    if (encoding) (void)pfree(encoding);
    PG_RETURN_VOID();
}

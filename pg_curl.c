#include <postgres.h>

#include <catalog/pg_type.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#include <signal.h>
#include <utils/builtins.h>

#include <curl/curl.h>

#define EXTENSION(function) Datum (function)(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(function); Datum (function)(PG_FUNCTION_ARGS)

#define D1(...) ereport(DEBUG1, (errmsg(__VA_ARGS__)))
#define D2(...) ereport(DEBUG2, (errmsg(__VA_ARGS__)))
#define D3(...) ereport(DEBUG3, (errmsg(__VA_ARGS__)))
#define D4(...) ereport(DEBUG4, (errmsg(__VA_ARGS__)))
#define D5(...) ereport(DEBUG5, (errmsg(__VA_ARGS__)))
#define E(...) ereport(ERROR, (errmsg(__VA_ARGS__)))
#define F(...) ereport(FATAL, (errmsg(__VA_ARGS__)))
#define I(...) ereport(INFO, (errmsg(__VA_ARGS__)))
#define L(...) ereport(LOG, (errmsg(__VA_ARGS__)))
#define N(...) ereport(NOTICE, (errmsg(__VA_ARGS__)))
#define W(...) ereport(WARNING, (errmsg(__VA_ARGS__)))

PG_MODULE_MAGIC;

typedef struct MemoryStreamString {
    bool use;
    char *data;
    FILE *file;
    size_t len;
} MemoryStreamString;

#if CURL_AT_LEAST_VERSION(7, 56, 0)
static bool has_mime;
#endif
static CURL *curl = NULL;
#if CURL_AT_LEAST_VERSION(7, 56, 0)
static curl_mime *mime;
#endif
static FILE *read_str = NULL;
static int pg_curl_interrupt_requested = 0;
static MemoryStreamString data_in_str = {0};
static MemoryStreamString data_out_str = {0};
static MemoryStreamString debug_str = {0};
static MemoryStreamString header_in_str = {0};
static MemoryStreamString header_out_str = {0};
static pqsigfunc pgsql_interrupt_handler = NULL;
static struct curl_slist *header = NULL;
#if CURL_AT_LEAST_VERSION(7, 20, 0)
static struct curl_slist *recipient = NULL;
#endif

static void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }

static void freeMemoryStreamString(MemoryStreamString *buf) {
    if (buf->data) free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

static void initMemoryStreamString(MemoryStreamString *buf) {
    freeMemoryStreamString(buf);
    if (!(buf->file = open_memstream(&buf->data, &buf->len))) E("!open_memstream");
}

static void free_file(FILE **file) {
    if (*file) {
        fclose(*file);
        *file = NULL;
    }
}

static void init_file(FILE **file, void *buf, size_t size) {
    free_file(file);
    if (!(*file = fmemopen(buf, size, "rb"))) E("!fmemopen");
}

void _PG_init(void); void _PG_init(void) {
#if CURL_AT_LEAST_VERSION(7, 8, 0)
    if (curl_global_init(CURL_GLOBAL_ALL)) E("curl_global_init");
#endif
    if (!(curl = curl_easy_init())) E("!curl_easy_init");
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    if (!(curl_mime_init(curl))) E("!curl_mime_init");
    has_mime = false;
#endif
    pg_curl_interrupt_requested = 0;
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
}

void _PG_fini(void); void _PG_fini(void) {
    pqsignal(SIGINT, pgsql_interrupt_handler);
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    curl_mime_free(mime);
#endif
    curl_slist_free_all(header);
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    curl_slist_free_all(recipient);
#endif
    curl_easy_cleanup(curl);
#if CURL_AT_LEAST_VERSION(7, 8, 0)
    curl_global_cleanup();
#endif
    freeMemoryStreamString(&data_in_str);
    freeMemoryStreamString(&data_out_str);
    freeMemoryStreamString(&debug_str);
    freeMemoryStreamString(&header_in_str);
    freeMemoryStreamString(&header_out_str);
    free_file(&read_str);
}

EXTENSION(pg_curl_easy_header_reset) {
    curl_slist_free_all(header);
    header = NULL;
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_mime_reset) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    curl_mime_free(mime);
    if (!(mime = curl_mime_init(curl))) E("!curl_mime_init");
    has_mime = false;
    PG_RETURN_VOID();
#else
    E("curl_easy_mime_reset requires curl 7.56.0 or later");
#endif
}

EXTENSION(pg_curl_easy_readdata_reset) {
    free_file(&read_str);
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_recipient_reset) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    curl_slist_free_all(recipient);
    recipient = NULL;
    PG_RETURN_VOID();
#else
    E("curl_easy_recipient_reset requires curl 7.20.0 or later");
#endif
}

EXTENSION(pg_curl_easy_reset) {
    pg_curl_easy_header_reset(fcinfo);
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    pg_curl_easy_mime_reset(fcinfo);
#endif
    pg_curl_easy_readdata_reset(fcinfo);
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    pg_curl_easy_recipient_reset(fcinfo);
#endif
#if CURL_AT_LEAST_VERSION(7, 12, 1)
    curl_easy_reset(curl);
#endif
    freeMemoryStreamString(&data_in_str);
    freeMemoryStreamString(&data_out_str);
    freeMemoryStreamString(&debug_str);
    freeMemoryStreamString(&header_in_str);
    freeMemoryStreamString(&header_out_str);
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_escape) {
#if CURL_AT_LEAST_VERSION(7, 15, 4)
    text *string;
    char *escape;
    if (PG_ARGISNULL(0)) E("string is null!");
    string = DatumGetTextP(PG_GETARG_DATUM(0));
    if (!(escape = curl_easy_escape(curl, VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string)))) E("!curl_easy_escape");
    string = cstring_to_text(escape);
    curl_free(escape);
    PG_RETURN_TEXT_P(string);
#else
    E("curl_easy_escape requires curl 7.15.4 or later");
#endif
}

EXTENSION(pg_curl_easy_unescape) {
#if CURL_AT_LEAST_VERSION(7, 15, 4)
    text *url;
    char *unescape;
    int outlength;
    if (PG_ARGISNULL(0)) E("url is null!");
    url = DatumGetTextP(PG_GETARG_DATUM(0));
    if (!(unescape = curl_easy_unescape(curl, VARDATA_ANY(url), VARSIZE_ANY_EXHDR(url), &outlength))) PG_RETURN_NULL();
    url = cstring_to_text_with_len(unescape, outlength);
    curl_free(unescape);
    PG_RETURN_TEXT_P(url);
#else
    E("curl_easy_unescape requires curl 7.15.4 or later");
#endif
}

EXTENSION(pg_curl_header_append) {
    char *name, *value;
    StringInfoData buf;
    struct curl_slist *temp = header;
    if (PG_ARGISNULL(0)) E("name is null!");
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) E("value is null!");
    value = TextDatumGetCString(PG_GETARG_DATUM(1));
    initStringInfo(&buf);
    appendStringInfo(&buf, "%s: %s", name, value);
    if ((temp = curl_slist_append(temp, buf.data))) header = temp; else E("!curl_slist_append");
    pfree(name);
    pfree(value);
    pfree(buf.data);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_recipient_append) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    char *email;
    struct curl_slist *temp = recipient;
    if (PG_ARGISNULL(0)) E("email is null!");
    email = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, email))) recipient = temp; else E("!curl_slist_append");
    pfree(email);
    PG_RETURN_BOOL(temp != NULL);
#else
    E("curl_recipient_append requires curl 7.20.0 or later");
#endif
}

EXTENSION(pg_curl_mime_data) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    char *name = NULL, *file = NULL, *type = NULL, *code = NULL, *head = NULL;
    CURLcode res = CURL_LAST;
    curl_mimepart *part;
    text *data;
    if (PG_ARGISNULL(0)) E("data is null!");
    data = DatumGetTextP(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) file = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) type = TextDatumGetCString(PG_GETARG_DATUM(3));
    if (!PG_ARGISNULL(4)) code = TextDatumGetCString(PG_GETARG_DATUM(4));
    if (!PG_ARGISNULL(5)) head = TextDatumGetCString(PG_GETARG_DATUM(5));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data))) != CURLE_OK) E("curl_mime_data(%s): %s", VARDATA_ANY(data), curl_easy_strerror(res));
    if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) E("curl_mime_name(%s): %s", name, curl_easy_strerror(res));
    if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) E("curl_mime_filename(%s): %s", file, curl_easy_strerror(res));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) E("curl_mime_type(%s): %s", type, curl_easy_strerror(res));
    if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) E("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res));
    if (head) {
        struct curl_slist *headers = NULL;
        if (!(headers = curl_slist_append(headers, head))) E("!curl_slist_append");
        if ((res = curl_mime_headers(part, headers, true)) != CURLE_OK) E("curl_mime_headers(%s): %s", head, curl_easy_strerror(res));
    }
    if (name) pfree(name);
    if (file) pfree(file);
    if (type) pfree(type);
    if (code) pfree(code);
    if (head) pfree(head);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
#else
    E("curl_mime_data requires curl 7.56.0 or later");
#endif
}

EXTENSION(pg_curl_mime_data_bytea) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    CURLcode res = CURL_LAST;
    bytea *data;
    char *name = NULL, *file = NULL, *type = NULL, *code = NULL, *head = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) E("data is null!");
    data = DatumGetByteaP(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) file = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) type = TextDatumGetCString(PG_GETARG_DATUM(3));
    if (!PG_ARGISNULL(4)) code = TextDatumGetCString(PG_GETARG_DATUM(4));
    if (!PG_ARGISNULL(5)) head = TextDatumGetCString(PG_GETARG_DATUM(5));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data))) != CURLE_OK) E("curl_mime_data(%s): %s", VARDATA_ANY(data), curl_easy_strerror(res));
    if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) E("curl_mime_name(%s): %s", name, curl_easy_strerror(res));
    if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) E("curl_mime_filename(%s): %s", file, curl_easy_strerror(res));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) E("curl_mime_type(%s): %s", type, curl_easy_strerror(res));
    if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) E("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res));
    if (head) {
        struct curl_slist *headers = NULL;
        if (!(headers = curl_slist_append(headers, head))) E("!curl_slist_append");
        if ((res = curl_mime_headers(part, headers, true)) != CURLE_OK) E("curl_mime_headers(%s): %s", head, curl_easy_strerror(res));
    }
    if (name) pfree(name);
    if (file) pfree(file);
    if (type) pfree(type);
    if (code) pfree(code);
    if (head) pfree(head);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
#else
    E("curl_mime_data requires curl 7.56.0 or later");
#endif
}

EXTENSION(pg_curl_mime_file) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    CURLcode res = CURL_LAST;
    char *data, *name = NULL, *file = NULL, *type = NULL, *code = NULL, *head = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) E("data is null!");
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) file = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) type = TextDatumGetCString(PG_GETARG_DATUM(3));
    if (!PG_ARGISNULL(4)) code = TextDatumGetCString(PG_GETARG_DATUM(4));
    if (!PG_ARGISNULL(5)) head = TextDatumGetCString(PG_GETARG_DATUM(5));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_filedata(part, data)) != CURLE_OK) E("curl_mime_filedata(%s): %s", data, curl_easy_strerror(res));
    if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) E("curl_mime_name(%s): %s", name, curl_easy_strerror(res));
    if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) E("curl_mime_filename(%s): %s", file, curl_easy_strerror(res));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) E("curl_mime_type(%s): %s", type, curl_easy_strerror(res));
    if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) E("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res));
    if (head) {
        struct curl_slist *headers = NULL;
        if (!(headers = curl_slist_append(headers, head))) E("!curl_slist_append");
        if ((res = curl_mime_headers(part, headers, true)) != CURLE_OK) E("curl_mime_headers(%s): %s", head, curl_easy_strerror(res));
    }
    pfree(data);
    if (name) pfree(name);
    if (file) pfree(file);
    if (type) pfree(type);
    if (code) pfree(code);
    if (head) pfree(head);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
#else
    E("curl_mime_file requires curl 7.56.0 or later");
#endif
}

EXTENSION(pg_curl_easy_setopt_copypostfields) {
#if CURL_AT_LEAST_VERSION(7, 17, 1)
    CURLcode res = CURL_LAST;
    bytea *parameter;
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = DatumGetTextP(PG_GETARG_DATUM(0));
    if ((res = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, VARSIZE_ANY_EXHDR(parameter))) != CURLE_OK) E("curl_easy_setopt(CURLOPT_POSTFIELDSIZE): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, VARDATA_ANY(parameter))) != CURLE_OK) E("curl_easy_setopt(CURLOPT_COPYPOSTFIELDS): %s", curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
#else
    E("curl_easy_setopt_copypostfields requires curl 7.17.1 or later");
#endif
}

EXTENSION(pg_curl_easy_setopt_readdata) {
#if CURL_AT_LEAST_VERSION(7, 9, 7)
    CURLcode res = CURL_LAST;
    bytea *parameter;
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = DatumGetTextP(PG_GETARG_DATUM(0));
    if ((res = curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_UPLOAD): %s", curl_easy_strerror(res));
#if CURL_AT_LEAST_VERSION(7, 23, 0)
    if ((res = curl_easy_setopt(curl, CURLOPT_INFILESIZE, VARSIZE_ANY_EXHDR(parameter))) != CURLE_OK) E("curl_easy_setopt(CURLOPT_INFILESIZE): %s", curl_easy_strerror(res));
#endif
    init_file(&read_str, VARDATA_ANY(parameter), VARSIZE_ANY_EXHDR(parameter));
    if ((res = curl_easy_setopt(curl, CURLOPT_READDATA, read_str)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_READDATA): %s", curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
#else
    E("curl_easy_setopt_readdata requires curl 7.9.7 or later");
#endif
}

static Datum pg_curl_easy_setopt_char(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURL_LAST;
    char *parameter;
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((res = curl_easy_setopt(curl, option, parameter)) != CURLE_OK) E("curl_easy_setopt(%i, %s): %s", option, parameter, curl_easy_strerror(res));
    pfree(parameter);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_abstract_unix_socket) {
#if CURL_AT_LEAST_VERSION(7, 53, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ABSTRACT_UNIX_SOCKET);
#else
    E("curl_easy_setopt_abstract_unix_socket requires curl 7.53.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_accept_encoding) {
#if CURL_AT_LEAST_VERSION(7, 21, 6)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ACCEPT_ENCODING);
#else
    E("curl_easy_setopt_accept_encoding requires curl 7.21.6 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_cainfo) {
#if CURL_AT_LEAST_VERSION(7, 60, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CAINFO);
#else
    E("curl_easy_setopt_cainfo requires curl 7.60.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_capath) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CAPATH);
#else
    E("curl_easy_setopt_capath requires curl 7.56.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_cookiefile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIEFILE); }
EXTENSION(pg_curl_easy_setopt_cookiejar) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIEJAR); }
EXTENSION(pg_curl_easy_setopt_cookielist) {
#if CURL_AT_LEAST_VERSION(7, 14, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIELIST);
#else
    E("curl_easy_setopt_cookielist requires curl 7.14.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_cookie) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIE); }
EXTENSION(pg_curl_easy_setopt_crlfile) {
#if CURL_AT_LEAST_VERSION(7, 19, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CRLFILE);
#else
    E("curl_easy_setopt_crlfile requires curl 7.19.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_customrequest) {
#if CURL_AT_LEAST_VERSION(7, 26, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CUSTOMREQUEST);
#else
    E("curl_easy_setopt_customrequest requires curl 7.26.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_default_protocol) {
#if CURL_AT_LEAST_VERSION(7, 45, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DEFAULT_PROTOCOL);
#else
    E("curl_easy_setopt_default_protocol requires curl 7.45.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_interface) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_INTERFACE);
#else
    E("curl_easy_setopt_dns_interface requires curl 7.33.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_local_ip4) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_LOCAL_IP4);
#else
    E("curl_easy_setopt_dns_local_ip4 requires curl 7.33.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_local_ip6) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_LOCAL_IP6);
#else
    E("curl_easy_setopt_dns_local_ip6 requires curl 7.33.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_servers) {
#if CURL_AT_LEAST_VERSION(7, 24, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_SERVERS);
#else
    E("url_easy_setopt_dns_servers requires curl 7.24.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_doh_url) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DOH_URL);
#else
    E("curl_easy_setopt_doh_url requires curl 7.62.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_egdsocket) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_EGDSOCKET); }
EXTENSION(pg_curl_easy_setopt_ftp_account) {
#if CURL_AT_LEAST_VERSION(7, 13, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTP_ACCOUNT);
#else
    E("curl_easy_setopt_ftp_account requires curl 7.13.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_alternative_to_user) {
#if CURL_AT_LEAST_VERSION(7, 15, 5)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTP_ALTERNATIVE_TO_USER);
#else
    E("curl_easy_setopt_ftp_alternative_to_user requires curl 7.15.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftpport) {
#if CURL_AT_LEAST_VERSION(7, 19, 5)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTPPORT);
#else
    E("curl_easy_setopt_ftpport requires curl 7.19.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_interface) {
#if CURL_AT_LEAST_VERSION(7, 24, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_INTERFACE);
#else
    E("curl_easy_setopt_interface requires curl 7.24.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_issuercert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ISSUERCERT); }
EXTENSION(pg_curl_easy_setopt_keypasswd) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_KEYPASSWD);
#else
    E("curl_easy_setopt_keypasswd requires curl 7.16.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_krblevel) {
#if CURL_AT_LEAST_VERSION(7, 16, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_KRBLEVEL);
#else
    E("curl_easy_setopt_krblevel requires curl 7.16.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_login_options) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_LOGIN_OPTIONS);
#else
    E("curl_easy_setopt_login_options requires curl 7.34.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_mail_auth) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_MAIL_AUTH);
#else
    E("curl_easy_setopt_mail_auth requires curl 7.25.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_mail_from) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_MAIL_FROM);
#else
    E("curl_easy_setopt_mail_from requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_noproxy) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_NOPROXY);
#else
    E("curl_easy_setopt_noproxy requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_password) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PASSWORD);
#else
    E("curl_easy_setopt_password requires curl 7.19.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_pinnedpublickey) {
#if CURL_AT_LEAST_VERSION(7, 39, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PINNEDPUBLICKEY);
#else
    E("curl_easy_setopt_pinnedpublickey requires curl 7.39.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_pre_proxy) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PRE_PROXY);
#else
    E("curl_easy_setopt_pre_proxy requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_cainfo) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CAINFO);
#else
    E("curl_easy_setopt_proxy_cainfo requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_capath) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CAPATH);
#else
    E("curl_easy_setopt_proxy_capath requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_crlfile) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CRLFILE);
#else
    E("curl_easy_setopt_proxy_crlfile requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_keypasswd) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_KEYPASSWD);
#else
    E("curl_easy_setopt_proxy_keypasswd requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxypassword) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYPASSWORD);
#else
    E("curl_easy_setopt_proxypassword requires curl 7.19.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_pinnedpublickey) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_PINNEDPUBLICKEY);
#else
    E("curl_easy_setopt_proxy_pinnedpublickey requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_service_name) {
#if CURL_AT_LEAST_VERSION(7, 43, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SERVICE_NAME);
#else
    E("curl_easy_setopt_proxy_service_name requires curl 7.43.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy) {
#if CURL_AT_LEAST_VERSION(7, 14, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY);
#else
    E("curl_easy_setopt_proxy requires curl 7.14.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslcert) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLCERT);
#else
    E("curl_easy_setopt_proxy_sslcert requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslcerttype) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLCERTTYPE);
#else
    E("curl_easy_setopt_proxy_sslcerttype requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_ssl_cipher_list) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSL_CIPHER_LIST);
#else
    E("curl_easy_setopt_proxy_ssl_cipher_list requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslkey) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLKEY);
#else
    E("curl_easy_setopt_proxy_sslkey requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslkeytype) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLKEYTYPE);
#else
    E("curl_easy_setopt_proxy_sslkeytype requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tls13_ciphers) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLS13_CIPHERS);
#else
    E("curl_easy_setopt_proxy_tls13_ciphers requires curl 7.61.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_password) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_PASSWORD);
#else
    E("curl_easy_setopt_proxy_tlsauth_password requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_type) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_TYPE);
#else
    E("curl_easy_setopt_proxy_tlsauth_type requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_username) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_USERNAME);
#else
    E("curl_easy_setopt_proxy_tlsauth_username requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyusername) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYUSERNAME);
#else
    E("curl_easy_setopt_proxyusername requires curl 7.19.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyuserpwd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYUSERPWD); }
EXTENSION(pg_curl_easy_setopt_random_file) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RANDOM_FILE); }
EXTENSION(pg_curl_easy_setopt_range) {
#if CURL_AT_LEAST_VERSION(7, 18, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RANGE);
#else
    E("curl_easy_setopt_range requires curl 7.18.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_referer) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REFERER); }
EXTENSION(pg_curl_easy_setopt_request_target) {
#if CURL_AT_LEAST_VERSION(7, 55, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REQUEST_TARGET);
#else
    E("curl_easy_setopt_request_target requires curl 7.55.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_session_id) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_SESSION_ID);
#else
    E("curl_easy_setopt_rtsp_session_id requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_stream_uri) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_STREAM_URI);
#else
    E("curl_easy_setopt_rtsp_stream_uri requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_transport) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_TRANSPORT);
#else
    E("curl_easy_setopt_rtsp_transport requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_service_name) {
#if CURL_AT_LEAST_VERSION(7, 43, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SERVICE_NAME);
#else
    E("curl_easy_setopt_service_name requires curl 7.43.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_socks5_gssapi_service) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    W("curl_easy_setopt_socks5_gssapi_service deprecated, use curl_easy_setopt_proxy_service_name instead");
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SOCKS5_GSSAPI_SERVICE);
#else
    E("curl_easy_setopt_socks5_gssapi_service requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_host_public_key_md5) {
#if CURL_AT_LEAST_VERSION(7, 17, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_HOST_PUBLIC_KEY_MD5);
#else
    E("curl_easy_setopt_ssh_host_public_key_md5 requires curl 7.17.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_knownhosts) {
#if CURL_AT_LEAST_VERSION(7, 19, 6)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_KNOWNHOSTS);
#else
    E("curl_easy_setopt_ssh_knownhosts requires curl 7.19.6 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_private_keyfile) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_PRIVATE_KEYFILE);
#else
    E("curl_easy_setopt_ssh_private_keyfile requires curl 7.16.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_public_keyfile) {
#if CURL_AT_LEAST_VERSION(7, 26, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_PUBLIC_KEYFILE);
#else
    E("curl_easy_setopt_ssh_public_keyfile requires curl 7.26.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_sslcert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLCERT); }
EXTENSION(pg_curl_easy_setopt_sslcerttype) {
#if CURL_AT_LEAST_VERSION(7, 9, 3)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLCERTTYPE);
#else
    E("curl_easy_setopt_sslcerttype requires curl 7.9.3 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_cipher_list) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSL_CIPHER_LIST); }
EXTENSION(pg_curl_easy_setopt_sslengine) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLENGINE); }
EXTENSION(pg_curl_easy_setopt_sslkey) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLKEY); }
EXTENSION(pg_curl_easy_setopt_sslkeytype) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLKEYTYPE); }
EXTENSION(pg_curl_easy_setopt_tls13_ciphers) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLS13_CIPHERS);
#else
    E("curl_easy_setopt_tls13_ciphers requires curl 7.61.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tlsauth_password) {
#if CURL_AT_LEAST_VERSION(7, 21, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_PASSWORD);
#else
    E("curl_easy_setopt_tlsauth_password requires curl 7.21.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tlsauth_type) {
#if CURL_AT_LEAST_VERSION(7, 21, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_TYPE);
#else
    E("curl_easy_setopt_tlsauth_type requires curl 7.21.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tlsauth_username) {
#if CURL_AT_LEAST_VERSION(7, 21, 4)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_USERNAME);
#else
    E("curl_easy_setopt_tlsauth_username requires curl 7.21.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_unix_socket_path) {
#if CURL_AT_LEAST_VERSION(7, 40, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_UNIX_SOCKET_PATH);
#else
    E("curl_easy_setopt_unix_socket_path requires curl 7.40.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_url) {
#if CURL_AT_LEAST_VERSION(7, 31, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_URL);
#else
    E("curl_easy_setopt_url requires curl 7.31.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_useragent) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERAGENT); }
EXTENSION(pg_curl_easy_setopt_username) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERNAME);
#else
    E("curl_easy_setopt_username requires curl 7.19.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_userpwd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERPWD); }
EXTENSION(pg_curl_easy_setopt_xoauth2_bearer) {
#if CURL_AT_LEAST_VERSION(7, 33, 0)
    return pg_curl_easy_setopt_char(fcinfo, CURLOPT_XOAUTH2_BEARER);
#else
    E("curl_easy_setopt_xoauth2_bearer requires curl 7.33.0 or later");
#endif
}

static Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURL_LAST;
    long parameter;
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = PG_GETARG_INT64(0);
    if ((res = curl_easy_setopt(curl, option, parameter)) != CURLE_OK) E("curl_easy_setopt(%i, %li): %s", option, parameter, curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_accepttimeout_ms) {
#if CURL_AT_LEAST_VERSION(7, 24, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_ACCEPTTIMEOUT_MS);
#else
    E("curl_easy_setopt_accepttimeout_ms requires curl 7.24.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_address_scope) {
#if CURL_AT_LEAST_VERSION(7, 19, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_ADDRESS_SCOPE);
#else
    E("curl_easy_setopt_address_scope requires curl 7.19.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_append) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_APPEND);
#else
    E("curl_easy_setopt_append requires curl 7.16.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_autoreferer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_AUTOREFERER); }
EXTENSION(pg_curl_easy_setopt_buffersize) {
#if CURL_AT_LEAST_VERSION(7, 10, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_BUFFERSIZE);
#else
    E("curl_easy_setopt_buffersize requires curl 7.10.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_certinfo) {
#if CURL_AT_LEAST_VERSION(7, 50, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CERTINFO);
#else
    E("curl_easy_setopt_certinfo requires curl 7.50.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_connect_only) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECT_ONLY);
#else
    E("curl_easy_setopt_connect_only requires curl 7.15.2 or later");
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
    E("curl_easy_setopt_dirlistonly requires curl 7.16.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_disallow_username_in_url) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DISALLOW_USERNAME_IN_URL);
#else
    E("curl_easy_setopt_disallow_username_in_url requires curl 7.61.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_cache_timeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_CACHE_TIMEOUT); }
EXTENSION(pg_curl_easy_setopt_dns_shuffle_addresses) {
#if CURL_AT_LEAST_VERSION(7, 60, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_SHUFFLE_ADDRESSES);
#else
    E("curl_easy_setopt_dns_shuffle_addresses requires curl 7.60.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_dns_use_global_cache) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    W("curl_easy_setopt_dns_use_global_cache deprecated");
#endif
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_USE_GLOBAL_CACHE);
}
EXTENSION(pg_curl_easy_setopt_expect_100_timeout_ms) {
#if CURL_AT_LEAST_VERSION(7, 36, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_EXPECT_100_TIMEOUT_MS);
#else
    E("curl_easy_setopt_expect_100_timeout_ms requires curl 7.36.0 or later");
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
    E("curl_easy_setopt_ftp_create_missing_dirs requires curl 7.10.7 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_filemethod) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_FILEMETHOD);
#else
    E("curl_easy_setopt_ftp_filemethod requires curl 7.15.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_skip_pasv_ip) {
#if CURL_AT_LEAST_VERSION(7, 14, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_SKIP_PASV_IP);
#else
    E("curl_easy_setopt_ftp_skip_pasv_ip requires curl 7.14.2 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftpsslauth) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTPSSLAUTH);
#else
    E("curl_easy_setopt_ftpsslauth requires curl 7.12.2 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_ssl_ccc) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_SSL_CCC);
#else
    E("curl_easy_setopt_ftp_ssl_ccc requires curl 7.16.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_use_eprt) {
#if CURL_AT_LEAST_VERSION(7, 10, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_EPRT);
#else
    E("curl_easy_setopt_ftp_use_eprt requires curl 7.10.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ftp_use_epsv) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_EPSV); }
EXTENSION(pg_curl_easy_setopt_ftp_use_pret) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_PRET);
#else
    E("curl_easy_setopt_ftp_use_pret requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_gssapi_delegation) {
#if CURL_AT_LEAST_VERSION(7, 22, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_GSSAPI_DELEGATION);
#else
    E("curl_easy_setopt_gssapi_delegation requires curl 7.22.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_happy_eyeballs_timeout_ms) {
#if CURL_AT_LEAST_VERSION(7, 59, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS);
#else
    E("curl_easy_setopt_happy_eyeballs_timeout_ms requires curl 7.59.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_haproxyprotocol) {
#if CURL_AT_LEAST_VERSION(7, 60, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HAPROXYPROTOCOL);
#else
    E("curl_easy_setopt_haproxyprotocol requires curl 7.60.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_header) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HEADER); }
EXTENSION(pg_curl_easy_setopt_http09_allowed) {
#if CURL_AT_LEAST_VERSION(7, 64, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP09_ALLOWED);
#else
    E("curl_easy_setopt_http09_allowed requires curl 7.64.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_httpauth) {
#if CURL_AT_LEAST_VERSION(7, 10, 6)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPAUTH);
#else
    E("curl_easy_setopt_httpauth requires curl 7.10.6 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_http_content_decoding) {
#if CURL_AT_LEAST_VERSION(7, 16, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_CONTENT_DECODING);
#else
    E("curl_easy_setopt_http_content_decoding requires curl 7.16.2 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_httpget) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPGET); }
EXTENSION(pg_curl_easy_setopt_httpproxytunnel) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPPROXYTUNNEL); }
EXTENSION(pg_curl_easy_setopt_http_transfer_decoding) {
#if CURL_AT_LEAST_VERSION(7, 16, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_TRANSFER_DECODING);
#else
    E("curl_easy_setopt_http_transfer_decoding requires curl 7.16.2 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_http_version) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_VERSION); }
EXTENSION(pg_curl_easy_setopt_ignore_content_length) {
#if CURL_AT_LEAST_VERSION(7, 14, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_IGNORE_CONTENT_LENGTH);
#else
    E("curl_easy_setopt_ignore_content_length requires curl 7.14.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ipresolve) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_IPRESOLVE); }
EXTENSION(pg_curl_easy_setopt_keep_sending_on_error) {
#if CURL_AT_LEAST_VERSION(7, 51, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_KEEP_SENDING_ON_ERROR);
#else
    E("curl_easy_setopt_keep_sending_on_error requires curl 7.51.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_localportrange) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOCALPORTRANGE);
#else
    E("curl_easy_setopt_localportrange requires curl 7.15.2 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_localport) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOCALPORT);
#else
    E("curl_easy_setopt_localport requires curl 7.15.2 or later");
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
    E("curl_easy_setopt_new_directory_perms requires curl 7.16.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_new_file_perms) {
#if CURL_AT_LEAST_VERSION(7, 16, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NEW_FILE_PERMS);
#else
    E("curl_easy_setopt_new_file_perms requires curl 7.16.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_nobody) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NOBODY); }
EXTENSION(pg_curl_easy_setopt_nosignal) {
#if CURL_AT_LEAST_VERSION(7, 10, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NOSIGNAL);
#else
    E("curl_easy_setopt_nosignal requires curl 7.10.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_path_as_is) {
#if CURL_AT_LEAST_VERSION(7, 42, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PATH_AS_IS);
#else
    E("curl_easy_setopt_path_as_is requires curl 7.42.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_pipewait) {
#if CURL_AT_LEAST_VERSION(7, 34, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PIPEWAIT);
#else
    E("curl_easy_setopt_pipewait requires curl 7.34.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_port) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PORT); }
EXTENSION(pg_curl_easy_setopt_postredir) {
#if CURL_AT_LEAST_VERSION(7, 19, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_POSTREDIR);
#else
    E("curl_easy_setopt_postredir requires curl 7.19.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_post) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_POST); }
EXTENSION(pg_curl_easy_setopt_protocols) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROTOCOLS);
#else
    E("curl_easy_setopt_protocols requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyauth) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYAUTH);
#else
    E("curl_easy_setopt_proxyauth requires curl 7.10.7 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxyport) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYPORT); }
EXTENSION(pg_curl_easy_setopt_proxy_ssl_options) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_OPTIONS);
#else
    E("curl_easy_setopt_proxy_ssl_options requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_ssl_verifyhost) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_VERIFYHOST);
#else
    E("curl_easy_setopt_proxy_ssl_verifyhost requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_ssl_verifypeer) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_VERIFYPEER);
#else
    E("curl_easy_setopt_proxy_ssl_verifypeer requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_sslversion) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSLVERSION);
#else
    E("curl_easy_setopt_proxy_sslversion requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxy_transfer_mode) {
#if CURL_AT_LEAST_VERSION(7, 18, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_TRANSFER_MODE);
#else
    E("curl_easy_setopt_proxy_transfer_mode requires curl 7.18.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_proxytype) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYTYPE); }
EXTENSION(pg_curl_easy_setopt_put) {
#if CURL_AT_LEAST_VERSION(7, 12, 1)
    W("curl_easy_setopt_put deprecated");
#endif
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PUT);
}
EXTENSION(pg_curl_easy_setopt_redir_protocols) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_REDIR_PROTOCOLS);
#else
    E("curl_easy_setopt_redir_protocols requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_resume_from) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RESUME_FROM); }
EXTENSION(pg_curl_easy_setopt_rtsp_client_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_CLIENT_CSEQ);
#else
    E("curl_easy_setopt_rtsp_client_cseq requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_request) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_REQUEST);
#else
    E("curl_easy_setopt_rtsp_request requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_rtsp_server_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_SERVER_CSEQ);
#else
    E("curl_easy_setopt_rtsp_server_cseq requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_sasl_ir) {
#if CURL_AT_LEAST_VERSION(7, 31, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SASL_IR);
#else
    E("curl_easy_setopt_sasl_ir requires curl 7.31.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_server_response_timeout) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SERVER_RESPONSE_TIMEOUT);
#else
    E("curl_easy_setopt_server_response_timeout requires curl 7.10.8 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_socks5_auth) {
#if CURL_AT_LEAST_VERSION(7, 55, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SOCKS5_AUTH);
#else
    E("curl_easy_setopt_socks5_auth requires curl 7.55.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_socks5_gssapi_nec) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SOCKS5_GSSAPI_NEC);
#else
    E("curl_easy_setopt_socks5_gssapi_nec requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_auth_types) {
#if CURL_AT_LEAST_VERSION(7, 16, 1)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSH_AUTH_TYPES);
#else
    E("curl_easy_setopt_ssh_auth_types requires curl 7.16.1 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssh_compression) {
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSH_COMPRESSION);
#else
    E("curl_easy_setopt_ssh_compression requires curl 7.56.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_enable_alpn) {
#if CURL_AT_LEAST_VERSION(7, 36, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_ENABLE_ALPN);
#else
    E("curl_easy_setopt_ssl_enable_alpn requires curl 7.36.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_enable_npn) {
#if CURL_AT_LEAST_VERSION(7, 36, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_ENABLE_NPN);
#else
    E("curl_easy_setopt_ssl_enable_npn requires curl 7.36.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_falsestart) {
#if CURL_AT_LEAST_VERSION(7, 42, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_FALSESTART);
#else
    E("curl_easy_setopt_ssl_falsestart requires curl 7.42.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_options) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_OPTIONS);
#else
    E("curl_easy_setopt_ssl_options requires curl 7.25.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_sessionid_cache) {
#if CURL_AT_LEAST_VERSION(7, 16, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_SESSIONID_CACHE);
#else
    E("curl_easy_setopt_ssl_sessionid_cache requires curl 7.16.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_ssl_verifyhost) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYHOST); }
EXTENSION(pg_curl_easy_setopt_ssl_verifypeer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYPEER); }
EXTENSION(pg_curl_easy_setopt_ssl_verifystatus) {
#if CURL_AT_LEAST_VERSION(7, 41, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYSTATUS);
#else
    E("curl_easy_setopt_ssl_verifystatus requires curl 7.41.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_sslversion) {
#if CURL_AT_LEAST_VERSION(7, 18, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSLVERSION);
#else
    E("curl_easy_setopt_sslversion requires curl 7.18.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_stream_weight) {
#if CURL_AT_LEAST_VERSION(7, 46, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_STREAM_WEIGHT);
#else
    E("curl_easy_setopt_stream_weight requires curl 7.46.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_suppress_connect_headers) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SUPPRESS_CONNECT_HEADERS);
#else
    E("curl_easy_setopt_suppress_connect_headers requires curl 7.54.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_fastopen) {
#if CURL_AT_LEAST_VERSION(7, 49, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_FASTOPEN);
#else
    E("curl_easy_setopt_tcp_fastopen requires curl 7.49.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_keepalive) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPALIVE);
#else
    E("curl_easy_setopt_tcp_keepalive requires curl 7.25.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_keepidle) {
#if CURL_AT_LEAST_VERSION(7, 25, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPIDLE);
#else
    E("curl_easy_setopt_tcp_keepidle requires curl 7.25.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tcp_keepintvl) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPINTVL); }
EXTENSION(pg_curl_easy_setopt_tcp_nodelay) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_NODELAY); }
EXTENSION(pg_curl_easy_setopt_tftp_blksize) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TFTP_BLKSIZE);
#else
    E("curl_easy_setopt_tftp_blksize requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_tftp_no_options) {
#if CURL_AT_LEAST_VERSION(7, 48, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TFTP_NO_OPTIONS);
#else
    E("curl_easy_setopt_tftp_no_options requires curl 7.48.0 or later");
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
    E("curl_easy_setopt_transfer_encoding requires curl 7.21.6 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_transfertext) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TRANSFERTEXT); }
EXTENSION(pg_curl_easy_setopt_unrestricted_auth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UNRESTRICTED_AUTH); }
EXTENSION(pg_curl_easy_setopt_upkeep_interval_ms) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPKEEP_INTERVAL_MS);
#else
    E("curl_easy_setopt_upkeep_interval_ms requires curl 7.62.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_upload_buffersize) {
#if CURL_AT_LEAST_VERSION(7, 62, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPLOAD_BUFFERSIZE);
#else
    E("curl_easy_setopt_upload_buffersize requires curl 7.62.0 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_use_ssl) {
#if CURL_AT_LEAST_VERSION(7, 16, 5)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_USE_SSL);
#else
    E("curl_easy_setopt_use_ssl requires curl 7.16.5 or later");
#endif
}
EXTENSION(pg_curl_easy_setopt_verbose) {
    W("curl_easy_setopt_verbose deprecated, use curl_easy_perform(debug:=true) and curl_easy_getinfo_debug() instead");
    PG_RETURN_BOOL(false);
}
EXTENSION(pg_curl_easy_setopt_wildcardmatch) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_setopt_long(fcinfo, CURLOPT_WILDCARDMATCH);
#else
    E("curl_easy_setopt_wildcardmatch requires curl 7.21.0 or later");
#endif
}

static int debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    if (size) switch (type) {
        case CURLINFO_DATA_IN: if (data_in_str.use && fwrite(data, size, 1, data_in_str.file) != size / size) E("!fwrite"); break;
        case CURLINFO_DATA_OUT: if (data_out_str.use && fwrite(data, size, 1, data_out_str.file) != size / size) E("!fwrite"); break;
        case CURLINFO_HEADER_IN: if (header_in_str.use && fwrite(data, size, 1, header_in_str.file) != size / size) E("!fwrite"); break;
        case CURLINFO_HEADER_OUT: if (header_out_str.use && fwrite(data, size, 1, header_out_str.file) != size / size) E("!fwrite"); break;
        case CURLINFO_TEXT: if (debug_str.use && fwrite(data, size, 1, debug_str.file) != size / size) E("!fwrite"); break;
        default: break;
    }
    return 0;
}

#if CURL_AT_LEAST_VERSION(7, 32, 0)
static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_interrupt_requested; }
#endif

#if CURL_AT_LEAST_VERSION(7, 9, 7)
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) { return size * nmemb; }
#endif

EXTENSION(pg_curl_easy_perform) {
    char errbuf[CURL_ERROR_SIZE] = {0};
    CURLcode res = CURL_LAST;
    int try = PG_ARGISNULL(0) ? 1 : PG_GETARG_INT32(0);
    long sleep = PG_ARGISNULL(1) ? 1000000 : PG_GETARG_INT64(1);
    if (try <= 0) E("try <= 0!");
    if (sleep < 0) E("sleep < 0!");
    data_in_str.use = PG_ARGISNULL(5) ? true :PG_GETARG_BOOL(5);
    data_out_str.use = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);
    debug_str.use = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
    header_in_str.use = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);
    header_out_str.use = PG_ARGISNULL(4) ? false : PG_GETARG_BOOL(4);
    data_in_str.use ? initMemoryStreamString(&data_in_str) : freeMemoryStreamString(&data_in_str);
    data_out_str.use ? initMemoryStreamString(&data_out_str) : freeMemoryStreamString(&data_out_str);
    debug_str.use ? initMemoryStreamString(&debug_str) : freeMemoryStreamString(&debug_str);
    header_in_str.use ? initMemoryStreamString(&header_in_str) : freeMemoryStreamString(&header_in_str);
    header_out_str.use ? initMemoryStreamString(&header_out_str) : freeMemoryStreamString(&header_out_str);
    if (debug_str.use || header_in_str.use || header_out_str.use || data_in_str.use || data_out_str.use) {
        if ((res = curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_VERBOSE): %s", curl_easy_strerror(res));
        if ((res = curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_callback)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_DEBUGFUNCTION): %s", curl_easy_strerror(res));
    }
    if ((res = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_ERRORBUFFER): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_NOPROGRESS): %s", curl_easy_strerror(res));
#if CURL_AT_LEAST_VERSION(7, 9, 7)
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_WRITEFUNCTION): %s", curl_easy_strerror(res));
#endif
#if CURL_AT_LEAST_VERSION(7, 32, 0)
    if ((res = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_XFERINFOFUNCTION): %s", curl_easy_strerror(res));
#endif
    if (header && ((res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_HTTPHEADER): %s", curl_easy_strerror(res));
#if CURL_AT_LEAST_VERSION(7, 32, 0)
    if (recipient && ((res = curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipient)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_MAIL_RCPT): %s", curl_easy_strerror(res));
#endif
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    if (has_mime && ((res = curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_MIMEPOST): %s", curl_easy_strerror(res));
#endif
    pg_curl_interrupt_requested = 0;
    while (try--) switch (res = curl_easy_perform(curl)) {
        case CURLE_OK: try = 0; break;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_FAILED_INIT:
        case CURLE_URL_MALFORMAT:
        case CURLE_NOT_BUILT_IN:
        case CURLE_FUNCTION_NOT_FOUND:
        case CURLE_BAD_FUNCTION_ARGUMENT:
        case CURLE_UNKNOWN_OPTION:
        case CURLE_LDAP_INVALID_URL:
        case CURLE_ABORTED_BY_CALLBACK: try = 0; if (pgsql_interrupt_handler && pg_curl_interrupt_requested) { (*pgsql_interrupt_handler)(pg_curl_interrupt_requested); pg_curl_interrupt_requested = 0; } // fall through
        default: if (try) {
            if (strlen(errbuf)) W("curl_easy_perform: %s: %s", curl_easy_strerror(res), errbuf);
            else W("curl_easy_perform: %s", curl_easy_strerror(res));
            if (sleep) pg_usleep(sleep);
        } else {
            if (strlen(errbuf)) E("curl_easy_perform: %s: %s", curl_easy_strerror(res), errbuf);
            else E("curl_easy_perform: %s", curl_easy_strerror(res));
        }
    }
    if (data_in_str.use) fclose(data_in_str.file);
    if (data_out_str.use) fclose(data_out_str.file);
    if (header_in_str.use) fclose(header_in_str.file);
    if (header_out_str.use) fclose(header_out_str.file);
    if (debug_str.use) fclose(debug_str.file);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_getinfo_debug) {
    if (!debug_str.len) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(debug_str.data, debug_str.len));
}

EXTENSION(pg_curl_easy_getinfo_header_in) {
    if (!header_in_str.len) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(header_in_str.data, header_in_str.len));
}

EXTENSION(pg_curl_easy_getinfo_header_out) {
    if (!header_out_str.len) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(header_out_str.data, header_out_str.len));
}

EXTENSION(pg_curl_easy_getinfo_data_in) {
    if (!data_in_str.len) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(cstring_to_text_with_len(data_in_str.data, data_in_str.len));
}

EXTENSION(pg_curl_easy_getinfo_data_out) {
    if (!data_out_str.len) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(cstring_to_text_with_len(data_out_str.data, data_out_str.len));
}

EXTENSION(pg_curl_easy_getinfo_headers) {
    W("curl_easy_getinfo_headers deprecated, use curl_easy_perform(header_in:=true) and curl_easy_getinfo_header_in() instead");
    return pg_curl_easy_getinfo_header_in(fcinfo);
}

EXTENSION(pg_curl_easy_getinfo_response) {
    W("curl_easy_getinfo_response deprecated, use curl_easy_perform(data_in:=true) and curl_easy_getinfo_data_in() instead");
    return pg_curl_easy_getinfo_data_in(fcinfo);
}

static Datum pg_curl_easy_getinfo_char(PG_FUNCTION_ARGS, CURLINFO info) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    CURLcode res = CURL_LAST;
    char *value = NULL;
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(value));
#else
    E("curl_easy_getinfo_* requires curl 7.4.1 or later");
#endif
}

EXTENSION(pg_curl_easy_getinfo_content_type) {
#if CURL_AT_LEAST_VERSION(7, 9, 4)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_CONTENT_TYPE);
#else
    E("curl_easy_getinfo_content_type requires curl 7.9.4 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_effective_url) {
#if CURL_AT_LEAST_VERSION(7, 4, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_EFFECTIVE_URL);
#else
    E("curl_easy_getinfo_effective_url requires curl 7.4.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_ftp_entry_path) {
#if CURL_AT_LEAST_VERSION(7, 15, 4)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_FTP_ENTRY_PATH);
#else
    E("curl_easy_getinfo_ftp_entry_path requires curl 7.15.4 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_local_ip) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_LOCAL_IP);
#else
    E("curl_easy_getinfo_local_ip requires curl 7.21.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_primary_ip) {
#if CURL_AT_LEAST_VERSION(7, 19, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_PRIMARY_IP);
#else
    E("curl_easy_getinfo_primary_ip requires curl 7.19.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_private) {
#if CURL_AT_LEAST_VERSION(7, 10, 3)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_PRIVATE);
#else
    E("curl_easy_getinfo_private requires curl 7.10.3 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_redirect_url) {
#if CURL_AT_LEAST_VERSION(7, 18, 2)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_REDIRECT_URL);
#else
    E("curl_easy_getinfo_redirect_url requires curl 7.18.2 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_session_id) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_RTSP_SESSION_ID);
#else
    E("curl_easy_getinfo_rtsp_session_id requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_scheme) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_SCHEME);
#else
    E("curl_easy_getinfo_scheme requires curl 7.52.0 or later");
#endif
}

static Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS, CURLINFO info) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    CURLcode res = CURL_LAST;
    long value;
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    PG_RETURN_INT64(value);
#else
    E("curl_easy_getinfo_* requires curl 7.4.1 or later");
#endif
}

EXTENSION(pg_curl_easy_getinfo_condition_unmet) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_CONDITION_UNMET);
#else
    E("curl_easy_getinfo_condition_unmet requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_filetime) {
#if CURL_AT_LEAST_VERSION(7, 5, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_FILETIME);
#else
    E("curl_easy_getinfo_filetime requires curl 7.5.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_header_size) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HEADER_SIZE);
#else
    E("curl_easy_getinfo_header_size requires curl 7.4.1 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_httpauth_avail) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTPAUTH_AVAIL);
#else
    E("curl_easy_getinfo_httpauth_avail requires curl 7.10.8 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_http_connectcode) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTP_CONNECTCODE);
#else
    E("curl_easy_getinfo_http_connectcode requires curl 7.10.7 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_http_version) {
#if CURL_AT_LEAST_VERSION(7, 50, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTP_VERSION);
#else
    E("curl_easy_getinfo_http_version requires curl 7.50.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_lastsocket) {
#if CURL_AT_LEAST_VERSION(7, 15, 2)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_LASTSOCKET);
#else
    E("curl_easy_getinfo_lastsocket requires curl 7.15.2 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_local_port) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_LOCAL_PORT);
#else
    E("curl_easy_getinfo_local_port requires curl 7.21.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_num_connects) {
#if CURL_AT_LEAST_VERSION(7, 12, 3)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_NUM_CONNECTS);
#else
    E("curl_easy_getinfo_num_connects requires curl 7.12.3 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_os_errno) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_OS_ERRNO);
#else
    E("curl_easy_getinfo_os_errno requires curl 7.12.2 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_primary_port) {
#if CURL_AT_LEAST_VERSION(7, 21, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PRIMARY_PORT);
#else
    E("curl_easy_getinfo_primary_port requires curl 7.21.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_protocol) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROTOCOL);
#else
    E("curl_easy_getinfo_protocol requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_proxyauth_avail) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROXYAUTH_AVAIL);
#else
    E("curl_easy_getinfo_proxyauth_avail requires curl 7.10.8 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_proxy_ssl_verifyresult) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROXY_SSL_VERIFYRESULT);
#else
    E("curl_easy_getinfo_proxy_ssl_verifyresult requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_redirect_count) {
#if CURL_AT_LEAST_VERSION(7, 9, 7)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_REDIRECT_COUNT);
#else
    E("curl_easy_getinfo_redirect_count requires curl 7.9.7 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_request_size) {
#if CURL_AT_LEAST_VERSION(7, 4, 1)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_REQUEST_SIZE);
#else
    E("curl_easy_getinfo_request_size requires curl 7.4.1 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_response_code) {
#if CURL_AT_LEAST_VERSION(7, 10, 8)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RESPONSE_CODE);
#else
    E("curl_easy_getinfo_response_code requires curl 7.10.8 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_client_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_CLIENT_CSEQ);
#else
    E("curl_easy_getinfo_rtsp_client_cseq requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_cseq_recv) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_CSEQ_RECV);
#else
    E("curl_easy_getinfo_rtsp_cseq_recv requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_rtsp_server_cseq) {
#if CURL_AT_LEAST_VERSION(7, 20, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_SERVER_CSEQ);
#else
    E("curl_easy_getinfo_rtsp_server_cseq requires curl 7.20.0 or later");
#endif
}
EXTENSION(pg_curl_easy_getinfo_ssl_verifyresult) {
#if CURL_AT_LEAST_VERSION(7, 5, 0)
    return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_SSL_VERIFYRESULT);
#else
    E("curl_easy_getinfo_ssl_verifyresult requires curl 7.5.0 or later");
#endif
}

EXTENSION(pg_curl_http_version_1_0) { PG_RETURN_INT64(CURL_HTTP_VERSION_1_0); }
EXTENSION(pg_curl_http_version_1_1) { PG_RETURN_INT64(CURL_HTTP_VERSION_1_1); }
EXTENSION(pg_curl_http_version_2_0) { PG_RETURN_INT64(CURL_HTTP_VERSION_2_0); }
EXTENSION(pg_curl_http_version_2_prior_knowledge) {
#if CURL_AT_LEAST_VERSION(7, 49, 0)
    PG_RETURN_INT64(CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
#else
    E("curl_http_version_2_prior_knowledge requires curl 7.49.0 or later");
#endif
}
EXTENSION(pg_curl_http_version_2tls) { PG_RETURN_INT64(CURL_HTTP_VERSION_2TLS); }
EXTENSION(pg_curl_http_version_3) {
#if CURL_AT_LEAST_VERSION(7, 66, 0)
    PG_RETURN_INT64(CURL_HTTP_VERSION_3);
#else
    E("curl_http_version_3 requires curl 7.66.0 or later");
#endif
}
EXTENSION(pg_curl_http_version_none) { PG_RETURN_INT64(CURL_HTTP_VERSION_NONE); }

EXTENSION(pg_curlusessl_none) { PG_RETURN_INT64(CURLUSESSL_NONE); }
EXTENSION(pg_curlusessl_try) { PG_RETURN_INT64(CURLUSESSL_TRY); }
EXTENSION(pg_curlusessl_control) { PG_RETURN_INT64(CURLUSESSL_CONTROL); }
EXTENSION(pg_curlusessl_all) { PG_RETURN_INT64(CURLUSESSL_ALL); }

EXTENSION(pg_curl_upkeep_interval_default) {
#ifdef CURL_UPKEEP_INTERVAL_DEFAULT
    PG_RETURN_INT64(CURL_UPKEEP_INTERVAL_DEFAULT);
#else
    E("!CURL_UPKEEP_INTERVAL_DEFAULT");
#endif
}

EXTENSION(pg_curl_timecond_none) { PG_RETURN_INT64(CURL_TIMECOND_NONE); }
EXTENSION(pg_curl_timecond_ifmodsince) { PG_RETURN_INT64(CURL_TIMECOND_IFMODSINCE); }
EXTENSION(pg_curl_timecond_ifunmodsince) { PG_RETURN_INT64(CURL_TIMECOND_IFUNMODSINCE); }

EXTENSION(pg_curl_sslversion_default) { PG_RETURN_INT64(CURL_SSLVERSION_DEFAULT); }
EXTENSION(pg_curl_sslversion_tlsv1) { PG_RETURN_INT64(CURL_SSLVERSION_TLSv1); }
EXTENSION(pg_curl_sslversion_sslv2) { PG_RETURN_INT64(CURL_SSLVERSION_SSLv2); }
EXTENSION(pg_curl_sslversion_sslv3) { PG_RETURN_INT64(CURL_SSLVERSION_SSLv3); }
EXTENSION(pg_curl_sslversion_tlsv1_0) { PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_0); }
EXTENSION(pg_curl_sslversion_tlsv1_1) { PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_1); }
EXTENSION(pg_curl_sslversion_tlsv1_2) { PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_2); }
EXTENSION(pg_curl_sslversion_tlsv1_3) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_3);
#else
    E("curl_sslversion_tlsv1_3 requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curl_sslversion_max_default) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_DEFAULT);
#else
    E("curl_sslversion_max_default requires curl 7.54.0 or later");
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_0) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_0);
#else
    E("curl_sslversion_max_tlsv1_0 requires curl 7.54.0 or later");
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_1) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_1);
#else
    E("curl_sslversion_max_tlsv1_1 requires curl 7.54.0 or later");
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_2) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_2);
#else
    E("curl_sslversion_max_tlsv1_2 requires curl 7.54.0 or later");
#endif
}
EXTENSION(pg_curl_sslversion_max_tlsv1_3) {
#if CURL_AT_LEAST_VERSION(7, 54, 0)
    PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_3);
#else
    E("curl_sslversion_max_tlsv1_3 requires curl 7.54.0 or later");
#endif
}

EXTENSION(pg_curlsslopt_allow_beast) { PG_RETURN_INT64(CURLSSLOPT_ALLOW_BEAST); }
EXTENSION(pg_curlsslopt_no_revoke) { PG_RETURN_INT64(CURLSSLOPT_NO_REVOKE); }
EXTENSION(pg_curlsslopt_no_partialchain) {
#ifdef CURLSSLOPT_NO_PARTIALCHAIN
    PG_RETURN_INT64(CURLSSLOPT_NO_PARTIALCHAIN);
#else
    E("!CURLSSLOPT_NO_PARTIALCHAIN");
#endif
}
EXTENSION(pg_curlsslopt_revoke_best_effort) {
#ifdef CURLSSLOPT_REVOKE_BEST_EFFORT
    PG_RETURN_INT64(CURLSSLOPT_REVOKE_BEST_EFFORT);
#else
    E("!CURLSSLOPT_REVOKE_BEST_EFFORT");
#endif
}
EXTENSION(pg_curlsslopt_native_ca) {
#ifdef CURLSSLOPT_NATIVE_CA
    PG_RETURN_INT64(CURLSSLOPT_NATIVE_CA);
#else
    E("!CURLSSLOPT_NATIVE_CA");
#endif
}
EXTENSION(pg_curlsslopt_auto_client_cert) {
#ifdef CURLSSLOPT_AUTO_CLIENT_CERT
    PG_RETURN_INT64(CURLSSLOPT_AUTO_CLIENT_CERT);
#else
    E("!CURLSSLOPT_AUTO_CLIENT_CERT");
#endif
}

EXTENSION(pg_curlssh_auth_publickey) { PG_RETURN_INT64(CURLSSH_AUTH_PUBLICKEY); }
EXTENSION(pg_curlssh_auth_password) { PG_RETURN_INT64(CURLSSH_AUTH_PASSWORD); }
EXTENSION(pg_curlssh_auth_host) { PG_RETURN_INT64(CURLSSH_AUTH_HOST); }
EXTENSION(pg_curlssh_auth_keyboard) { PG_RETURN_INT64(CURLSSH_AUTH_KEYBOARD); }
EXTENSION(pg_curlssh_auth_agent) { PG_RETURN_INT64(CURLSSH_AUTH_AGENT); }
EXTENSION(pg_curlssh_auth_any) { PG_RETURN_INT64(CURLSSH_AUTH_ANY); }

EXTENSION(pg_curlauth_basic) { PG_RETURN_INT64(CURLAUTH_BASIC); }
EXTENSION(pg_curlauth_digest) { PG_RETURN_INT64(CURLAUTH_DIGEST); }
EXTENSION(pg_curlauth_digest_ie) { PG_RETURN_INT64(CURLAUTH_DIGEST_IE); }
EXTENSION(pg_curlauth_bearer) {
#if CURL_AT_LEAST_VERSION(7, 61, 0)
    PG_RETURN_INT64(CURLAUTH_BEARER);
#else
    E("curlauth_bearer requires curl 7.61.0 or later");
#endif
}
EXTENSION(pg_curlauth_negotiate) { PG_RETURN_INT64(CURLAUTH_NEGOTIATE); }
EXTENSION(pg_curlauth_ntlm) { PG_RETURN_INT64(CURLAUTH_NTLM); }
EXTENSION(pg_curlauth_ntlm_wb) { PG_RETURN_INT64(CURLAUTH_NTLM_WB); }
EXTENSION(pg_curlauth_any) { PG_RETURN_INT64(CURLAUTH_ANY); }
EXTENSION(pg_curlauth_anysafe) { PG_RETURN_INT64(CURLAUTH_ANYSAFE); }
EXTENSION(pg_curlauth_only) { PG_RETURN_INT64(CURLAUTH_ONLY); }
EXTENSION(pg_curlauth_aws_sigv4) {
#ifdef CURLAUTH_AWS_SIGV4
    PG_RETURN_INT64(CURLAUTH_AWS_SIGV4);
#else
    E("!CURLAUTH_AWS_SIGV4");
#endif
}
EXTENSION(pg_curlauth_gssapi) {
#ifdef CURLAUTH_GSSAPI
    PG_RETURN_INT64(CURLAUTH_GSSAPI);
#else
    E("!CURLAUTH_GSSAPI");
#endif
}
EXTENSION(pg_curlauth_none) { PG_RETURN_INT64(CURLAUTH_NONE); }

EXTENSION(pg_curl_rtspreq_options) { PG_RETURN_INT64(CURL_RTSPREQ_OPTIONS); }
EXTENSION(pg_curl_rtspreq_describe) { PG_RETURN_INT64(CURL_RTSPREQ_DESCRIBE); }
EXTENSION(pg_curl_rtspreq_announce) { PG_RETURN_INT64(CURL_RTSPREQ_ANNOUNCE); }
EXTENSION(pg_curl_rtspreq_setup) { PG_RETURN_INT64(CURL_RTSPREQ_SETUP); }
EXTENSION(pg_curl_rtspreq_play) { PG_RETURN_INT64(CURL_RTSPREQ_PLAY); }
EXTENSION(pg_curl_rtspreq_pause) { PG_RETURN_INT64(CURL_RTSPREQ_PAUSE); }
EXTENSION(pg_curl_rtspreq_teardown) { PG_RETURN_INT64(CURL_RTSPREQ_TEARDOWN); }
EXTENSION(pg_curl_rtspreq_get_parameter) { PG_RETURN_INT64(CURL_RTSPREQ_GET_PARAMETER); }
EXTENSION(pg_curl_rtspreq_set_parameter) { PG_RETURN_INT64(CURL_RTSPREQ_SET_PARAMETER); }
EXTENSION(pg_curl_rtspreq_record) { PG_RETURN_INT64(CURL_RTSPREQ_RECORD); }
EXTENSION(pg_curl_rtspreq_receive) { PG_RETURN_INT64(CURL_RTSPREQ_RECEIVE); }

EXTENSION(pg_curlproto_dict) { PG_RETURN_INT64(CURLPROTO_DICT); }
EXTENSION(pg_curlproto_file) { PG_RETURN_INT64(CURLPROTO_FILE); }
EXTENSION(pg_curlproto_ftp) { PG_RETURN_INT64(CURLPROTO_FTP); }
EXTENSION(pg_curlproto_ftps) { PG_RETURN_INT64(CURLPROTO_FTPS); }
EXTENSION(pg_curlproto_gopher) { PG_RETURN_INT64(CURLPROTO_GOPHER); }
EXTENSION(pg_curlproto_http) { PG_RETURN_INT64(CURLPROTO_HTTP); }
EXTENSION(pg_curlproto_https) { PG_RETURN_INT64(CURLPROTO_HTTPS); }
EXTENSION(pg_curlproto_imap) { PG_RETURN_INT64(CURLPROTO_IMAP); }
EXTENSION(pg_curlproto_imaps) { PG_RETURN_INT64(CURLPROTO_IMAPS); }
EXTENSION(pg_curlproto_ldap) { PG_RETURN_INT64(CURLPROTO_LDAP); }
EXTENSION(pg_curlproto_ldaps) { PG_RETURN_INT64(CURLPROTO_LDAPS); }
EXTENSION(pg_curlproto_pop3) { PG_RETURN_INT64(CURLPROTO_POP3); }
EXTENSION(pg_curlproto_pop3s) { PG_RETURN_INT64(CURLPROTO_POP3S); }
EXTENSION(pg_curlproto_rtmp) { PG_RETURN_INT64(CURLPROTO_RTMP); }
EXTENSION(pg_curlproto_rtmpe) { PG_RETURN_INT64(CURLPROTO_RTMPE); }
EXTENSION(pg_curlproto_rtmps) { PG_RETURN_INT64(CURLPROTO_RTMPS); }
EXTENSION(pg_curlproto_rtmpt) { PG_RETURN_INT64(CURLPROTO_RTMPT); }
EXTENSION(pg_curlproto_rtmpte) { PG_RETURN_INT64(CURLPROTO_RTMPTE); }
EXTENSION(pg_curlproto_rtmpts) { PG_RETURN_INT64(CURLPROTO_RTMPTS); }
EXTENSION(pg_curlproto_rtsp) { PG_RETURN_INT64(CURLPROTO_RTSP); }
EXTENSION(pg_curlproto_scp) { PG_RETURN_INT64(CURLPROTO_SCP); }
EXTENSION(pg_curlproto_sftp) { PG_RETURN_INT64(CURLPROTO_SFTP); }
EXTENSION(pg_curlproto_smb) { PG_RETURN_INT64(CURLPROTO_SMB); }
EXTENSION(pg_curlproto_smbs) { PG_RETURN_INT64(CURLPROTO_SMBS); }
EXTENSION(pg_curlproto_smtp) { PG_RETURN_INT64(CURLPROTO_SMTP); }
EXTENSION(pg_curlproto_smtps) { PG_RETURN_INT64(CURLPROTO_SMTPS); }
EXTENSION(pg_curlproto_telnet) { PG_RETURN_INT64(CURLPROTO_TELNET); }
EXTENSION(pg_curlproto_tftp) { PG_RETURN_INT64(CURLPROTO_TFTP); }
EXTENSION(pg_curlproto_all) { PG_RETURN_INT64(CURLPROTO_ALL); }

EXTENSION(pg_curlproxy_http) { PG_RETURN_INT64(CURLPROXY_HTTP); }
EXTENSION(pg_curlproxy_https) {
#if CURL_AT_LEAST_VERSION(7, 52, 0)
    PG_RETURN_INT64(CURLPROXY_HTTPS);
#else
    E("curlproxy_https requires curl 7.52.0 or later");
#endif
}
EXTENSION(pg_curlproxy_http_1_0) { PG_RETURN_INT64(CURLPROXY_HTTP_1_0); }
EXTENSION(pg_curlproxy_socks4) { PG_RETURN_INT64(CURLPROXY_SOCKS4); }
EXTENSION(pg_curlproxy_socks4a) { PG_RETURN_INT64(CURLPROXY_SOCKS4A); }
EXTENSION(pg_curlproxy_socks5) { PG_RETURN_INT64(CURLPROXY_SOCKS5); }
EXTENSION(pg_curlproxy_socks5_hostname) { PG_RETURN_INT64(CURLPROXY_SOCKS5_HOSTNAME); }

EXTENSION(pg_curl_redir_post_301) { PG_RETURN_INT64(CURL_REDIR_POST_301); }
EXTENSION(pg_curl_redir_post_302) { PG_RETURN_INT64(CURL_REDIR_POST_302); }
EXTENSION(pg_curl_redir_post_303) { PG_RETURN_INT64(CURL_REDIR_POST_303); }
EXTENSION(pg_curl_redir_post_all) { PG_RETURN_INT64(CURL_REDIR_POST_ALL); }

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
    E("!CURL_HET_DEFAULT");
#endif
}

EXTENSION(pg_curlgssapi_delegation_flag) { PG_RETURN_INT64(CURLGSSAPI_DELEGATION_FLAG); }
EXTENSION(pg_curlgssapi_delegation_policy_flag) { PG_RETURN_INT64(CURLGSSAPI_DELEGATION_POLICY_FLAG); }
EXTENSION(pg_curlgssapi_delegation_none) { PG_RETURN_INT64(CURLGSSAPI_DELEGATION_NONE); }

EXTENSION(pg_curlftpssl_ccc_none) { PG_RETURN_INT64(CURLFTPSSL_CCC_NONE); }
EXTENSION(pg_curlftpssl_ccc_passive) { PG_RETURN_INT64(CURLFTPSSL_CCC_PASSIVE); }
EXTENSION(pg_curlftpssl_ccc_active) { PG_RETURN_INT64(CURLFTPSSL_CCC_ACTIVE); }

EXTENSION(pg_curlftpauth_default) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    PG_RETURN_INT64(CURLFTPAUTH_DEFAULT);
#else
    E("curlftpauth_default requires curl 7.12.2 or later");
#endif
}
EXTENSION(pg_curlftpauth_ssl) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    PG_RETURN_INT64(CURLFTPAUTH_SSL);
#else
    E("curlftpauth_ssl requires curl 7.12.2 or later");
#endif
}
EXTENSION(pg_curlftpauth_tls) {
#if CURL_AT_LEAST_VERSION(7, 12, 2)
    PG_RETURN_INT64(CURLFTPAUTH_TLS);
#else
    E("curlftpauth_tls requires curl 7.12.2 or later");
#endif
}

EXTENSION(pg_curlftpmethod_multicwd) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    PG_RETURN_INT64(CURLFTPMETHOD_MULTICWD);
#else
    E("curlftpmethod_multicwd requires curl 7.15.1 or later");
#endif
}
EXTENSION(pg_curlftpmethod_nocwd) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    PG_RETURN_INT64(CURLFTPMETHOD_NOCWD);
#else
    E("curlftpmethod_nocwd requires curl 7.15.1 or later");
#endif
}
EXTENSION(pg_curlftpmethod_singlecwd) {
#if CURL_AT_LEAST_VERSION(7, 15, 1)
    PG_RETURN_INT64(CURLFTPMETHOD_SINGLECWD);
#else
    E("curlftpmethod_singlecwd requires curl 7.15.1 or later");
#endif
}

EXTENSION(pg_curlftp_create_dir) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    PG_RETURN_INT64(CURLFTP_CREATE_DIR);
#else
    E("curlftp_create_dir requires curl 7.10.7 or later");
#endif
}
EXTENSION(pg_curlftp_create_dir_retry) {
#if CURL_AT_LEAST_VERSION(7, 19, 4)
    PG_RETURN_INT64(CURLFTP_CREATE_DIR_RETRY);
#else
    E("curlftp_create_dir_retry requires curl 7.19.4 or later");
#endif
}
EXTENSION(pg_curlftp_create_dir_none) {
#if CURL_AT_LEAST_VERSION(7, 10, 7)
    PG_RETURN_INT64(CURLFTP_CREATE_DIR_NONE);
#else
    E("curlftp_create_dir_none requires curl 7.10.7 or later");
#endif
}

EXTENSION(pg_curl_max_write_size) {
#ifdef CURL_MAX_WRITE_SIZE
    PG_RETURN_INT64(CURL_MAX_WRITE_SIZE);
#else
    E("!CURL_MAX_WRITE_SIZE");
#endif
}

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

typedef struct FileString {
    char *data;
    FILE *file;
    size_t len;
} FileString;

#if CURL_AT_LEAST_VERSION(7, 56, 0)
static bool has_mime;
#endif
static CURL *curl = NULL;
#if CURL_AT_LEAST_VERSION(7, 56, 0)
static curl_mime *mime;
#endif
static FileString header_str = {NULL, NULL, 0};
static FILE *read_str_file = NULL;
static FileString write_str = {NULL, NULL, 0};
static int pg_curl_interrupt_requested = 0;
static pqsigfunc pgsql_interrupt_handler = NULL;
static struct curl_slist *header = NULL;
static struct curl_slist *recipient = NULL;

static void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }

void _PG_init(void); void _PG_init(void) {
    if (curl_global_init(CURL_GLOBAL_ALL)) E("curl_global_init");
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
    curl_slist_free_all(recipient);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    if (header_str.data) { free(header_str.data); header_str.data = NULL; }
    if (read_str_file) { fclose(read_str_file); read_str_file = NULL; }
    if (write_str.data) { free(write_str.data); write_str.data = NULL; }
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
    if (read_str_file) { fclose(read_str_file); read_str_file = NULL; }
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_recipient_reset) {
    curl_slist_free_all(recipient);
    recipient = NULL;
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_reset) {
    pg_curl_easy_header_reset(fcinfo);
#if CURL_AT_LEAST_VERSION(7, 56, 0)
    pg_curl_easy_mime_reset(fcinfo);
#endif
    pg_curl_easy_readdata_reset(fcinfo);
    pg_curl_easy_recipient_reset(fcinfo);
    curl_easy_reset(curl);
    PG_RETURN_VOID();
}

EXTENSION(pg_curl_easy_escape) {
    text *string;
    char *escape;
    if (PG_ARGISNULL(0)) E("string is null!");
    string = DatumGetTextP(PG_GETARG_DATUM(0));
    if (!(escape = curl_easy_escape(curl, VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string)))) E("!curl_easy_escape");
    string = cstring_to_text(escape);
    curl_free(escape);
    PG_RETURN_TEXT_P(string);
}

EXTENSION(pg_curl_easy_unescape) {
    text *url;
    char *unescape;
    int outlength;
    if (PG_ARGISNULL(0)) E("url is null!");
    url = DatumGetTextP(PG_GETARG_DATUM(0));
    if (!(unescape = curl_easy_unescape(curl, VARDATA_ANY(url), VARSIZE_ANY_EXHDR(url), &outlength))) PG_RETURN_NULL();
    url = cstring_to_text_with_len(unescape, outlength);
    curl_free(unescape);
    PG_RETURN_TEXT_P(url);
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
    char *email;
    struct curl_slist *temp = recipient;
    if (PG_ARGISNULL(0)) E("email is null!");
    email = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, email))) recipient = temp; else E("!curl_slist_append");
    pfree(email);
    PG_RETURN_BOOL(temp != NULL);
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

EXTENSION(pg_curl_easy_setopt_char2) {
    char *parameter;
    CURLcode res = CURL_LAST;
    CURLoption option;
    if (PG_ARGISNULL(0)) E("option is null!");
    if (PG_ARGISNULL(1)) E("parameter is null!");
    option = PG_GETARG_INT32(0);
    parameter = TextDatumGetCString(PG_GETARG_DATUM(1));
    if ((res = curl_easy_setopt(curl, option, parameter)) != CURLE_OK) E("curl_easy_setopt(%i, %s): %s", option, parameter, curl_easy_strerror(res));
    pfree(parameter);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_copypostfields) {
    CURLcode res = CURL_LAST;
    bytea *parameter;
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = DatumGetTextP(PG_GETARG_DATUM(0));
    if ((res = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, VARSIZE_ANY_EXHDR(parameter))) != CURLE_OK) E("curl_easy_setopt(CURLOPT_POSTFIELDSIZE): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, VARDATA_ANY(parameter))) != CURLE_OK) E("curl_easy_setopt(CURLOPT_COPYPOSTFIELDS): %s", curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_long2) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    long parameter;
    if (PG_ARGISNULL(0)) E("option is null!");
    if (PG_ARGISNULL(1)) E("parameter is null!");
    option = PG_GETARG_INT32(0);
    parameter = PG_GETARG_INT64(1);
    if ((res = curl_easy_setopt(curl, option, parameter)) != CURLE_OK) E("curl_easy_setopt(%i, %li): %s", option, parameter, curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_readdata) {
    CURLcode res = CURL_LAST;
    bytea *parameter;
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = DatumGetTextP(PG_GETARG_DATUM(0));
    if (read_str_file) { fclose(read_str_file); read_str_file = NULL; }
    if ((res = curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_UPLOAD): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_INFILESIZE, VARSIZE_ANY_EXHDR(parameter))) != CURLE_OK) E("curl_easy_setopt(CURLOPT_INFILESIZE): %s", curl_easy_strerror(res));
    if (!(read_str_file = fmemopen(VARDATA_ANY(parameter), VARSIZE_ANY_EXHDR(parameter), "rb"))) E("!fmemopen");
    if ((res = curl_easy_setopt(curl, CURLOPT_READDATA, read_str_file)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_READDATA): %s", curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
}

static Datum pg_curl_easy_setopt_char(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURL_LAST;
    char *parameter;
    W("Deprecated and will be removed! Use curl_easy_setopt instead.");
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((res = curl_easy_setopt(curl, option, parameter)) != CURLE_OK) E("curl_easy_setopt(%i, %s): %s", option, parameter, curl_easy_strerror(res));
    pfree(parameter);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_abstract_unix_socket) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ABSTRACT_UNIX_SOCKET); }
EXTENSION(pg_curl_easy_setopt_accept_encoding) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ACCEPT_ENCODING); }
EXTENSION(pg_curl_easy_setopt_cainfo) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CAINFO); }
EXTENSION(pg_curl_easy_setopt_capath) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CAPATH); }
EXTENSION(pg_curl_easy_setopt_cookiefile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIEFILE); }
EXTENSION(pg_curl_easy_setopt_cookiejar) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIEJAR); }
EXTENSION(pg_curl_easy_setopt_cookielist) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIELIST); }
EXTENSION(pg_curl_easy_setopt_cookie) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_COOKIE); }
EXTENSION(pg_curl_easy_setopt_crlfile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CRLFILE); }
EXTENSION(pg_curl_easy_setopt_customrequest) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_CUSTOMREQUEST); }
EXTENSION(pg_curl_easy_setopt_default_protocol) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DEFAULT_PROTOCOL); }
EXTENSION(pg_curl_easy_setopt_dns_interface) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_INTERFACE); }
EXTENSION(pg_curl_easy_setopt_dns_local_ip4) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_LOCAL_IP4); }
EXTENSION(pg_curl_easy_setopt_dns_local_ip6) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_LOCAL_IP6); }
EXTENSION(pg_curl_easy_setopt_dns_servers) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DNS_SERVERS); }
EXTENSION(pg_curl_easy_setopt_doh_url) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_DOH_URL); }
EXTENSION(pg_curl_easy_setopt_egdsocket) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_EGDSOCKET); }
EXTENSION(pg_curl_easy_setopt_ftp_account) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTP_ACCOUNT); }
EXTENSION(pg_curl_easy_setopt_ftp_alternative_to_user) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTP_ALTERNATIVE_TO_USER); }
EXTENSION(pg_curl_easy_setopt_ftpport) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_FTPPORT); }
EXTENSION(pg_curl_easy_setopt_interface) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_INTERFACE); }
EXTENSION(pg_curl_easy_setopt_issuercert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_ISSUERCERT); }
EXTENSION(pg_curl_easy_setopt_keypasswd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_KEYPASSWD); }
EXTENSION(pg_curl_easy_setopt_krblevel) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_KRBLEVEL); }
EXTENSION(pg_curl_easy_setopt_login_options) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_LOGIN_OPTIONS); }
EXTENSION(pg_curl_easy_setopt_mail_auth) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_MAIL_AUTH); }
EXTENSION(pg_curl_easy_setopt_mail_from) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_MAIL_FROM); }
EXTENSION(pg_curl_easy_setopt_noproxy) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_NOPROXY); }
EXTENSION(pg_curl_easy_setopt_password) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PASSWORD); }
EXTENSION(pg_curl_easy_setopt_pinnedpublickey) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PINNEDPUBLICKEY); }
EXTENSION(pg_curl_easy_setopt_pre_proxy) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PRE_PROXY); }
EXTENSION(pg_curl_easy_setopt_proxy_cainfo) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CAINFO); }
EXTENSION(pg_curl_easy_setopt_proxy_capath) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CAPATH); }
EXTENSION(pg_curl_easy_setopt_proxy_crlfile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_CRLFILE); }
EXTENSION(pg_curl_easy_setopt_proxy_keypasswd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_KEYPASSWD); }
EXTENSION(pg_curl_easy_setopt_proxypassword) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYPASSWORD); }
EXTENSION(pg_curl_easy_setopt_proxy_pinnedpublickey) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_PINNEDPUBLICKEY); }
EXTENSION(pg_curl_easy_setopt_proxy_service_name) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SERVICE_NAME); }
EXTENSION(pg_curl_easy_setopt_proxy) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY); }
EXTENSION(pg_curl_easy_setopt_proxy_sslcert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLCERT); }
EXTENSION(pg_curl_easy_setopt_proxy_sslcerttype) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLCERTTYPE); }
EXTENSION(pg_curl_easy_setopt_proxy_ssl_cipher_list) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSL_CIPHER_LIST); }
EXTENSION(pg_curl_easy_setopt_proxy_sslkey) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLKEY); }
EXTENSION(pg_curl_easy_setopt_proxy_sslkeytype) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_SSLKEYTYPE); }
EXTENSION(pg_curl_easy_setopt_proxy_tls13_ciphers) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLS13_CIPHERS); }
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_password) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_PASSWORD); }
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_type) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_TYPE); }
EXTENSION(pg_curl_easy_setopt_proxy_tlsauth_username) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXY_TLSAUTH_USERNAME); }
EXTENSION(pg_curl_easy_setopt_proxyusername) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYUSERNAME); }
EXTENSION(pg_curl_easy_setopt_proxyuserpwd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_PROXYUSERPWD); }
EXTENSION(pg_curl_easy_setopt_random_file) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RANDOM_FILE); }
EXTENSION(pg_curl_easy_setopt_range) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RANGE); }
EXTENSION(pg_curl_easy_setopt_referer) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REFERER); }
EXTENSION(pg_curl_easy_setopt_request_target) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_REQUEST_TARGET); }
EXTENSION(pg_curl_easy_setopt_rtsp_session_id) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_SESSION_ID); }
EXTENSION(pg_curl_easy_setopt_rtsp_stream_uri) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_STREAM_URI); }
EXTENSION(pg_curl_easy_setopt_rtsp_transport) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_RTSP_TRANSPORT); }
EXTENSION(pg_curl_easy_setopt_service_name) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SERVICE_NAME); }
EXTENSION(pg_curl_easy_setopt_socks5_gssapi_service) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SOCKS5_GSSAPI_SERVICE); }
EXTENSION(pg_curl_easy_setopt_ssh_host_public_key_md5) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_HOST_PUBLIC_KEY_MD5); }
EXTENSION(pg_curl_easy_setopt_ssh_knownhosts) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_KNOWNHOSTS); }
EXTENSION(pg_curl_easy_setopt_ssh_private_keyfile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_PRIVATE_KEYFILE); }
EXTENSION(pg_curl_easy_setopt_ssh_public_keyfile) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSH_PUBLIC_KEYFILE); }
EXTENSION(pg_curl_easy_setopt_sslcert) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLCERT); }
EXTENSION(pg_curl_easy_setopt_sslcerttype) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLCERTTYPE); }
EXTENSION(pg_curl_easy_setopt_ssl_cipher_list) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSL_CIPHER_LIST); }
EXTENSION(pg_curl_easy_setopt_sslengine) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLENGINE); }
EXTENSION(pg_curl_easy_setopt_sslkey) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLKEY); }
EXTENSION(pg_curl_easy_setopt_sslkeytype) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_SSLKEYTYPE); }
EXTENSION(pg_curl_easy_setopt_tls13_ciphers) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLS13_CIPHERS); }
EXTENSION(pg_curl_easy_setopt_tlsauth_password) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_PASSWORD); }
EXTENSION(pg_curl_easy_setopt_tlsauth_type) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_TYPE); }
EXTENSION(pg_curl_easy_setopt_tlsauth_username) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_TLSAUTH_USERNAME); }
EXTENSION(pg_curl_easy_setopt_unix_socket_path) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_UNIX_SOCKET_PATH); }
EXTENSION(pg_curl_easy_setopt_url) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_URL); }
EXTENSION(pg_curl_easy_setopt_useragent) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERAGENT); }
EXTENSION(pg_curl_easy_setopt_username) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERNAME); }
EXTENSION(pg_curl_easy_setopt_userpwd) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_USERPWD); }
EXTENSION(pg_curl_easy_setopt_xoauth2_bearer) { return pg_curl_easy_setopt_char(fcinfo, CURLOPT_XOAUTH2_BEARER); }

static Datum pg_curl_easy_setopt_long(PG_FUNCTION_ARGS, CURLoption option) {
    CURLcode res = CURL_LAST;
    long parameter;
    W("Deprecated and will be removed! Use curl_easy_setopt instead.");
    if (PG_ARGISNULL(0)) E("parameter is null!");
    parameter = PG_GETARG_INT64(0);
    if ((res = curl_easy_setopt(curl, option, parameter)) != CURLE_OK) E("curl_easy_setopt(%i, %li): %s", option, parameter, curl_easy_strerror(res));
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_accepttimeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_ACCEPTTIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_address_scope) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_ADDRESS_SCOPE); }
EXTENSION(pg_curl_easy_setopt_append) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_APPEND); }
EXTENSION(pg_curl_easy_setopt_autoreferer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_AUTOREFERER); }
EXTENSION(pg_curl_easy_setopt_buffersize) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_BUFFERSIZE); }
EXTENSION(pg_curl_easy_setopt_certinfo) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CERTINFO); }
EXTENSION(pg_curl_easy_setopt_connect_only) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECT_ONLY); }
EXTENSION(pg_curl_easy_setopt_connecttimeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECTTIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_connecttimeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CONNECTTIMEOUT); }
EXTENSION(pg_curl_easy_setopt_cookiesession) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_COOKIESESSION); }
EXTENSION(pg_curl_easy_setopt_crlf) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_CRLF); }
EXTENSION(pg_curl_easy_setopt_dirlistonly) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DIRLISTONLY); }
EXTENSION(pg_curl_easy_setopt_disallow_username_in_url) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DISALLOW_USERNAME_IN_URL); }
EXTENSION(pg_curl_easy_setopt_dns_cache_timeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_CACHE_TIMEOUT); }
EXTENSION(pg_curl_easy_setopt_dns_shuffle_addresses) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_SHUFFLE_ADDRESSES); }
EXTENSION(pg_curl_easy_setopt_dns_use_global_cache) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_DNS_USE_GLOBAL_CACHE); }
EXTENSION(pg_curl_easy_setopt_expect_100_timeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_EXPECT_100_TIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_failonerror) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FAILONERROR); }
EXTENSION(pg_curl_easy_setopt_filetime) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FILETIME); }
EXTENSION(pg_curl_easy_setopt_followlocation) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FOLLOWLOCATION); }
EXTENSION(pg_curl_easy_setopt_forbid_reuse) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FORBID_REUSE); }
EXTENSION(pg_curl_easy_setopt_fresh_connect) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FRESH_CONNECT); }
EXTENSION(pg_curl_easy_setopt_ftp_create_missing_dirs) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_CREATE_MISSING_DIRS); }
EXTENSION(pg_curl_easy_setopt_ftp_filemethod) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_FILEMETHOD); }
EXTENSION(pg_curl_easy_setopt_ftp_skip_pasv_ip) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_SKIP_PASV_IP); }
EXTENSION(pg_curl_easy_setopt_ftpsslauth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTPSSLAUTH); }
EXTENSION(pg_curl_easy_setopt_ftp_ssl_ccc) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_SSL_CCC); }
EXTENSION(pg_curl_easy_setopt_ftp_use_eprt) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_EPRT); }
EXTENSION(pg_curl_easy_setopt_ftp_use_epsv) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_EPSV); }
EXTENSION(pg_curl_easy_setopt_ftp_use_pret) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_FTP_USE_PRET); }
EXTENSION(pg_curl_easy_setopt_gssapi_delegation) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_GSSAPI_DELEGATION); }
EXTENSION(pg_curl_easy_setopt_happy_eyeballs_timeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_haproxyprotocol) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HAPROXYPROTOCOL); }
EXTENSION(pg_curl_easy_setopt_header) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HEADER); }
EXTENSION(pg_curl_easy_setopt_http09_allowed) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP09_ALLOWED); }
EXTENSION(pg_curl_easy_setopt_httpauth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPAUTH); }
EXTENSION(pg_curl_easy_setopt_http_content_decoding) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_CONTENT_DECODING); }
EXTENSION(pg_curl_easy_setopt_httpget) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPGET); }
EXTENSION(pg_curl_easy_setopt_httpproxytunnel) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTPPROXYTUNNEL); }
EXTENSION(pg_curl_easy_setopt_http_transfer_decoding) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_TRANSFER_DECODING); }
EXTENSION(pg_curl_easy_setopt_http_version) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_HTTP_VERSION); }
EXTENSION(pg_curl_easy_setopt_ignore_content_length) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_IGNORE_CONTENT_LENGTH); }
EXTENSION(pg_curl_easy_setopt_ipresolve) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_IPRESOLVE); }
EXTENSION(pg_curl_easy_setopt_keep_sending_on_error) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_KEEP_SENDING_ON_ERROR); }
EXTENSION(pg_curl_easy_setopt_localportrange) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOCALPORTRANGE); }
EXTENSION(pg_curl_easy_setopt_localport) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOCALPORT); }
EXTENSION(pg_curl_easy_setopt_low_speed_limit) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOW_SPEED_LIMIT); }
EXTENSION(pg_curl_easy_setopt_low_speed_time) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_LOW_SPEED_TIME); }
EXTENSION(pg_curl_easy_setopt_maxconnects) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_MAXCONNECTS); }
EXTENSION(pg_curl_easy_setopt_maxfilesize) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_MAXFILESIZE); }
EXTENSION(pg_curl_easy_setopt_maxredirs) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_MAXREDIRS); }
EXTENSION(pg_curl_easy_setopt_netrc) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NETRC); }
EXTENSION(pg_curl_easy_setopt_new_directory_perms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NEW_DIRECTORY_PERMS); }
EXTENSION(pg_curl_easy_setopt_new_file_perms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NEW_FILE_PERMS); }
EXTENSION(pg_curl_easy_setopt_nobody) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NOBODY); }
EXTENSION(pg_curl_easy_setopt_nosignal) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_NOSIGNAL); }
EXTENSION(pg_curl_easy_setopt_path_as_is) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PATH_AS_IS); }
EXTENSION(pg_curl_easy_setopt_pipewait) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PIPEWAIT); }
EXTENSION(pg_curl_easy_setopt_port) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PORT); }
EXTENSION(pg_curl_easy_setopt_postredir) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_POSTREDIR); }
EXTENSION(pg_curl_easy_setopt_post) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_POST); }
EXTENSION(pg_curl_easy_setopt_protocols) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROTOCOLS); }
EXTENSION(pg_curl_easy_setopt_proxyauth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYAUTH); }
EXTENSION(pg_curl_easy_setopt_proxyport) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYPORT); }
EXTENSION(pg_curl_easy_setopt_proxy_ssl_options) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_OPTIONS); }
EXTENSION(pg_curl_easy_setopt_proxy_ssl_verifyhost) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_VERIFYHOST); }
EXTENSION(pg_curl_easy_setopt_proxy_ssl_verifypeer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSL_VERIFYPEER); }
EXTENSION(pg_curl_easy_setopt_proxy_sslversion) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_SSLVERSION); }
EXTENSION(pg_curl_easy_setopt_proxy_transfer_mode) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXY_TRANSFER_MODE); }
EXTENSION(pg_curl_easy_setopt_proxytype) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PROXYTYPE); }
EXTENSION(pg_curl_easy_setopt_put) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_PUT); }
EXTENSION(pg_curl_easy_setopt_redir_protocols) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_REDIR_PROTOCOLS); }
EXTENSION(pg_curl_easy_setopt_resume_from) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RESUME_FROM); }
EXTENSION(pg_curl_easy_setopt_rtsp_client_cseq) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_CLIENT_CSEQ); }
EXTENSION(pg_curl_easy_setopt_rtsp_request) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_REQUEST); }
EXTENSION(pg_curl_easy_setopt_rtsp_server_cseq) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_RTSP_SERVER_CSEQ); }
EXTENSION(pg_curl_easy_setopt_sasl_ir) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SASL_IR); }
EXTENSION(pg_curl_easy_setopt_server_response_timeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SERVER_RESPONSE_TIMEOUT); }
EXTENSION(pg_curl_easy_setopt_socks5_auth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SOCKS5_AUTH); }
EXTENSION(pg_curl_easy_setopt_socks5_gssapi_nec) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SOCKS5_GSSAPI_NEC); }
EXTENSION(pg_curl_easy_setopt_ssh_auth_types) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSH_AUTH_TYPES); }
EXTENSION(pg_curl_easy_setopt_ssh_compression) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSH_COMPRESSION); }
EXTENSION(pg_curl_easy_setopt_ssl_enable_alpn) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_ENABLE_ALPN); }
EXTENSION(pg_curl_easy_setopt_ssl_enable_npn) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_ENABLE_NPN); }
EXTENSION(pg_curl_easy_setopt_ssl_falsestart) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_FALSESTART); }
EXTENSION(pg_curl_easy_setopt_ssl_options) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_OPTIONS); }
EXTENSION(pg_curl_easy_setopt_ssl_sessionid_cache) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_SESSIONID_CACHE); }
EXTENSION(pg_curl_easy_setopt_ssl_verifyhost) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYHOST); }
EXTENSION(pg_curl_easy_setopt_ssl_verifypeer) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYPEER); }
EXTENSION(pg_curl_easy_setopt_ssl_verifystatus) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSL_VERIFYSTATUS); }
EXTENSION(pg_curl_easy_setopt_sslversion) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SSLVERSION); }
EXTENSION(pg_curl_easy_setopt_stream_weight) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_STREAM_WEIGHT); }
EXTENSION(pg_curl_easy_setopt_suppress_connect_headers) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_SUPPRESS_CONNECT_HEADERS); }
EXTENSION(pg_curl_easy_setopt_tcp_fastopen) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_FASTOPEN); }
EXTENSION(pg_curl_easy_setopt_tcp_keepalive) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPALIVE); }
EXTENSION(pg_curl_easy_setopt_tcp_keepidle) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPIDLE); }
EXTENSION(pg_curl_easy_setopt_tcp_keepintvl) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_KEEPINTVL); }
EXTENSION(pg_curl_easy_setopt_tcp_nodelay) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TCP_NODELAY); }
EXTENSION(pg_curl_easy_setopt_tftp_blksize) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TFTP_BLKSIZE); }
EXTENSION(pg_curl_easy_setopt_tftp_no_options) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TFTP_NO_OPTIONS); }
EXTENSION(pg_curl_easy_setopt_timecondition) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMECONDITION); }
EXTENSION(pg_curl_easy_setopt_timeout_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMEOUT_MS); }
EXTENSION(pg_curl_easy_setopt_timeout) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMEOUT); }
EXTENSION(pg_curl_easy_setopt_timevalue) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TIMEVALUE); }
EXTENSION(pg_curl_easy_setopt_transfer_encoding) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TRANSFER_ENCODING); }
EXTENSION(pg_curl_easy_setopt_transfertext) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_TRANSFERTEXT); }
EXTENSION(pg_curl_easy_setopt_unrestricted_auth) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UNRESTRICTED_AUTH); }
EXTENSION(pg_curl_easy_setopt_upkeep_interval_ms) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPKEEP_INTERVAL_MS); }
EXTENSION(pg_curl_easy_setopt_upload_buffersize) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_UPLOAD_BUFFERSIZE); }
EXTENSION(pg_curl_easy_setopt_use_ssl) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_USE_SSL); }
EXTENSION(pg_curl_easy_setopt_verbose) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_VERBOSE); }
EXTENSION(pg_curl_easy_setopt_wildcardmatch) { return pg_curl_easy_setopt_long(fcinfo, CURLOPT_WILDCARDMATCH); }

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_interrupt_requested; }

EXTENSION(pg_curl_easy_perform) {
    char errbuf[CURL_ERROR_SIZE] = {0};
    CURLcode res = CURL_LAST;
    int try;
    long sleep;
    if (PG_ARGISNULL(0)) E("try is null!");
    try = PG_GETARG_INT32(0);
    if (try <= 0) E("try <= 0!");
    if (PG_ARGISNULL(1)) E("sleep is null!");
    sleep = PG_GETARG_INT64(1);
    if (sleep < 0) E("sleep < 0!");
    if (header_str.data) { free(header_str.data); header_str.data = NULL; }
    if (write_str.data) { free(write_str.data); write_str.data = NULL; }
    if (!(header_str.file = open_memstream(&header_str.data, &header_str.len))) E("!open_memstream");
    if (!(write_str.file = open_memstream(&write_str.data, &write_str.len))) E("!open_memstream");
    if ((res = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_ERRORBUFFER): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_HEADERDATA, header_str.file)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_HEADERDATA): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_NOPROGRESS): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_str.file)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_WRITEDATA): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_XFERINFOFUNCTION): %s", curl_easy_strerror(res));
    if (header && ((res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_HTTPHEADER): %s", curl_easy_strerror(res));
    if (recipient && ((res = curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipient)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_MAIL_RCPT): %s", curl_easy_strerror(res));
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
    fclose(header_str.file);
    fclose(write_str.file);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_getinfo_headers) {
    if (!header_str.data) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(header_str.data, header_str.len));
}

EXTENSION(pg_curl_easy_getinfo_response) {
    if (!write_str.data) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(cstring_to_text_with_len(write_str.data, write_str.len));
}

EXTENSION(pg_curl_easy_getinfo) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    if (PG_ARGISNULL(0)) E("info is null!");
    info = PG_GETARG_INT32(0);
    switch (info) {
        case CURLINFO_CONTENT_TYPE:
        case CURLINFO_EFFECTIVE_URL:
        case CURLINFO_FTP_ENTRY_PATH:
        case CURLINFO_LOCAL_IP:
        case CURLINFO_PRIMARY_IP:
        case CURLINFO_PRIVATE:
        case CURLINFO_REDIRECT_URL:
        case CURLINFO_RTSP_SESSION_ID:
        case CURLINFO_SCHEME: {
            char *value = NULL;
            if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
            if (!value) PG_RETURN_NULL();
            PG_RETURN_TEXT_P(cstring_to_text(value));
        } break;
        default: {
            long value;
            StringInfoData buf;
            initStringInfo(&buf);
            if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
            appendStringInfo(&buf, "%li", value);
            PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
        } break;
    }
    PG_RETURN_NULL();
}

EXTENSION(pg_curl_easy_getinfo_char2) {
    char *value = NULL;
    CURLcode res = CURL_LAST;
    CURLINFO info;
    W("Deprecated and will be removed! Use curl_easy_getinfo instead.");
    if (PG_ARGISNULL(0)) E("info is null!");
    info = PG_GETARG_INT32(0);
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(value));
}

static Datum pg_curl_easy_getinfo_char(PG_FUNCTION_ARGS, CURLINFO info) {
    CURLcode res = CURL_LAST;
    char *value = NULL;
    W("Deprecated and will be removed! Use curl_easy_getinfo instead.");
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(value));
}

EXTENSION(pg_curl_easy_getinfo_content_type) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_CONTENT_TYPE); }
EXTENSION(pg_curl_easy_getinfo_effective_url) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_EFFECTIVE_URL); }
EXTENSION(pg_curl_easy_getinfo_ftp_entry_path) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_FTP_ENTRY_PATH); }
EXTENSION(pg_curl_easy_getinfo_local_ip) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_LOCAL_IP); }
EXTENSION(pg_curl_easy_getinfo_primary_ip) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_PRIMARY_IP); }
EXTENSION(pg_curl_easy_getinfo_private) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_PRIVATE); }
EXTENSION(pg_curl_easy_getinfo_redirect_url) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_REDIRECT_URL); }
EXTENSION(pg_curl_easy_getinfo_rtsp_session_id) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_RTSP_SESSION_ID); }
EXTENSION(pg_curl_easy_getinfo_scheme) { return pg_curl_easy_getinfo_char(fcinfo, CURLINFO_SCHEME); }

EXTENSION(pg_curl_easy_getinfo_long2) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    long value;
    W("Deprecated and will be removed! Use curl_easy_getinfo instead.");
    if (PG_ARGISNULL(0)) E("info is null!");
    info = PG_GETARG_INT32(0);
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    PG_RETURN_INT64(value);
}

static Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS, CURLINFO info) {
    CURLcode res = CURL_LAST;
    long value;
    W("Deprecated and will be removed! Use curl_easy_getinfo instead.");
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    PG_RETURN_INT64(value);
}

EXTENSION(pg_curl_easy_getinfo_condition_unmet) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_CONDITION_UNMET); }
EXTENSION(pg_curl_easy_getinfo_filetime) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_FILETIME); }
EXTENSION(pg_curl_easy_getinfo_header_size) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HEADER_SIZE); }
EXTENSION(pg_curl_easy_getinfo_httpauth_avail) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTPAUTH_AVAIL); }
EXTENSION(pg_curl_easy_getinfo_http_connectcode) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTP_CONNECTCODE); }
EXTENSION(pg_curl_easy_getinfo_http_version) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_HTTP_VERSION); }
EXTENSION(pg_curl_easy_getinfo_lastsocket) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_LASTSOCKET); }
EXTENSION(pg_curl_easy_getinfo_local_port) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_LOCAL_PORT); }
EXTENSION(pg_curl_easy_getinfo_num_connects) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_NUM_CONNECTS); }
EXTENSION(pg_curl_easy_getinfo_os_errno) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_OS_ERRNO); }
EXTENSION(pg_curl_easy_getinfo_primary_port) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PRIMARY_PORT); }
EXTENSION(pg_curl_easy_getinfo_protocol) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROTOCOL); }
EXTENSION(pg_curl_easy_getinfo_proxyauth_avail) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROXYAUTH_AVAIL); }
EXTENSION(pg_curl_easy_getinfo_proxy_ssl_verifyresult) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_PROXY_SSL_VERIFYRESULT); }
EXTENSION(pg_curl_easy_getinfo_redirect_count) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_REDIRECT_COUNT); }
EXTENSION(pg_curl_easy_getinfo_request_size) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_REQUEST_SIZE); }
EXTENSION(pg_curl_easy_getinfo_response_code) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RESPONSE_CODE); }
EXTENSION(pg_curl_easy_getinfo_rtsp_client_cseq) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_CLIENT_CSEQ); }
EXTENSION(pg_curl_easy_getinfo_rtsp_cseq_recv) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_CSEQ_RECV); }
EXTENSION(pg_curl_easy_getinfo_rtsp_server_cseq) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_RTSP_SERVER_CSEQ); }
EXTENSION(pg_curl_easy_getinfo_ssl_verifyresult) { return pg_curl_easy_getinfo_long(fcinfo, CURLINFO_SSL_VERIFYRESULT); }

EXTENSION(pg_curlopt_abstract_unix_socket) { PG_RETURN_INT32(CURLOPT_ABSTRACT_UNIX_SOCKET); }
EXTENSION(pg_curlopt_accept_encoding) { PG_RETURN_INT32(CURLOPT_ACCEPT_ENCODING); }
EXTENSION(pg_curlopt_cainfo) { PG_RETURN_INT32(CURLOPT_CAINFO); }
EXTENSION(pg_curlopt_capath) { PG_RETURN_INT32(CURLOPT_CAPATH); }
EXTENSION(pg_curlopt_cookiefile) { PG_RETURN_INT32(CURLOPT_COOKIEFILE); }
EXTENSION(pg_curlopt_cookiejar) { PG_RETURN_INT32(CURLOPT_COOKIEJAR); }
EXTENSION(pg_curlopt_cookielist) { PG_RETURN_INT32(CURLOPT_COOKIELIST); }
EXTENSION(pg_curlopt_cookie) { PG_RETURN_INT32(CURLOPT_COOKIE); }
EXTENSION(pg_curlopt_crlfile) { PG_RETURN_INT32(CURLOPT_CRLFILE); }
EXTENSION(pg_curlopt_customrequest) { PG_RETURN_INT32(CURLOPT_CUSTOMREQUEST); }
EXTENSION(pg_curlopt_default_protocol) { PG_RETURN_INT32(CURLOPT_DEFAULT_PROTOCOL); }
EXTENSION(pg_curlopt_dns_interface) { PG_RETURN_INT32(CURLOPT_DNS_INTERFACE); }
EXTENSION(pg_curlopt_dns_local_ip4) { PG_RETURN_INT32(CURLOPT_DNS_LOCAL_IP4); }
EXTENSION(pg_curlopt_dns_local_ip6) { PG_RETURN_INT32(CURLOPT_DNS_LOCAL_IP6); }
EXTENSION(pg_curlopt_dns_servers) { PG_RETURN_INT32(CURLOPT_DNS_SERVERS); }
EXTENSION(pg_curlopt_doh_url) { PG_RETURN_INT32(CURLOPT_DOH_URL); }
EXTENSION(pg_curlopt_egdsocket) { PG_RETURN_INT32(CURLOPT_EGDSOCKET); }
EXTENSION(pg_curlopt_ftp_account) { PG_RETURN_INT32(CURLOPT_FTP_ACCOUNT); }
EXTENSION(pg_curlopt_ftp_alternative_to_user) { PG_RETURN_INT32(CURLOPT_FTP_ALTERNATIVE_TO_USER); }
EXTENSION(pg_curlopt_ftpport) { PG_RETURN_INT32(CURLOPT_FTPPORT); }
EXTENSION(pg_curlopt_interface) { PG_RETURN_INT32(CURLOPT_INTERFACE); }
EXTENSION(pg_curlopt_issuercert) { PG_RETURN_INT32(CURLOPT_ISSUERCERT); }
EXTENSION(pg_curlopt_keypasswd) { PG_RETURN_INT32(CURLOPT_KEYPASSWD); }
EXTENSION(pg_curlopt_krblevel) { PG_RETURN_INT32(CURLOPT_KRBLEVEL); }
EXTENSION(pg_curlopt_login_options) { PG_RETURN_INT32(CURLOPT_LOGIN_OPTIONS); }
EXTENSION(pg_curlopt_mail_auth) { PG_RETURN_INT32(CURLOPT_MAIL_AUTH); }
EXTENSION(pg_curlopt_mail_from) { PG_RETURN_INT32(CURLOPT_MAIL_FROM); }
EXTENSION(pg_curlopt_noproxy) { PG_RETURN_INT32(CURLOPT_NOPROXY); }
EXTENSION(pg_curlopt_password) { PG_RETURN_INT32(CURLOPT_PASSWORD); }
EXTENSION(pg_curlopt_pinnedpublickey) { PG_RETURN_INT32(CURLOPT_PINNEDPUBLICKEY); }
EXTENSION(pg_curlopt_pre_proxy) { PG_RETURN_INT32(CURLOPT_PRE_PROXY); }
EXTENSION(pg_curlopt_proxy_cainfo) { PG_RETURN_INT32(CURLOPT_PROXY_CAINFO); }
EXTENSION(pg_curlopt_proxy_capath) { PG_RETURN_INT32(CURLOPT_PROXY_CAPATH); }
EXTENSION(pg_curlopt_proxy_crlfile) { PG_RETURN_INT32(CURLOPT_PROXY_CRLFILE); }
EXTENSION(pg_curlopt_proxy_keypasswd) { PG_RETURN_INT32(CURLOPT_PROXY_KEYPASSWD); }
EXTENSION(pg_curlopt_proxypassword) { PG_RETURN_INT32(CURLOPT_PROXYPASSWORD); }
EXTENSION(pg_curlopt_proxy_pinnedpublickey) { PG_RETURN_INT32(CURLOPT_PROXY_PINNEDPUBLICKEY); }
EXTENSION(pg_curlopt_proxy_service_name) { PG_RETURN_INT32(CURLOPT_PROXY_SERVICE_NAME); }
EXTENSION(pg_curlopt_proxy) { PG_RETURN_INT32(CURLOPT_PROXY); }
EXTENSION(pg_curlopt_proxy_sslcert) { PG_RETURN_INT32(CURLOPT_PROXY_SSLCERT); }
EXTENSION(pg_curlopt_proxy_sslcerttype) { PG_RETURN_INT32(CURLOPT_PROXY_SSLCERTTYPE); }
EXTENSION(pg_curlopt_proxy_ssl_cipher_list) { PG_RETURN_INT32(CURLOPT_PROXY_SSL_CIPHER_LIST); }
EXTENSION(pg_curlopt_proxy_sslkey) { PG_RETURN_INT32(CURLOPT_PROXY_SSLKEY); }
EXTENSION(pg_curlopt_proxy_sslkeytype) { PG_RETURN_INT32(CURLOPT_PROXY_SSLKEYTYPE); }
EXTENSION(pg_curlopt_proxy_tls13_ciphers) { PG_RETURN_INT32(CURLOPT_PROXY_TLS13_CIPHERS); }
EXTENSION(pg_curlopt_proxy_tlsauth_password) { PG_RETURN_INT32(CURLOPT_PROXY_TLSAUTH_PASSWORD); }
EXTENSION(pg_curlopt_proxy_tlsauth_type) { PG_RETURN_INT32(CURLOPT_PROXY_TLSAUTH_TYPE); }
EXTENSION(pg_curlopt_proxy_tlsauth_username) { PG_RETURN_INT32(CURLOPT_PROXY_TLSAUTH_USERNAME); }
EXTENSION(pg_curlopt_proxyusername) { PG_RETURN_INT32(CURLOPT_PROXYUSERNAME); }
EXTENSION(pg_curlopt_proxyuserpwd) { PG_RETURN_INT32(CURLOPT_PROXYUSERPWD); }
EXTENSION(pg_curlopt_random_file) { PG_RETURN_INT32(CURLOPT_RANDOM_FILE); }
EXTENSION(pg_curlopt_range) { PG_RETURN_INT32(CURLOPT_RANGE); }
EXTENSION(pg_curlopt_referer) { PG_RETURN_INT32(CURLOPT_REFERER); }
EXTENSION(pg_curlopt_request_target) { PG_RETURN_INT32(CURLOPT_REQUEST_TARGET); }
EXTENSION(pg_curlopt_rtsp_session_id) { PG_RETURN_INT32(CURLOPT_RTSP_SESSION_ID); }
EXTENSION(pg_curlopt_rtsp_stream_uri) { PG_RETURN_INT32(CURLOPT_RTSP_STREAM_URI); }
EXTENSION(pg_curlopt_rtsp_transport) { PG_RETURN_INT32(CURLOPT_RTSP_TRANSPORT); }
EXTENSION(pg_curlopt_service_name) { PG_RETURN_INT32(CURLOPT_SERVICE_NAME); }
EXTENSION(pg_curlopt_socks5_gssapi_service) { PG_RETURN_INT32(CURLOPT_SOCKS5_GSSAPI_SERVICE); }
EXTENSION(pg_curlopt_ssh_host_public_key_md5) { PG_RETURN_INT32(CURLOPT_SSH_HOST_PUBLIC_KEY_MD5); }
EXTENSION(pg_curlopt_ssh_knownhosts) { PG_RETURN_INT32(CURLOPT_SSH_KNOWNHOSTS); }
EXTENSION(pg_curlopt_ssh_private_keyfile) { PG_RETURN_INT32(CURLOPT_SSH_PRIVATE_KEYFILE); }
EXTENSION(pg_curlopt_ssh_public_keyfile) { PG_RETURN_INT32(CURLOPT_SSH_PUBLIC_KEYFILE); }
EXTENSION(pg_curlopt_sslcert) { PG_RETURN_INT32(CURLOPT_SSLCERT); }
EXTENSION(pg_curlopt_sslcerttype) { PG_RETURN_INT32(CURLOPT_SSLCERTTYPE); }
EXTENSION(pg_curlopt_ssl_cipher_list) { PG_RETURN_INT32(CURLOPT_SSL_CIPHER_LIST); }
EXTENSION(pg_curlopt_sslengine) { PG_RETURN_INT32(CURLOPT_SSLENGINE); }
EXTENSION(pg_curlopt_sslkey) { PG_RETURN_INT32(CURLOPT_SSLKEY); }
EXTENSION(pg_curlopt_sslkeytype) { PG_RETURN_INT32(CURLOPT_SSLKEYTYPE); }
EXTENSION(pg_curlopt_tls13_ciphers) { PG_RETURN_INT32(CURLOPT_TLS13_CIPHERS); }
EXTENSION(pg_curlopt_tlsauth_password) { PG_RETURN_INT32(CURLOPT_TLSAUTH_PASSWORD); }
EXTENSION(pg_curlopt_tlsauth_type) { PG_RETURN_INT32(CURLOPT_TLSAUTH_TYPE); }
EXTENSION(pg_curlopt_tlsauth_username) { PG_RETURN_INT32(CURLOPT_TLSAUTH_USERNAME); }
EXTENSION(pg_curlopt_unix_socket_path) { PG_RETURN_INT32(CURLOPT_UNIX_SOCKET_PATH); }
EXTENSION(pg_curlopt_url) { PG_RETURN_INT32(CURLOPT_URL); }
EXTENSION(pg_curlopt_useragent) { PG_RETURN_INT32(CURLOPT_USERAGENT); }
EXTENSION(pg_curlopt_username) { PG_RETURN_INT32(CURLOPT_USERNAME); }
EXTENSION(pg_curlopt_userpwd) { PG_RETURN_INT32(CURLOPT_USERPWD); }
EXTENSION(pg_curlopt_xoauth2_bearer) { PG_RETURN_INT32(CURLOPT_XOAUTH2_BEARER); }

EXTENSION(pg_curlopt_accepttimeout_ms) { PG_RETURN_INT32(CURLOPT_ACCEPTTIMEOUT_MS); }
EXTENSION(pg_curlopt_address_scope) { PG_RETURN_INT32(CURLOPT_ADDRESS_SCOPE); }
EXTENSION(pg_curlopt_append) { PG_RETURN_INT32(CURLOPT_APPEND); }
EXTENSION(pg_curlopt_autoreferer) { PG_RETURN_INT32(CURLOPT_AUTOREFERER); }
EXTENSION(pg_curlopt_buffersize) { PG_RETURN_INT32(CURLOPT_BUFFERSIZE); }
EXTENSION(pg_curlopt_certinfo) { PG_RETURN_INT32(CURLOPT_CERTINFO); }
EXTENSION(pg_curlopt_connect_only) { PG_RETURN_INT32(CURLOPT_CONNECT_ONLY); }
EXTENSION(pg_curlopt_connecttimeout_ms) { PG_RETURN_INT32(CURLOPT_CONNECTTIMEOUT_MS); }
EXTENSION(pg_curlopt_connecttimeout) { PG_RETURN_INT32(CURLOPT_CONNECTTIMEOUT); }
EXTENSION(pg_curlopt_cookiesession) { PG_RETURN_INT32(CURLOPT_COOKIESESSION); }
EXTENSION(pg_curlopt_crlf) { PG_RETURN_INT32(CURLOPT_CRLF); }
EXTENSION(pg_curlopt_dirlistonly) { PG_RETURN_INT32(CURLOPT_DIRLISTONLY); }
EXTENSION(pg_curlopt_disallow_username_in_url) { PG_RETURN_INT32(CURLOPT_DISALLOW_USERNAME_IN_URL); }
EXTENSION(pg_curlopt_dns_cache_timeout) { PG_RETURN_INT32(CURLOPT_DNS_CACHE_TIMEOUT); }
EXTENSION(pg_curlopt_dns_shuffle_addresses) { PG_RETURN_INT32(CURLOPT_DNS_SHUFFLE_ADDRESSES); }
EXTENSION(pg_curlopt_dns_use_global_cache) { PG_RETURN_INT32(CURLOPT_DNS_USE_GLOBAL_CACHE); }
EXTENSION(pg_curlopt_expect_100_timeout_ms) { PG_RETURN_INT32(CURLOPT_EXPECT_100_TIMEOUT_MS); }
EXTENSION(pg_curlopt_failonerror) { PG_RETURN_INT32(CURLOPT_FAILONERROR); }
EXTENSION(pg_curlopt_filetime) { PG_RETURN_INT32(CURLOPT_FILETIME); }
EXTENSION(pg_curlopt_followlocation) { PG_RETURN_INT32(CURLOPT_FOLLOWLOCATION); }
EXTENSION(pg_curlopt_forbid_reuse) { PG_RETURN_INT32(CURLOPT_FORBID_REUSE); }
EXTENSION(pg_curlopt_fresh_connect) { PG_RETURN_INT32(CURLOPT_FRESH_CONNECT); }
EXTENSION(pg_curlopt_ftp_create_missing_dirs) { PG_RETURN_INT32(CURLOPT_FTP_CREATE_MISSING_DIRS); }
EXTENSION(pg_curlopt_ftp_filemethod) { PG_RETURN_INT32(CURLOPT_FTP_FILEMETHOD); }
EXTENSION(pg_curlopt_ftp_skip_pasv_ip) { PG_RETURN_INT32(CURLOPT_FTP_SKIP_PASV_IP); }
EXTENSION(pg_curlopt_ftpsslauth) { PG_RETURN_INT32(CURLOPT_FTPSSLAUTH); }
EXTENSION(pg_curlopt_ftp_ssl_ccc) { PG_RETURN_INT32(CURLOPT_FTP_SSL_CCC); }
EXTENSION(pg_curlopt_ftp_use_eprt) { PG_RETURN_INT32(CURLOPT_FTP_USE_EPRT); }
EXTENSION(pg_curlopt_ftp_use_epsv) { PG_RETURN_INT32(CURLOPT_FTP_USE_EPSV); }
EXTENSION(pg_curlopt_ftp_use_pret) { PG_RETURN_INT32(CURLOPT_FTP_USE_PRET); }
EXTENSION(pg_curlopt_gssapi_delegation) { PG_RETURN_INT32(CURLOPT_GSSAPI_DELEGATION); }
EXTENSION(pg_curlopt_happy_eyeballs_timeout_ms) { PG_RETURN_INT32(CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS); }
EXTENSION(pg_curlopt_haproxyprotocol) { PG_RETURN_INT32(CURLOPT_HAPROXYPROTOCOL); }
EXTENSION(pg_curlopt_header) { PG_RETURN_INT32(CURLOPT_HEADER); }
EXTENSION(pg_curlopt_http09_allowed) { PG_RETURN_INT32(CURLOPT_HTTP09_ALLOWED); }
EXTENSION(pg_curlopt_httpauth) { PG_RETURN_INT32(CURLOPT_HTTPAUTH); }
EXTENSION(pg_curlopt_http_content_decoding) { PG_RETURN_INT32(CURLOPT_HTTP_CONTENT_DECODING); }
EXTENSION(pg_curlopt_httpget) { PG_RETURN_INT32(CURLOPT_HTTPGET); }
EXTENSION(pg_curlopt_httpproxytunnel) { PG_RETURN_INT32(CURLOPT_HTTPPROXYTUNNEL); }
EXTENSION(pg_curlopt_http_transfer_decoding) { PG_RETURN_INT32(CURLOPT_HTTP_TRANSFER_DECODING); }
EXTENSION(pg_curlopt_http_version) { PG_RETURN_INT32(CURLOPT_HTTP_VERSION); }
EXTENSION(pg_curlopt_ignore_content_length) { PG_RETURN_INT32(CURLOPT_IGNORE_CONTENT_LENGTH); }
EXTENSION(pg_curlopt_ipresolve) { PG_RETURN_INT32(CURLOPT_IPRESOLVE); }
EXTENSION(pg_curlopt_keep_sending_on_error) { PG_RETURN_INT32(CURLOPT_KEEP_SENDING_ON_ERROR); }
EXTENSION(pg_curlopt_localportrange) { PG_RETURN_INT32(CURLOPT_LOCALPORTRANGE); }
EXTENSION(pg_curlopt_localport) { PG_RETURN_INT32(CURLOPT_LOCALPORT); }
EXTENSION(pg_curlopt_low_speed_limit) { PG_RETURN_INT32(CURLOPT_LOW_SPEED_LIMIT); }
EXTENSION(pg_curlopt_low_speed_time) { PG_RETURN_INT32(CURLOPT_LOW_SPEED_TIME); }
EXTENSION(pg_curlopt_maxconnects) { PG_RETURN_INT32(CURLOPT_MAXCONNECTS); }
EXTENSION(pg_curlopt_maxfilesize) { PG_RETURN_INT32(CURLOPT_MAXFILESIZE); }
EXTENSION(pg_curlopt_maxredirs) { PG_RETURN_INT32(CURLOPT_MAXREDIRS); }
EXTENSION(pg_curlopt_netrc) { PG_RETURN_INT32(CURLOPT_NETRC); }
EXTENSION(pg_curlopt_new_directory_perms) { PG_RETURN_INT32(CURLOPT_NEW_DIRECTORY_PERMS); }
EXTENSION(pg_curlopt_new_file_perms) { PG_RETURN_INT32(CURLOPT_NEW_FILE_PERMS); }
EXTENSION(pg_curlopt_nobody) { PG_RETURN_INT32(CURLOPT_NOBODY); }
EXTENSION(pg_curlopt_nosignal) { PG_RETURN_INT32(CURLOPT_NOSIGNAL); }
EXTENSION(pg_curlopt_path_as_is) { PG_RETURN_INT32(CURLOPT_PATH_AS_IS); }
EXTENSION(pg_curlopt_pipewait) { PG_RETURN_INT32(CURLOPT_PIPEWAIT); }
EXTENSION(pg_curlopt_port) { PG_RETURN_INT32(CURLOPT_PORT); }
EXTENSION(pg_curlopt_postredir) { PG_RETURN_INT32(CURLOPT_POSTREDIR); }
EXTENSION(pg_curlopt_post) { PG_RETURN_INT32(CURLOPT_POST); }
EXTENSION(pg_curlopt_protocols) { PG_RETURN_INT32(CURLOPT_PROTOCOLS); }
EXTENSION(pg_curlopt_proxyauth) { PG_RETURN_INT32(CURLOPT_PROXYAUTH); }
EXTENSION(pg_curlopt_proxyport) { PG_RETURN_INT32(CURLOPT_PROXYPORT); }
EXTENSION(pg_curlopt_proxy_ssl_options) { PG_RETURN_INT32(CURLOPT_PROXY_SSL_OPTIONS); }
EXTENSION(pg_curlopt_proxy_ssl_verifyhost) { PG_RETURN_INT32(CURLOPT_PROXY_SSL_VERIFYHOST); }
EXTENSION(pg_curlopt_proxy_ssl_verifypeer) { PG_RETURN_INT32(CURLOPT_PROXY_SSL_VERIFYPEER); }
EXTENSION(pg_curlopt_proxy_sslversion) { PG_RETURN_INT32(CURLOPT_PROXY_SSLVERSION); }
EXTENSION(pg_curlopt_proxy_transfer_mode) { PG_RETURN_INT32(CURLOPT_PROXY_TRANSFER_MODE); }
EXTENSION(pg_curlopt_proxytype) { PG_RETURN_INT32(CURLOPT_PROXYTYPE); }
EXTENSION(pg_curlopt_put) { PG_RETURN_INT32(CURLOPT_PUT); }
EXTENSION(pg_curlopt_redir_protocols) { PG_RETURN_INT32(CURLOPT_REDIR_PROTOCOLS); }
EXTENSION(pg_curlopt_resume_from) { PG_RETURN_INT32(CURLOPT_RESUME_FROM); }
EXTENSION(pg_curlopt_rtsp_client_cseq) { PG_RETURN_INT32(CURLOPT_RTSP_CLIENT_CSEQ); }
EXTENSION(pg_curlopt_rtsp_request) { PG_RETURN_INT32(CURLOPT_RTSP_REQUEST); }
EXTENSION(pg_curlopt_rtsp_server_cseq) { PG_RETURN_INT32(CURLOPT_RTSP_SERVER_CSEQ); }
EXTENSION(pg_curlopt_sasl_ir) { PG_RETURN_INT32(CURLOPT_SASL_IR); }
EXTENSION(pg_curlopt_server_response_timeout) { PG_RETURN_INT32(CURLOPT_SERVER_RESPONSE_TIMEOUT); }
EXTENSION(pg_curlopt_socks5_auth) { PG_RETURN_INT32(CURLOPT_SOCKS5_AUTH); }
EXTENSION(pg_curlopt_socks5_gssapi_nec) { PG_RETURN_INT32(CURLOPT_SOCKS5_GSSAPI_NEC); }
EXTENSION(pg_curlopt_ssh_auth_types) { PG_RETURN_INT32(CURLOPT_SSH_AUTH_TYPES); }
EXTENSION(pg_curlopt_ssh_compression) { PG_RETURN_INT32(CURLOPT_SSH_COMPRESSION); }
EXTENSION(pg_curlopt_ssl_enable_alpn) { PG_RETURN_INT32(CURLOPT_SSL_ENABLE_ALPN); }
EXTENSION(pg_curlopt_ssl_enable_npn) { PG_RETURN_INT32(CURLOPT_SSL_ENABLE_NPN); }
EXTENSION(pg_curlopt_ssl_falsestart) { PG_RETURN_INT32(CURLOPT_SSL_FALSESTART); }
EXTENSION(pg_curlopt_ssl_options) { PG_RETURN_INT32(CURLOPT_SSL_OPTIONS); }
EXTENSION(pg_curlopt_ssl_sessionid_cache) { PG_RETURN_INT32(CURLOPT_SSL_SESSIONID_CACHE); }
EXTENSION(pg_curlopt_ssl_verifyhost) { PG_RETURN_INT32(CURLOPT_SSL_VERIFYHOST); }
EXTENSION(pg_curlopt_ssl_verifypeer) { PG_RETURN_INT32(CURLOPT_SSL_VERIFYPEER); }
EXTENSION(pg_curlopt_ssl_verifystatus) { PG_RETURN_INT32(CURLOPT_SSL_VERIFYSTATUS); }
EXTENSION(pg_curlopt_sslversion) { PG_RETURN_INT32(CURLOPT_SSLVERSION); }
EXTENSION(pg_curlopt_stream_weight) { PG_RETURN_INT32(CURLOPT_STREAM_WEIGHT); }
EXTENSION(pg_curlopt_suppress_connect_headers) { PG_RETURN_INT32(CURLOPT_SUPPRESS_CONNECT_HEADERS); }
EXTENSION(pg_curlopt_tcp_fastopen) { PG_RETURN_INT32(CURLOPT_TCP_FASTOPEN); }
EXTENSION(pg_curlopt_tcp_keepalive) { PG_RETURN_INT32(CURLOPT_TCP_KEEPALIVE); }
EXTENSION(pg_curlopt_tcp_keepidle) { PG_RETURN_INT32(CURLOPT_TCP_KEEPIDLE); }
EXTENSION(pg_curlopt_tcp_keepintvl) { PG_RETURN_INT32(CURLOPT_TCP_KEEPINTVL); }
EXTENSION(pg_curlopt_tcp_nodelay) { PG_RETURN_INT32(CURLOPT_TCP_NODELAY); }
EXTENSION(pg_curlopt_tftp_blksize) { PG_RETURN_INT32(CURLOPT_TFTP_BLKSIZE); }
EXTENSION(pg_curlopt_tftp_no_options) { PG_RETURN_INT32(CURLOPT_TFTP_NO_OPTIONS); }
EXTENSION(pg_curlopt_timecondition) { PG_RETURN_INT32(CURLOPT_TIMECONDITION); }
EXTENSION(pg_curlopt_timeout_ms) { PG_RETURN_INT32(CURLOPT_TIMEOUT_MS); }
EXTENSION(pg_curlopt_timeout) { PG_RETURN_INT32(CURLOPT_TIMEOUT); }
EXTENSION(pg_curlopt_timevalue) { PG_RETURN_INT32(CURLOPT_TIMEVALUE); }
EXTENSION(pg_curlopt_transfer_encoding) { PG_RETURN_INT32(CURLOPT_TRANSFER_ENCODING); }
EXTENSION(pg_curlopt_transfertext) { PG_RETURN_INT32(CURLOPT_TRANSFERTEXT); }
EXTENSION(pg_curlopt_unrestricted_auth) { PG_RETURN_INT32(CURLOPT_UNRESTRICTED_AUTH); }
EXTENSION(pg_curlopt_upkeep_interval_ms) { PG_RETURN_INT32(CURLOPT_UPKEEP_INTERVAL_MS); }
EXTENSION(pg_curlopt_upload_buffersize) { PG_RETURN_INT32(CURLOPT_UPLOAD_BUFFERSIZE); }
EXTENSION(pg_curlopt_use_ssl) { PG_RETURN_INT32(CURLOPT_USE_SSL); }
EXTENSION(pg_curlopt_verbose) { PG_RETURN_INT32(CURLOPT_VERBOSE); }
EXTENSION(pg_curlopt_wildcardmatch) { PG_RETURN_INT32(CURLOPT_WILDCARDMATCH); }

EXTENSION(pg_curl_http_version_1_0) { PG_RETURN_INT64(CURL_HTTP_VERSION_1_0); }
EXTENSION(pg_curl_http_version_1_1) { PG_RETURN_INT64(CURL_HTTP_VERSION_1_1); }
EXTENSION(pg_curl_http_version_2_0) { PG_RETURN_INT64(CURL_HTTP_VERSION_2_0); }
EXTENSION(pg_curl_http_version_2_prior_knowledge) { PG_RETURN_INT64(CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE); }
EXTENSION(pg_curl_http_version_2tls) { PG_RETURN_INT64(CURL_HTTP_VERSION_2TLS); }
EXTENSION(pg_curl_http_version_3) { PG_RETURN_INT64(CURL_HTTP_VERSION_3); }
EXTENSION(pg_curl_http_version_none) { PG_RETURN_INT64(CURL_HTTP_VERSION_NONE); }

EXTENSION(pg_curlusessl_none) { PG_RETURN_INT64(CURLUSESSL_NONE); }
EXTENSION(pg_curlusessl_try) { PG_RETURN_INT64(CURLUSESSL_TRY); }
EXTENSION(pg_curlusessl_control) { PG_RETURN_INT64(CURLUSESSL_CONTROL); }
EXTENSION(pg_curlusessl_all) { PG_RETURN_INT64(CURLUSESSL_ALL); }

EXTENSION(pg_curl_upkeep_interval_default) { PG_RETURN_INT64(CURL_UPKEEP_INTERVAL_DEFAULT); }

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
EXTENSION(pg_curl_sslversion_tlsv1_3) { PG_RETURN_INT64(CURL_SSLVERSION_TLSv1_3); }
EXTENSION(pg_curl_sslversion_max_default) { PG_RETURN_INT64(CURL_SSLVERSION_MAX_DEFAULT); }
EXTENSION(pg_curl_sslversion_max_tlsv1_0) { PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_0); }
EXTENSION(pg_curl_sslversion_max_tlsv1_1) { PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_1); }
EXTENSION(pg_curl_sslversion_max_tlsv1_2) { PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_2); }
EXTENSION(pg_curl_sslversion_max_tlsv1_3) { PG_RETURN_INT64(CURL_SSLVERSION_MAX_TLSv1_3); }

EXTENSION(pg_curlsslopt_allow_beast) { PG_RETURN_INT64(CURLSSLOPT_ALLOW_BEAST); }
EXTENSION(pg_curlsslopt_no_revoke) { PG_RETURN_INT64(CURLSSLOPT_NO_REVOKE); }
EXTENSION(pg_curlsslopt_no_partialchain) { PG_RETURN_INT64(CURLSSLOPT_NO_PARTIALCHAIN); }
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
EXTENSION(pg_curlauth_bearer) { PG_RETURN_INT64(CURLAUTH_BEARER); }
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
EXTENSION(pg_curlauth_gssapi) { PG_RETURN_INT64(CURLAUTH_GSSAPI); }
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
EXTENSION(pg_curlproxy_https) { PG_RETURN_INT64(CURLPROXY_HTTPS); }
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

EXTENSION(pg_curl_het_default) { PG_RETURN_INT64(CURL_HET_DEFAULT); }

EXTENSION(pg_curlgssapi_delegation_flag) { PG_RETURN_INT64(CURLGSSAPI_DELEGATION_FLAG); }
EXTENSION(pg_curlgssapi_delegation_policy_flag) { PG_RETURN_INT64(CURLGSSAPI_DELEGATION_POLICY_FLAG); }
EXTENSION(pg_curlgssapi_delegation_none) { PG_RETURN_INT64(CURLGSSAPI_DELEGATION_NONE); }

EXTENSION(pg_curlftpssl_ccc_none) { PG_RETURN_INT64(CURLFTPSSL_CCC_NONE); }
EXTENSION(pg_curlftpssl_ccc_passive) { PG_RETURN_INT64(CURLFTPSSL_CCC_PASSIVE); }
EXTENSION(pg_curlftpssl_ccc_active) { PG_RETURN_INT64(CURLFTPSSL_CCC_ACTIVE); }

EXTENSION(pg_curlftpauth_default) { PG_RETURN_INT64(CURLFTPAUTH_DEFAULT); }
EXTENSION(pg_curlftpauth_ssl) { PG_RETURN_INT64(CURLFTPAUTH_SSL); }
EXTENSION(pg_curlftpauth_tls) { PG_RETURN_INT64(CURLFTPAUTH_TLS); }

EXTENSION(pg_curlftpmethod_multicwd) { PG_RETURN_INT64(CURLFTPMETHOD_MULTICWD); }
EXTENSION(pg_curlftpmethod_nocwd) { PG_RETURN_INT64(CURLFTPMETHOD_NOCWD); }
EXTENSION(pg_curlftpmethod_singlecwd) { PG_RETURN_INT64(CURLFTPMETHOD_SINGLECWD); }

EXTENSION(pg_curlftp_create_dir) { PG_RETURN_INT64(CURLFTP_CREATE_DIR); }
EXTENSION(pg_curlftp_create_dir_retry) { PG_RETURN_INT64(CURLFTP_CREATE_DIR_RETRY); }
EXTENSION(pg_curlftp_create_dir_none) { PG_RETURN_INT64(CURLFTP_CREATE_DIR_NONE); }

EXTENSION(pg_curl_max_write_size) { PG_RETURN_INT64(CURL_MAX_WRITE_SIZE); }

EXTENSION(pg_curlinfo_content_type) { PG_RETURN_INT32(CURLINFO_CONTENT_TYPE); }
EXTENSION(pg_curlinfo_effective_url) { PG_RETURN_INT32(CURLINFO_EFFECTIVE_URL); }
EXTENSION(pg_curlinfo_ftp_entry_path) { PG_RETURN_INT32(CURLINFO_FTP_ENTRY_PATH); }
EXTENSION(pg_curlinfo_local_ip) { PG_RETURN_INT32(CURLINFO_LOCAL_IP); }
EXTENSION(pg_curlinfo_primary_ip) { PG_RETURN_INT32(CURLINFO_PRIMARY_IP); }
EXTENSION(pg_curlinfo_private) { PG_RETURN_INT32(CURLINFO_PRIVATE); }
EXTENSION(pg_curlinfo_redirect_url) { PG_RETURN_INT32(CURLINFO_REDIRECT_URL); }
EXTENSION(pg_curlinfo_rtsp_session_id) { PG_RETURN_INT32(CURLINFO_RTSP_SESSION_ID); }
EXTENSION(pg_curlinfo_scheme) { PG_RETURN_INT32(CURLINFO_SCHEME); }

EXTENSION(pg_curlinfo_condition_unmet) { PG_RETURN_INT32(CURLINFO_CONDITION_UNMET); }
EXTENSION(pg_curlinfo_filetime) { PG_RETURN_INT32(CURLINFO_FILETIME); }
EXTENSION(pg_curlinfo_header_size) { PG_RETURN_INT32(CURLINFO_HEADER_SIZE); }
EXTENSION(pg_curlinfo_httpauth_avail) { PG_RETURN_INT32(CURLINFO_HTTPAUTH_AVAIL); }
EXTENSION(pg_curlinfo_http_connectcode) { PG_RETURN_INT32(CURLINFO_HTTP_CONNECTCODE); }
EXTENSION(pg_curlinfo_http_version) { PG_RETURN_INT32(CURLINFO_HTTP_VERSION); }
EXTENSION(pg_curlinfo_lastsocket) { PG_RETURN_INT32(CURLINFO_LASTSOCKET); }
EXTENSION(pg_curlinfo_local_port) { PG_RETURN_INT32(CURLINFO_LOCAL_PORT); }
EXTENSION(pg_curlinfo_num_connects) { PG_RETURN_INT32(CURLINFO_NUM_CONNECTS); }
EXTENSION(pg_curlinfo_os_errno) { PG_RETURN_INT32(CURLINFO_OS_ERRNO); }
EXTENSION(pg_curlinfo_primary_port) { PG_RETURN_INT32(CURLINFO_PRIMARY_PORT); }
EXTENSION(pg_curlinfo_protocol) { PG_RETURN_INT32(CURLINFO_PROTOCOL); }
EXTENSION(pg_curlinfo_proxyauth_avail) { PG_RETURN_INT32(CURLINFO_PROXYAUTH_AVAIL); }
EXTENSION(pg_curlinfo_proxy_ssl_verifyresult) { PG_RETURN_INT32(CURLINFO_PROXY_SSL_VERIFYRESULT); }
EXTENSION(pg_curlinfo_redirect_count) { PG_RETURN_INT32(CURLINFO_REDIRECT_COUNT); }
EXTENSION(pg_curlinfo_request_size) { PG_RETURN_INT32(CURLINFO_REQUEST_SIZE); }
EXTENSION(pg_curlinfo_response_code) { PG_RETURN_INT32(CURLINFO_RESPONSE_CODE); }
EXTENSION(pg_curlinfo_rtsp_client_cseq) { PG_RETURN_INT32(CURLINFO_RTSP_CLIENT_CSEQ); }
EXTENSION(pg_curlinfo_rtsp_cseq_recv) { PG_RETURN_INT32(CURLINFO_RTSP_CSEQ_RECV); }
EXTENSION(pg_curlinfo_rtsp_server_cseq) { PG_RETURN_INT32(CURLINFO_RTSP_SERVER_CSEQ); }
EXTENSION(pg_curlinfo_ssl_verifyresult) { PG_RETURN_INT32(CURLINFO_SSL_VERIFYRESULT); }

#include <postgres.h>

#include <catalog/pg_type.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#include <signal.h>
#include <utils/builtins.h>

#include <curl/curl.h>

#define EXTENSION(function) Datum (function)(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(function); Datum (function)(PG_FUNCTION_ARGS)

#define FORMAT_0(fmt, ...) "%s(%s:%d): %s", __func__, __FILE__, __LINE__, fmt
#define FORMAT_1(fmt, ...) "%s(%s:%d): " fmt,  __func__, __FILE__, __LINE__
#define GET_FORMAT(fmt, ...) GET_FORMAT_PRIVATE(fmt, 0, ##__VA_ARGS__, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
#define GET_FORMAT_PRIVATE(fmt, \
      _0,  _1,  _2,  _3,  _4,  _5,  _6,  _7,  _8,  _9, \
     _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, \
     _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, \
     _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, \
     _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, \
     _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, \
     _60, _61, _62, _63, _64, _65, _66, _67, _68, _69, \
     _70, format, ...) FORMAT_ ## format(fmt)

#define D1(fmt, ...) ereport(DEBUG1, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D2(fmt, ...) ereport(DEBUG2, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D3(fmt, ...) ereport(DEBUG3, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D4(fmt, ...) ereport(DEBUG4, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D5(fmt, ...) ereport(DEBUG5, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define E(fmt, ...) ereport(ERROR, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define F(fmt, ...) ereport(FATAL, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define I(fmt, ...) ereport(INFO, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define L(fmt, ...) ereport(LOG, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define N(fmt, ...) ereport(NOTICE, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define W(fmt, ...) ereport(WARNING, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))

PG_MODULE_MAGIC;

typedef struct FileString {
    char *data;
    FILE *file;
    size_t len;
} FileString;

static bool has_mime;
static char errbuf[CURL_ERROR_SIZE];
static CURL *curl = NULL;
static curl_mime *mime;
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
    if (!(curl_mime_init(curl))) E("!curl_mime_init");
    has_mime = false;
    pg_curl_interrupt_requested = 0;
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
}

void _PG_fini(void); void _PG_fini(void) {
    pqsignal(SIGINT, pgsql_interrupt_handler);
    curl_easy_cleanup(curl);
    curl_mime_free(mime);
    curl_slist_free_all(header);
    curl_slist_free_all(recipient);
    curl_global_cleanup();
    if (header_str.data) { free(header_str.data); header_str.data = NULL; }
    if (read_str_file) { fclose(read_str_file); read_str_file = NULL; }
    if (write_str.data) { free(write_str.data); write_str.data = NULL; }
}

EXTENSION(pg_curl_easy_reset) {
    curl_easy_reset(curl);
    curl_mime_free(mime);
    curl_slist_free_all(header);
    curl_slist_free_all(recipient);
    header = NULL;
    recipient = NULL;
    if (!(mime = curl_mime_init(curl))) E("!curl_mime_init");
    has_mime = false;
    if (header_str.data) { free(header_str.data); header_str.data = NULL; }
    if (read_str_file) { fclose(read_str_file); read_str_file = NULL; }
    if (write_str.data) { free(write_str.data); write_str.data = NULL; }
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
}

EXTENSION(pg_curl_mime_data_bytea) {
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
}

EXTENSION(pg_curl_mime_file) {
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
    CURLcode res = CURL_LAST;
    int try;
    long sleep;
    if (PG_ARGISNULL(0)) E("try is null!");
    try = PG_GETARG_INT32(0);
    if (try <= 0) E("try <= 0!");
    if (PG_ARGISNULL(1)) E("sleep is null!");
    sleep = PG_GETARG_INT64(1);
    if (sleep < 0) E("sleep < 0!");
    while (try--) {
        errbuf[0] = 0;
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
        if (has_mime && ((res = curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_MIMEPOST): %s", curl_easy_strerror(res));
        pg_curl_interrupt_requested = 0;
        switch (res = curl_easy_perform(curl)) {
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
            default: {
                if (try) {
                    if (strlen(errbuf)) W("curl_easy_perform: %s: %s", curl_easy_strerror(res), errbuf);
                    else W("curl_easy_perform: %s", curl_easy_strerror(res));
                    if (sleep) pg_usleep(sleep);
                } else {
                    if (strlen(errbuf)) E("curl_easy_perform: %s: %s", curl_easy_strerror(res), errbuf);
                    else E("curl_easy_perform: %s", curl_easy_strerror(res));
                }
            }
        }
        fclose(header_str.file);
        fclose(write_str.file);
    }
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

static Datum pg_curl_easy_getinfo_char(PG_FUNCTION_ARGS, CURLINFO info) {
    CURLcode res = CURL_LAST;
    char *value = NULL;
    int len;
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%i): %s", info, curl_easy_strerror(res));
    len = value ? strlen(value) : 0;
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(value, len));
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

static Datum pg_curl_easy_getinfo_long(PG_FUNCTION_ARGS, CURLINFO info) {
    CURLcode res = CURL_LAST;
    long value;
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

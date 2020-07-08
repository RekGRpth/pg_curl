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
static CURL *curl = NULL;
static curl_mime *mime;
static FileString header_str = {NULL, NULL, 0};
static FileString read_str = {NULL, NULL, 0};
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
    if (read_str.data) { free(read_str.data); read_str.data = NULL; }
    if (read_str.file) { fclose(read_str.file); read_str.file = NULL; }
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
    if (read_str.data) { free(read_str.data); read_str.data = NULL; }
    if (read_str.file) { fclose(read_str.file); read_str.file = NULL; }
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

EXTENSION(pg_curl_easy_setopt_char) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *name, *value;
    if (PG_ARGISNULL(0)) E("option is null!");
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) E("parameter is null!");
    if (false);
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ABSTRACT_UNIX_SOCKET")) option = CURLOPT_ABSTRACT_UNIX_SOCKET;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ACCEPT_ENCODING")) option = CURLOPT_ACCEPT_ENCODING;
//    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ALTSVC")) option = CURLOPT_ALTSVC;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CAINFO")) option = CURLOPT_CAINFO;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CAPATH")) option = CURLOPT_CAPATH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "COOKIEFILE")) option = CURLOPT_COOKIEFILE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "COOKIEJAR")) option = CURLOPT_COOKIEJAR;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "COOKIELIST")) option = CURLOPT_COOKIELIST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "COOKIE")) option = CURLOPT_COOKIE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "COPYPOSTFIELDS")) option = CURLOPT_COPYPOSTFIELDS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CRLFILE")) option = CURLOPT_CRLFILE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CUSTOMREQUEST")) option = CURLOPT_CUSTOMREQUEST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DEFAULT_PROTOCOL")) option = CURLOPT_DEFAULT_PROTOCOL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_INTERFACE")) option = CURLOPT_DNS_INTERFACE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_LOCAL_IP4")) option = CURLOPT_DNS_LOCAL_IP4;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_LOCAL_IP6")) option = CURLOPT_DNS_LOCAL_IP6;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_SERVERS")) option = CURLOPT_DNS_SERVERS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DOH_URL")) option = CURLOPT_DOH_URL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "EGDSOCKET")) option = CURLOPT_EGDSOCKET;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_ACCOUNT")) option = CURLOPT_FTP_ACCOUNT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_ALTERNATIVE_TO_USER")) option = CURLOPT_FTP_ALTERNATIVE_TO_USER;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTPPORT")) option = CURLOPT_FTPPORT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "INTERFACE")) option = CURLOPT_INTERFACE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ISSUERCERT")) option = CURLOPT_ISSUERCERT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "KEYPASSWD")) option = CURLOPT_KEYPASSWD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "KRBLEVEL")) option = CURLOPT_KRBLEVEL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "LOGIN_OPTIONS")) option = CURLOPT_LOGIN_OPTIONS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "MAIL_AUTH")) option = CURLOPT_MAIL_AUTH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "MAIL_FROM")) option = CURLOPT_MAIL_FROM;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "NOPROXY")) option = CURLOPT_NOPROXY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PASSWORD")) option = CURLOPT_PASSWORD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PINNEDPUBLICKEY")) option = CURLOPT_PINNEDPUBLICKEY;
//    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "POSTFIELDS")) option = CURLOPT_POSTFIELDS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PRE_PROXY")) option = CURLOPT_PRE_PROXY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_CAINFO")) option = CURLOPT_PROXY_CAINFO;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_CAPATH")) option = CURLOPT_PROXY_CAPATH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_CRLFILE")) option = CURLOPT_PROXY_CRLFILE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_KEYPASSWD")) option = CURLOPT_PROXY_KEYPASSWD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXYPASSWORD")) option = CURLOPT_PROXYPASSWORD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_PINNEDPUBLICKEY")) option = CURLOPT_PROXY_PINNEDPUBLICKEY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SERVICE_NAME")) option = CURLOPT_PROXY_SERVICE_NAME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY")) option = CURLOPT_PROXY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSLCERT")) option = CURLOPT_PROXY_SSLCERT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSLCERTTYPE")) option = CURLOPT_PROXY_SSLCERTTYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSL_CIPHER_LIST")) option = CURLOPT_PROXY_SSL_CIPHER_LIST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSLKEY")) option = CURLOPT_PROXY_SSLKEY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSLKEYTYPE")) option = CURLOPT_PROXY_SSLKEYTYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_TLS13_CIPHERS")) option = CURLOPT_PROXY_TLS13_CIPHERS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_TLSAUTH_PASSWORD")) option = CURLOPT_PROXY_TLSAUTH_PASSWORD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_TLSAUTH_TYPE")) option = CURLOPT_PROXY_TLSAUTH_TYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_TLSAUTH_USERNAME")) option = CURLOPT_PROXY_TLSAUTH_USERNAME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXYUSERNAME")) option = CURLOPT_PROXYUSERNAME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXYUSERPWD")) option = CURLOPT_PROXYUSERPWD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RANDOM_FILE")) option = CURLOPT_RANDOM_FILE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RANGE")) option = CURLOPT_RANGE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "READDATA")) {
        text *value = DatumGetTextP(PG_GETARG_DATUM(1));
        if (!read_str.file && !(read_str.file = open_memstream(&read_str.data, &read_str.len))) E("!open_memstream");
        if (fwrite(VARDATA_ANY(value), sizeof(char), VARSIZE_ANY_EXHDR(value), read_str.file) != VARSIZE_ANY_EXHDR(value)) E("fwrite");
        if (fflush(read_str.file)) E("fflush");
        if ((res = curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_UPLOAD): %s", curl_easy_strerror(res));
        goto ret;
    }
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "REFERER")) option = CURLOPT_REFERER;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "REQUEST_TARGET")) option = CURLOPT_REQUEST_TARGET;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RTSP_SESSION_ID")) option = CURLOPT_RTSP_SESSION_ID;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RTSP_STREAM_URI")) option = CURLOPT_RTSP_STREAM_URI;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RTSP_TRANSPORT")) option = CURLOPT_RTSP_TRANSPORT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SERVICE_NAME")) option = CURLOPT_SERVICE_NAME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SOCKS5_GSSAPI_SERVICE")) option = CURLOPT_SOCKS5_GSSAPI_SERVICE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSH_HOST_PUBLIC_KEY_MD5")) option = CURLOPT_SSH_HOST_PUBLIC_KEY_MD5;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSH_KNOWNHOSTS")) option = CURLOPT_SSH_KNOWNHOSTS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSH_PRIVATE_KEYFILE")) option = CURLOPT_SSH_PRIVATE_KEYFILE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSH_PUBLIC_KEYFILE")) option = CURLOPT_SSH_PUBLIC_KEYFILE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSLCERT")) option = CURLOPT_SSLCERT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSLCERTTYPE")) option = CURLOPT_SSLCERTTYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_CIPHER_LIST")) option = CURLOPT_SSL_CIPHER_LIST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSLENGINE")) option = CURLOPT_SSLENGINE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSLKEY")) option = CURLOPT_SSLKEY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSLKEYTYPE")) option = CURLOPT_SSLKEYTYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TLS13_CIPHERS")) option = CURLOPT_TLS13_CIPHERS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TLSAUTH_PASSWORD")) option = CURLOPT_TLSAUTH_PASSWORD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TLSAUTH_TYPE")) option = CURLOPT_TLSAUTH_TYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TLSAUTH_USERNAME")) option = CURLOPT_TLSAUTH_USERNAME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "UNIX_SOCKET_PATH")) option = CURLOPT_UNIX_SOCKET_PATH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "URL")) option = CURLOPT_URL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "USERAGENT")) option = CURLOPT_USERAGENT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "USERNAME")) option = CURLOPT_USERNAME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "USERPWD")) option = CURLOPT_USERPWD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "XOAUTH2_BEARER")) option = CURLOPT_XOAUTH2_BEARER;
    else E("unsupported option %s", name);
    value = TextDatumGetCString(PG_GETARG_DATUM(1));
//    L("%s = %s", name, value);
    if ((res = curl_easy_setopt(curl, option, value)) != CURLE_OK) E("curl_easy_setopt(%s, %s): %s", name, value, curl_easy_strerror(res));
    pfree(value);
ret:
    pfree(name);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_long) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *name;
    long value;
    if (PG_ARGISNULL(0)) E("option is null!");
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) E("parameter is null!");
    value = PG_GETARG_INT64(1);
//    L("%s = %li", name, value);
    if (false);
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ACCEPTTIMEOUT_MS")) option = CURLOPT_ACCEPTTIMEOUT_MS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ADDRESS_SCOPE")) option = CURLOPT_ADDRESS_SCOPE;
//    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "ALTSVC_CTRL")) option = CURLOPT_ALTSVC_CTRL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "APPEND")) option = CURLOPT_APPEND;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "AUTOREFERER")) option = CURLOPT_AUTOREFERER;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "BUFFERSIZE")) option = CURLOPT_BUFFERSIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CERTINFO")) option = CURLOPT_CERTINFO;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CONNECT_ONLY")) option = CURLOPT_CONNECT_ONLY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CONNECTTIMEOUT_MS")) option = CURLOPT_CONNECTTIMEOUT_MS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CONNECTTIMEOUT")) option = CURLOPT_CONNECTTIMEOUT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "COOKIESESSION")) option = CURLOPT_COOKIESESSION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "CRLF")) option = CURLOPT_CRLF;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DIRLISTONLY")) option = CURLOPT_DIRLISTONLY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DISALLOW_USERNAME_IN_URL")) option = CURLOPT_DISALLOW_USERNAME_IN_URL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_CACHE_TIMEOUT")) option = CURLOPT_DNS_CACHE_TIMEOUT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_SHUFFLE_ADDRESSES")) option = CURLOPT_DNS_SHUFFLE_ADDRESSES;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "DNS_USE_GLOBAL_CACHE")) option = CURLOPT_DNS_USE_GLOBAL_CACHE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "EXPECT_100_TIMEOUT_MS")) option = CURLOPT_EXPECT_100_TIMEOUT_MS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FAILONERROR")) option = CURLOPT_FAILONERROR;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FILETIME")) option = CURLOPT_FILETIME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FOLLOWLOCATION")) option = CURLOPT_FOLLOWLOCATION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FORBID_REUSE")) option = CURLOPT_FORBID_REUSE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FRESH_CONNECT")) option = CURLOPT_FRESH_CONNECT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_CREATE_MISSING_DIRS")) option = CURLOPT_FTP_CREATE_MISSING_DIRS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_FILEMETHOD")) option = CURLOPT_FTP_FILEMETHOD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_SKIP_PASV_IP")) option = CURLOPT_FTP_SKIP_PASV_IP;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTPSSLAUTH")) option = CURLOPT_FTPSSLAUTH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_SSL_CCC")) option = CURLOPT_FTP_SSL_CCC;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_USE_EPRT")) option = CURLOPT_FTP_USE_EPRT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_USE_EPSV")) option = CURLOPT_FTP_USE_EPSV;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "FTP_USE_PRET")) option = CURLOPT_FTP_USE_PRET;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "GSSAPI_DELEGATION")) option = CURLOPT_GSSAPI_DELEGATION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HAPPY_EYEBALLS_TIMEOUT_MS")) option = CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HAPROXYPROTOCOL")) option = CURLOPT_HAPROXYPROTOCOL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HEADER")) option = CURLOPT_HEADER;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTP09_ALLOWED")) option = CURLOPT_HTTP09_ALLOWED;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTPAUTH")) option = CURLOPT_HTTPAUTH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTP_CONTENT_DECODING")) option = CURLOPT_HTTP_CONTENT_DECODING;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTPGET")) option = CURLOPT_HTTPGET;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTPPROXYTUNNEL")) option = CURLOPT_HTTPPROXYTUNNEL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTP_TRANSFER_DECODING")) option = CURLOPT_HTTP_TRANSFER_DECODING;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "HTTP_VERSION")) option = CURLOPT_HTTP_VERSION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "IGNORE_CONTENT_LENGTH")) option = CURLOPT_IGNORE_CONTENT_LENGTH;
//    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "INFILESIZE")) option = CURLOPT_INFILESIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "IPRESOLVE")) option = CURLOPT_IPRESOLVE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "KEEP_SENDING_ON_ERROR")) option = CURLOPT_KEEP_SENDING_ON_ERROR;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "LOCALPORTRANGE")) option = CURLOPT_LOCALPORTRANGE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "LOCALPORT")) option = CURLOPT_LOCALPORT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "LOW_SPEED_LIMIT")) option = CURLOPT_LOW_SPEED_LIMIT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "LOW_SPEED_TIME")) option = CURLOPT_LOW_SPEED_TIME;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "MAXCONNECTS")) option = CURLOPT_MAXCONNECTS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "MAXFILESIZE")) option = CURLOPT_MAXFILESIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "MAXREDIRS")) option = CURLOPT_MAXREDIRS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "NETRC")) option = CURLOPT_NETRC;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "NEW_DIRECTORY_PERMS")) option = CURLOPT_NEW_DIRECTORY_PERMS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "NEW_FILE_PERMS")) option = CURLOPT_NEW_FILE_PERMS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "NOBODY")) option = CURLOPT_NOBODY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "NOSIGNAL")) option = CURLOPT_NOSIGNAL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PATH_AS_IS")) option = CURLOPT_PATH_AS_IS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PIPEWAIT")) option = CURLOPT_PIPEWAIT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PORT")) option = CURLOPT_PORT;
//    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "POSTFIELDSIZE")) option = CURLOPT_POSTFIELDSIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "POSTREDIR")) option = CURLOPT_POSTREDIR;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "POST")) option = CURLOPT_POST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROTOCOLS")) option = CURLOPT_PROTOCOLS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXYAUTH")) option = CURLOPT_PROXYAUTH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXYPORT")) option = CURLOPT_PROXYPORT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSL_OPTIONS")) option = CURLOPT_PROXY_SSL_OPTIONS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSL_VERIFYHOST")) option = CURLOPT_PROXY_SSL_VERIFYHOST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSL_VERIFYPEER")) option = CURLOPT_PROXY_SSL_VERIFYPEER;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_SSLVERSION")) option = CURLOPT_PROXY_SSLVERSION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXY_TRANSFER_MODE")) option = CURLOPT_PROXY_TRANSFER_MODE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PROXYTYPE")) option = CURLOPT_PROXYTYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "PUT")) option = CURLOPT_PUT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "REDIR_PROTOCOLS")) option = CURLOPT_REDIR_PROTOCOLS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RESUME_FROM")) option = CURLOPT_RESUME_FROM;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RTSP_CLIENT_CSEQ")) option = CURLOPT_RTSP_CLIENT_CSEQ;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RTSP_REQUEST")) option = CURLOPT_RTSP_REQUEST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "RTSP_SERVER_CSEQ")) option = CURLOPT_RTSP_SERVER_CSEQ;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SASL_IR")) option = CURLOPT_SASL_IR;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SERVER_RESPONSE_TIMEOUT")) option = CURLOPT_SERVER_RESPONSE_TIMEOUT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SOCKS5_AUTH")) option = CURLOPT_SOCKS5_AUTH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SOCKS5_GSSAPI_NEC")) option = CURLOPT_SOCKS5_GSSAPI_NEC;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSH_AUTH_TYPES")) option = CURLOPT_SSH_AUTH_TYPES;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSH_COMPRESSION")) option = CURLOPT_SSH_COMPRESSION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_ENABLE_ALPN")) option = CURLOPT_SSL_ENABLE_ALPN;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_ENABLE_NPN")) option = CURLOPT_SSL_ENABLE_NPN;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_FALSESTART")) option = CURLOPT_SSL_FALSESTART;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_OPTIONS")) option = CURLOPT_SSL_OPTIONS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_SESSIONID_CACHE")) option = CURLOPT_SSL_SESSIONID_CACHE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_VERIFYHOST")) option = CURLOPT_SSL_VERIFYHOST;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_VERIFYPEER")) option = CURLOPT_SSL_VERIFYPEER;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSL_VERIFYSTATUS")) option = CURLOPT_SSL_VERIFYSTATUS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SSLVERSION")) option = CURLOPT_SSLVERSION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "STREAM_WEIGHT")) option = CURLOPT_STREAM_WEIGHT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "SUPPRESS_CONNECT_HEADERS")) option = CURLOPT_SUPPRESS_CONNECT_HEADERS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TCP_FASTOPEN")) option = CURLOPT_TCP_FASTOPEN;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TCP_KEEPALIVE")) option = CURLOPT_TCP_KEEPALIVE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TCP_KEEPIDLE")) option = CURLOPT_TCP_KEEPIDLE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TCP_KEEPINTVL")) option = CURLOPT_TCP_KEEPINTVL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TCP_NODELAY")) option = CURLOPT_TCP_NODELAY;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TFTP_BLKSIZE")) option = CURLOPT_TFTP_BLKSIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TFTP_NO_OPTIONS")) option = CURLOPT_TFTP_NO_OPTIONS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TIMECONDITION")) option = CURLOPT_TIMECONDITION;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TIMEOUT_MS")) option = CURLOPT_TIMEOUT_MS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TIMEOUT")) option = CURLOPT_TIMEOUT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TIMEVALUE")) option = CURLOPT_TIMEVALUE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TRANSFER_ENCODING")) option = CURLOPT_TRANSFER_ENCODING;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "TRANSFERTEXT")) option = CURLOPT_TRANSFERTEXT;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "UNRESTRICTED_AUTH")) option = CURLOPT_UNRESTRICTED_AUTH;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "UPKEEP_INTERVAL_MS")) option = CURLOPT_UPKEEP_INTERVAL_MS;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "UPLOAD_BUFFERSIZE")) option = CURLOPT_UPLOAD_BUFFERSIZE;
//    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "UPLOAD")) option = CURLOPT_UPLOAD;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "USE_SSL")) option = CURLOPT_USE_SSL;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "VERBOSE")) option = CURLOPT_VERBOSE;
    else if (!pg_strcasecmp(name + sizeof("CURLOPT_") - 1, "WILDCARDMATCH")) option = CURLOPT_WILDCARDMATCH;
    else E("unsupported option %s", name);
    if ((res = curl_easy_setopt(curl, option, value)) != CURLE_OK) E("curl_easy_setopt(%s, %li): %s", name, value, curl_easy_strerror(res));
    pfree(name);
    PG_RETURN_BOOL(res == CURLE_OK);
}

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_interrupt_requested; }

EXTENSION(pg_curl_easy_perform) {
    CURLcode res = CURL_LAST;
    if (header_str.data) { free(header_str.data); header_str.data = NULL; }
    if (write_str.data) { free(write_str.data); write_str.data = NULL; }
    if (!(header_str.file = open_memstream(&header_str.data, &header_str.len))) E("!open_memstream");
    if (!(write_str.file = open_memstream(&write_str.data, &write_str.len))) E("!open_memstream");
    if ((res = curl_easy_setopt(curl, CURLOPT_HEADERDATA, header_str.file)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_HEADERDATA): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_INFILESIZE, read_str.len)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_INFILESIZE): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_NOPROGRESS): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_READDATA, read_str.file)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_READDATA): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_str.file)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_WRITEDATA): %s", curl_easy_strerror(res));
    if ((res = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback)) != CURLE_OK) E("curl_easy_setopt(CURLOPT_XFERINFOFUNCTION): %s", curl_easy_strerror(res));
    if (header && ((res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_HTTPHEADER): %s", curl_easy_strerror(res));
    if (recipient && ((res = curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipient)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_MAIL_RCPT): %s", curl_easy_strerror(res));
    if (has_mime && ((res = curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime)) != CURLE_OK)) E("curl_easy_setopt(CURLOPT_MIMEPOST): %s", curl_easy_strerror(res));
    pg_curl_interrupt_requested = 0;
    switch (res = curl_easy_perform(curl)) {
        case CURLE_OK: break;
        case CURLE_ABORTED_BY_CALLBACK: if (pgsql_interrupt_handler && pg_curl_interrupt_requested) { (*pgsql_interrupt_handler)(pg_curl_interrupt_requested); pg_curl_interrupt_requested = 0; } // fall through
        default: E("curl_easy_perform: %s", curl_easy_strerror(res));
    }
    fclose(header_str.file);
    fclose(write_str.file);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_headers) {
    if (!header_str.data) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(header_str.data, header_str.len));
}

EXTENSION(pg_curl_easy_response) {
    if (!write_str.data) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(cstring_to_text_with_len(write_str.data, write_str.len));
}

EXTENSION(pg_curl_easy_getinfo_char) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    char *name, *value = NULL;
    int len;
    if (PG_ARGISNULL(0)) E("info is null!");
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (false);
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "CONTENT_TYPE")) info = CURLINFO_CONTENT_TYPE;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "EFFECTIVE_URL")) info = CURLINFO_EFFECTIVE_URL;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "FTP_ENTRY_PATH")) info = CURLINFO_FTP_ENTRY_PATH;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "LOCAL_IP")) info = CURLINFO_LOCAL_IP;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "PRIMARY_IP")) info = CURLINFO_PRIMARY_IP;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "PRIVATE")) info = CURLINFO_PRIVATE;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "REDIRECT_URL")) info = CURLINFO_REDIRECT_URL;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "RTSP_SESSION_ID")) info = CURLINFO_RTSP_SESSION_ID;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "SCHEME")) info = CURLINFO_SCHEME;
    else E("unsupported option %s", name);
    pfree(name);
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%s): %s", name, curl_easy_strerror(res));
    len = value ? strlen(value) : 0;
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(value, len));
}

EXTENSION(pg_curl_easy_getinfo_long) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    char *name;
    long value;
    if (PG_ARGISNULL(0)) E("info is null!");
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (false);
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "CONDITION_UNMET")) info = CURLINFO_CONDITION_UNMET;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "FILETIME")) info = CURLINFO_FILETIME;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "HEADER_SIZE")) info = CURLINFO_HEADER_SIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "HTTPAUTH_AVAIL")) info = CURLINFO_HTTPAUTH_AVAIL;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "HTTP_CONNECTCODE")) info = CURLINFO_HTTP_CONNECTCODE;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "HTTP_VERSION")) info = CURLINFO_HTTP_VERSION;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "LASTSOCKET")) info = CURLINFO_LASTSOCKET;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "LOCAL_PORT")) info = CURLINFO_LOCAL_PORT;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "NUM_CONNECTS")) info = CURLINFO_NUM_CONNECTS;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "OS_ERRNO")) info = CURLINFO_OS_ERRNO;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "PRIMARY_PORT")) info = CURLINFO_PRIMARY_PORT;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "PROTOCOL")) info = CURLINFO_PROTOCOL;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "PROXYAUTH_AVAIL")) info = CURLINFO_PROXYAUTH_AVAIL;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "PROXY_SSL_VERIFYRESULT")) info = CURLINFO_PROXY_SSL_VERIFYRESULT;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "REDIRECT_COUNT")) info = CURLINFO_REDIRECT_COUNT;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "REQUEST_SIZE")) info = CURLINFO_REQUEST_SIZE;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "RESPONSE_CODE")) info = CURLINFO_RESPONSE_CODE;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "RTSP_CLIENT_CSEQ")) info = CURLINFO_RTSP_CLIENT_CSEQ;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "RTSP_CSEQ_RECV")) info = CURLINFO_RTSP_CSEQ_RECV;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "RTSP_SERVER_CSEQ")) info = CURLINFO_RTSP_SERVER_CSEQ;
    else if (!pg_strcasecmp(name + sizeof("CURLINFO_") - 1, "SSL_VERIFYRESULT")) info = CURLINFO_SSL_VERIFYRESULT;
    else E("unsupported option %s", name);
    pfree(name);
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) E("curl_easy_getinfo(%s): %s", name, curl_easy_strerror(res));
    PG_RETURN_INT64(value);
}

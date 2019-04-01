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
StringInfoData header_buf;
StringInfoData write_buf;
pqsigfunc pgsql_interrupt_handler = NULL;
int pg_curl_interrupt_requested = 0;
struct curl_slist *header = NULL;
struct curl_slist *recipient = NULL;
curl_mime *mime;
bool has_mime;

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
    (void)initStringInfo(&header_buf);
    (void)initStringInfo(&write_buf);
}

void _PG_fini(void) {
    (pqsigfunc)pqsignal(SIGINT, pgsql_interrupt_handler);
    if (curl) (void)curl_easy_cleanup(curl);
    (void)curl_mime_free(mime);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
    (void)curl_global_cleanup();
    (void)pfree(header_buf.data);
    (void)pfree(write_buf.data);
}

Datum pg_curl_easy_init(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_init); Datum pg_curl_easy_init(PG_FUNCTION_ARGS) {
    if (curl) ereport(ERROR, (errmsg("curl already init!")));
    curl = curl_easy_init();
    if (!curl) ereport(ERROR, (errmsg("!curl")));
    mime = curl_mime_init(curl);
    if (!mime) ereport(ERROR, (errmsg("!mime")));
    has_mime = false;
    PG_RETURN_BOOL(curl != NULL);
}

Datum pg_curl_easy_reset(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_reset); Datum pg_curl_easy_reset(PG_FUNCTION_ARGS) {
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    (void)curl_easy_reset(curl);
    (void)curl_slist_free_all(header);
    header = NULL;
    (void)curl_slist_free_all(recipient);
    recipient = NULL;
    (void)curl_mime_free(mime);
    mime = curl_mime_init(curl);
    if (!mime) ereport(ERROR, (errmsg("!mime")));
    has_mime = false;
    (void)resetStringInfo(&header_buf);
    (void)resetStringInfo(&write_buf);
    PG_RETURN_VOID();
}

Datum pg_curl_easy_escape(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_escape); Datum pg_curl_easy_escape(PG_FUNCTION_ARGS) {
    int length;
    char *string, *escape;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("string is null!")));
    string = TextDatumGetCString(PG_GETARG_DATUM(0));
    length = PG_GETARG_INT32(1);
    escape = curl_easy_escape(curl, string, length);
    (void)pfree(string);
    if (!escape) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(escape));
}

Datum pg_curl_easy_unescape(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_unescape); Datum pg_curl_easy_unescape(PG_FUNCTION_ARGS) {
    int length;
    char *url, *unescape;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("url is null!")));
    url = TextDatumGetCString(PG_GETARG_DATUM(0));
    length = PG_GETARG_INT32(1);
    unescape = curl_easy_unescape(curl, url, length, NULL);
    (void)pfree(url);
    if (!unescape) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(unescape));
}

Datum pg_curl_header_append(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_header_append); Datum pg_curl_header_append(PG_FUNCTION_ARGS) {
    char *name, *value;
    StringInfoData buf;
    struct curl_slist *temp = header;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("name is null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("value is null!")));
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
    char *email;
    struct curl_slist *temp = recipient;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("email is null!")));
    email = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, email))) recipient = temp;
    (void)pfree(email);
    PG_RETURN_BOOL(temp != NULL);
}

Datum pg_curl_mime_data(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_data); Datum pg_curl_mime_data(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *data, *encoding = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("data is null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) encoding = TextDatumGetCString(PG_GETARG_DATUM(1));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, data, CURL_ZERO_TERMINATED)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", data, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(data);
    if (encoding) (void)pfree(encoding);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_mime_filedata(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_filedata); Datum pg_curl_mime_filedata(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *filename, *base = NULL, *type = NULL, *encoding = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("filename is null!")));
    filename = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) base = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) type = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) encoding = TextDatumGetCString(PG_GETARG_DATUM(3));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_filedata(part, filename)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_filedata(%s): %s", filename, curl_easy_strerror(res))));
    if (base && ((res = curl_mime_filename(part, base)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_filename(%s): %s", base, curl_easy_strerror(res))));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(filename);
    if (base) (void)pfree(base);
    if (type) (void)pfree(type);
    if (encoding) (void)pfree(encoding);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_mime_data_name(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_data_name); Datum pg_curl_mime_data_name(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *data, *name, *encoding = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument data must not null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument name must not null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) encoding = TextDatumGetCString(PG_GETARG_DATUM(2));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, data, CURL_ZERO_TERMINATED)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", data, curl_easy_strerror(res))));
    if ((res = curl_mime_name(part, name)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_name(%s): %s", name, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(data);
    (void)pfree(name);
    if (encoding) (void)pfree(encoding);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

Datum pg_curl_mime_data_type(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_mime_data_type); Datum pg_curl_mime_data_type(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    char *data, *type, *encoding = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("first argument data must not null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("second argument type must not null!")));
    type = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) encoding = TextDatumGetCString(PG_GETARG_DATUM(2));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, data, CURL_ZERO_TERMINATED)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", data, curl_easy_strerror(res))));
    if ((res = curl_mime_type(part, type)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
    if (encoding && ((res = curl_mime_encoder(part, encoding)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", encoding, curl_easy_strerror(res))));
    (void)pfree(data);
    (void)pfree(type);
    if (encoding) (void)pfree(encoding);
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
    if (false);
    else if (!pg_strncasecmp(option_char, "CURLOPT_ABSTRACT_UNIX_SOCKET", sizeof("CURLOPT_ABSTRACT_UNIX_SOCKET") - 1)) option = CURLOPT_ABSTRACT_UNIX_SOCKET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_ACCEPT_ENCODING", sizeof("CURLOPT_ACCEPT_ENCODING") - 1)) option = CURLOPT_ACCEPT_ENCODING;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_ALTSVC", sizeof("CURLOPT_ALTSVC") - 1)) option = CURLOPT_ALTSVC;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CAINFO", sizeof("CURLOPT_CAINFO") - 1)) option = CURLOPT_CAINFO;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CAPATH", sizeof("CURLOPT_CAPATH") - 1)) option = CURLOPT_CAPATH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_COOKIEFILE", sizeof("CURLOPT_COOKIEFILE") - 1)) option = CURLOPT_COOKIEFILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_COOKIEJAR", sizeof("CURLOPT_COOKIEJAR") - 1)) option = CURLOPT_COOKIEJAR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_COOKIELIST", sizeof("CURLOPT_COOKIELIST") - 1)) option = CURLOPT_COOKIELIST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_COOKIE", sizeof("CURLOPT_COOKIE") - 1)) option = CURLOPT_COOKIE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_COPYPOSTFIELDS", sizeof("CURLOPT_COPYPOSTFIELDS") - 1)) option = CURLOPT_COPYPOSTFIELDS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CRLFILE", sizeof("CURLOPT_CRLFILE") - 1)) option = CURLOPT_CRLFILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CUSTOMREQUEST", sizeof("CURLOPT_CUSTOMREQUEST") - 1)) option = CURLOPT_CUSTOMREQUEST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DEFAULT_PROTOCOL", sizeof("CURLOPT_DEFAULT_PROTOCOL") - 1)) option = CURLOPT_DEFAULT_PROTOCOL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_INTERFACE", sizeof("CURLOPT_DNS_INTERFACE") - 1)) option = CURLOPT_DNS_INTERFACE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_LOCAL_IP4", sizeof("CURLOPT_DNS_LOCAL_IP4") - 1)) option = CURLOPT_DNS_LOCAL_IP4;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_LOCAL_IP6", sizeof("CURLOPT_DNS_LOCAL_IP6") - 1)) option = CURLOPT_DNS_LOCAL_IP6;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_SERVERS", sizeof("CURLOPT_DNS_SERVERS") - 1)) option = CURLOPT_DNS_SERVERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DOH_URL", sizeof("CURLOPT_DOH_URL") - 1)) option = CURLOPT_DOH_URL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_EGDSOCKET", sizeof("CURLOPT_EGDSOCKET") - 1)) option = CURLOPT_EGDSOCKET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_ACCOUNT", sizeof("CURLOPT_FTP_ACCOUNT") - 1)) option = CURLOPT_FTP_ACCOUNT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_ALTERNATIVE_TO_USER", sizeof("CURLOPT_FTP_ALTERNATIVE_TO_USER") - 1)) option = CURLOPT_FTP_ALTERNATIVE_TO_USER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTPPORT", sizeof("CURLOPT_FTPPORT") - 1)) option = CURLOPT_FTPPORT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_INTERFACE", sizeof("CURLOPT_INTERFACE") - 1)) option = CURLOPT_INTERFACE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_ISSUERCERT", sizeof("CURLOPT_ISSUERCERT") - 1)) option = CURLOPT_ISSUERCERT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_KEYPASSWD", sizeof("CURLOPT_KEYPASSWD") - 1)) option = CURLOPT_KEYPASSWD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_KRBLEVEL", sizeof("CURLOPT_KRBLEVEL") - 1)) option = CURLOPT_KRBLEVEL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_LOGIN_OPTIONS", sizeof("CURLOPT_LOGIN_OPTIONS") - 1)) option = CURLOPT_LOGIN_OPTIONS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAIL_AUTH", sizeof("CURLOPT_MAIL_AUTH") - 1)) option = CURLOPT_MAIL_AUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAIL_FROM", sizeof("CURLOPT_MAIL_FROM") - 1)) option = CURLOPT_MAIL_FROM;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NOPROXY", sizeof("CURLOPT_NOPROXY") - 1)) option = CURLOPT_NOPROXY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PASSWORD", sizeof("CURLOPT_PASSWORD") - 1)) option = CURLOPT_PASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PINNEDPUBLICKEY", sizeof("CURLOPT_PINNEDPUBLICKEY") - 1)) option = CURLOPT_PINNEDPUBLICKEY;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_POSTFIELDS", sizeof("CURLOPT_POSTFIELDS") - 1)) option = CURLOPT_POSTFIELDS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PRE_PROXY", sizeof("CURLOPT_PRE_PROXY") - 1)) option = CURLOPT_PRE_PROXY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_CAINFO", sizeof("CURLOPT_PROXY_CAINFO") - 1)) option = CURLOPT_PROXY_CAINFO;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_CAPATH", sizeof("CURLOPT_PROXY_CAPATH") - 1)) option = CURLOPT_PROXY_CAPATH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_CRLFILE", sizeof("CURLOPT_PROXY_CRLFILE") - 1)) option = CURLOPT_PROXY_CRLFILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_KEYPASSWD", sizeof("CURLOPT_PROXY_KEYPASSWD") - 1)) option = CURLOPT_PROXY_KEYPASSWD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYPASSWORD", sizeof("CURLOPT_PROXYPASSWORD") - 1)) option = CURLOPT_PROXYPASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_PINNEDPUBLICKEY", sizeof("CURLOPT_PROXY_PINNEDPUBLICKEY") - 1)) option = CURLOPT_PROXY_PINNEDPUBLICKEY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SERVICE_NAME", sizeof("CURLOPT_PROXY_SERVICE_NAME") - 1)) option = CURLOPT_PROXY_SERVICE_NAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY", sizeof("CURLOPT_PROXY") - 1)) option = CURLOPT_PROXY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSLCERT", sizeof("CURLOPT_PROXY_SSLCERT") - 1)) option = CURLOPT_PROXY_SSLCERT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSLCERTTYPE", sizeof("CURLOPT_PROXY_SSLCERTTYPE") - 1)) option = CURLOPT_PROXY_SSLCERTTYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSL_CIPHER_LIST", sizeof("CURLOPT_PROXY_SSL_CIPHER_LIST") - 1)) option = CURLOPT_PROXY_SSL_CIPHER_LIST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSLKEY", sizeof("CURLOPT_PROXY_SSLKEY") - 1)) option = CURLOPT_PROXY_SSLKEY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSLKEYTYPE", sizeof("CURLOPT_PROXY_SSLKEYTYPE") - 1)) option = CURLOPT_PROXY_SSLKEYTYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLS13_CIPHERS", sizeof("CURLOPT_PROXY_TLS13_CIPHERS") - 1)) option = CURLOPT_PROXY_TLS13_CIPHERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLSAUTH_PASSWORD", sizeof("CURLOPT_PROXY_TLSAUTH_PASSWORD") - 1)) option = CURLOPT_PROXY_TLSAUTH_PASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLSAUTH_TYPE", sizeof("CURLOPT_PROXY_TLSAUTH_TYPE") - 1)) option = CURLOPT_PROXY_TLSAUTH_TYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TLSAUTH_USERNAME", sizeof("CURLOPT_PROXY_TLSAUTH_USERNAME") - 1)) option = CURLOPT_PROXY_TLSAUTH_USERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYUSERNAME", sizeof("CURLOPT_PROXYUSERNAME") - 1)) option = CURLOPT_PROXYUSERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYUSERPWD", sizeof("CURLOPT_PROXYUSERPWD") - 1)) option = CURLOPT_PROXYUSERPWD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RANDOM_FILE", sizeof("CURLOPT_RANDOM_FILE") - 1)) option = CURLOPT_RANDOM_FILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RANGE", sizeof("CURLOPT_RANGE") - 1)) option = CURLOPT_RANGE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_REFERER", sizeof("CURLOPT_REFERER") - 1)) option = CURLOPT_REFERER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_REQUEST_TARGET", sizeof("CURLOPT_REQUEST_TARGET") - 1)) option = CURLOPT_REQUEST_TARGET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RTSP_SESSION_ID", sizeof("CURLOPT_RTSP_SESSION_ID") - 1)) option = CURLOPT_RTSP_SESSION_ID;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RTSP_STREAM_URI", sizeof("CURLOPT_RTSP_STREAM_URI") - 1)) option = CURLOPT_RTSP_STREAM_URI;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RTSP_TRANSPORT", sizeof("CURLOPT_RTSP_TRANSPORT") - 1)) option = CURLOPT_RTSP_TRANSPORT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SERVICE_NAME", sizeof("CURLOPT_SERVICE_NAME") - 1)) option = CURLOPT_SERVICE_NAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SOCKS5_GSSAPI_SERVICE", sizeof("CURLOPT_SOCKS5_GSSAPI_SERVICE") - 1)) option = CURLOPT_SOCKS5_GSSAPI_SERVICE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSH_HOST_PUBLIC_KEY_MD5", sizeof("CURLOPT_SSH_HOST_PUBLIC_KEY_MD5") - 1)) option = CURLOPT_SSH_HOST_PUBLIC_KEY_MD5;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSH_KNOWNHOSTS", sizeof("CURLOPT_SSH_KNOWNHOSTS") - 1)) option = CURLOPT_SSH_KNOWNHOSTS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSH_PRIVATE_KEYFILE", sizeof("CURLOPT_SSH_PRIVATE_KEYFILE") - 1)) option = CURLOPT_SSH_PRIVATE_KEYFILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSH_PUBLIC_KEYFILE", sizeof("CURLOPT_SSH_PUBLIC_KEYFILE") - 1)) option = CURLOPT_SSH_PUBLIC_KEYFILE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLCERT", sizeof("CURLOPT_SSLCERT") - 1)) option = CURLOPT_SSLCERT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLCERTTYPE", sizeof("CURLOPT_SSLCERTTYPE") - 1)) option = CURLOPT_SSLCERTTYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_CIPHER_LIST", sizeof("CURLOPT_SSL_CIPHER_LIST") - 1)) option = CURLOPT_SSL_CIPHER_LIST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLENGINE", sizeof("CURLOPT_SSLENGINE") - 1)) option = CURLOPT_SSLENGINE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLKEY", sizeof("CURLOPT_SSLKEY") - 1)) option = CURLOPT_SSLKEY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLKEYTYPE", sizeof("CURLOPT_SSLKEYTYPE") - 1)) option = CURLOPT_SSLKEYTYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLS13_CIPHERS", sizeof("CURLOPT_TLS13_CIPHERS") - 1)) option = CURLOPT_TLS13_CIPHERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLSAUTH_PASSWORD", sizeof("CURLOPT_TLSAUTH_PASSWORD") - 1)) option = CURLOPT_TLSAUTH_PASSWORD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLSAUTH_TYPE", sizeof("CURLOPT_TLSAUTH_TYPE") - 1)) option = CURLOPT_TLSAUTH_TYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TLSAUTH_USERNAME", sizeof("CURLOPT_TLSAUTH_USERNAME") - 1)) option = CURLOPT_TLSAUTH_USERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_UNIX_SOCKET_PATH", sizeof("CURLOPT_UNIX_SOCKET_PATH") - 1)) option = CURLOPT_UNIX_SOCKET_PATH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_URL", sizeof("CURLOPT_URL") - 1)) option = CURLOPT_URL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_USERAGENT", sizeof("CURLOPT_USERAGENT") - 1)) option = CURLOPT_USERAGENT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_USERNAME", sizeof("CURLOPT_USERNAME") - 1)) option = CURLOPT_USERNAME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_USERPWD", sizeof("CURLOPT_USERPWD") - 1)) option = CURLOPT_USERPWD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_XOAUTH2_BEARER", sizeof("CURLOPT_XOAUTH2_BEARER") - 1)) option = CURLOPT_XOAUTH2_BEARER;
    else ereport(ERROR, (errmsg("unsupported option %s", option_char)));
    if ((res = curl_easy_setopt(curl, option, parameter_char)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %s): %s", option_char, parameter_char, curl_easy_strerror(res))));
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
    else if (!pg_strncasecmp(option_char, "CURLOPT_ACCEPTTIMEOUT_MS", sizeof("CURLOPT_ACCEPTTIMEOUT_MS") - 1)) option = CURLOPT_ACCEPTTIMEOUT_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_ADDRESS_SCOPE", sizeof("CURLOPT_ADDRESS_SCOPE") - 1)) option = CURLOPT_ADDRESS_SCOPE;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_ALTSVC_CTRL", sizeof("CURLOPT_ALTSVC_CTRL") - 1)) option = CURLOPT_ALTSVC_CTRL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_APPEND", sizeof("CURLOPT_APPEND") - 1)) option = CURLOPT_APPEND;
    else if (!pg_strncasecmp(option_char, "CURLOPT_AUTOREFERER", sizeof("CURLOPT_AUTOREFERER") - 1)) option = CURLOPT_AUTOREFERER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_BUFFERSIZE", sizeof("CURLOPT_BUFFERSIZE") - 1)) option = CURLOPT_BUFFERSIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CERTINFO", sizeof("CURLOPT_CERTINFO") - 1)) option = CURLOPT_CERTINFO;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CONNECT_ONLY", sizeof("CURLOPT_CONNECT_ONLY") - 1)) option = CURLOPT_CONNECT_ONLY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CONNECTTIMEOUT_MS", sizeof("CURLOPT_CONNECTTIMEOUT_MS") - 1)) option = CURLOPT_CONNECTTIMEOUT_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CONNECTTIMEOUT", sizeof("CURLOPT_CONNECTTIMEOUT") - 1)) option = CURLOPT_CONNECTTIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_COOKIESESSION", sizeof("CURLOPT_COOKIESESSION") - 1)) option = CURLOPT_COOKIESESSION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_CRLF", sizeof("CURLOPT_CRLF") - 1)) option = CURLOPT_CRLF;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DIRLISTONLY", sizeof("CURLOPT_DIRLISTONLY") - 1)) option = CURLOPT_DIRLISTONLY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DISALLOW_USERNAME_IN_URL", sizeof("CURLOPT_DISALLOW_USERNAME_IN_URL") - 1)) option = CURLOPT_DISALLOW_USERNAME_IN_URL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_CACHE_TIMEOUT", sizeof("CURLOPT_DNS_CACHE_TIMEOUT") - 1)) option = CURLOPT_DNS_CACHE_TIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_SHUFFLE_ADDRESSES", sizeof("CURLOPT_DNS_SHUFFLE_ADDRESSES") - 1)) option = CURLOPT_DNS_SHUFFLE_ADDRESSES;
    else if (!pg_strncasecmp(option_char, "CURLOPT_DNS_USE_GLOBAL_CACHE", sizeof("CURLOPT_DNS_USE_GLOBAL_CACHE") - 1)) option = CURLOPT_DNS_USE_GLOBAL_CACHE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_EXPECT_100_TIMEOUT_MS", sizeof("CURLOPT_EXPECT_100_TIMEOUT_MS") - 1)) option = CURLOPT_EXPECT_100_TIMEOUT_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FAILONERROR", sizeof("CURLOPT_FAILONERROR") - 1)) option = CURLOPT_FAILONERROR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FILETIME", sizeof("CURLOPT_FILETIME") - 1)) option = CURLOPT_FILETIME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FOLLOWLOCATION", sizeof("CURLOPT_FOLLOWLOCATION") - 1)) option = CURLOPT_FOLLOWLOCATION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FORBID_REUSE", sizeof("CURLOPT_FORBID_REUSE") - 1)) option = CURLOPT_FORBID_REUSE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FRESH_CONNECT", sizeof("CURLOPT_FRESH_CONNECT") - 1)) option = CURLOPT_FRESH_CONNECT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_CREATE_MISSING_DIRS", sizeof("CURLOPT_FTP_CREATE_MISSING_DIRS") - 1)) option = CURLOPT_FTP_CREATE_MISSING_DIRS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_FILEMETHOD", sizeof("CURLOPT_FTP_FILEMETHOD") - 1)) option = CURLOPT_FTP_FILEMETHOD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_SKIP_PASV_IP", sizeof("CURLOPT_FTP_SKIP_PASV_IP") - 1)) option = CURLOPT_FTP_SKIP_PASV_IP;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTPSSLAUTH", sizeof("CURLOPT_FTPSSLAUTH") - 1)) option = CURLOPT_FTPSSLAUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_SSL_CCC", sizeof("CURLOPT_FTP_SSL_CCC") - 1)) option = CURLOPT_FTP_SSL_CCC;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_USE_EPRT", sizeof("CURLOPT_FTP_USE_EPRT") - 1)) option = CURLOPT_FTP_USE_EPRT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_USE_EPSV", sizeof("CURLOPT_FTP_USE_EPSV") - 1)) option = CURLOPT_FTP_USE_EPSV;
    else if (!pg_strncasecmp(option_char, "CURLOPT_FTP_USE_PRET", sizeof("CURLOPT_FTP_USE_PRET") - 1)) option = CURLOPT_FTP_USE_PRET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_GSSAPI_DELEGATION", sizeof("CURLOPT_GSSAPI_DELEGATION") - 1)) option = CURLOPT_GSSAPI_DELEGATION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS", sizeof("CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS") - 1)) option = CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HAPROXYPROTOCOL", sizeof("CURLOPT_HAPROXYPROTOCOL") - 1)) option = CURLOPT_HAPROXYPROTOCOL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HEADER", sizeof("CURLOPT_HEADER") - 1)) option = CURLOPT_HEADER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTP09_ALLOWED", sizeof("CURLOPT_HTTP09_ALLOWED") - 1)) option = CURLOPT_HTTP09_ALLOWED;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTPAUTH", sizeof("CURLOPT_HTTPAUTH") - 1)) option = CURLOPT_HTTPAUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTP_CONTENT_DECODING", sizeof("CURLOPT_HTTP_CONTENT_DECODING") - 1)) option = CURLOPT_HTTP_CONTENT_DECODING;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTPGET", sizeof("CURLOPT_HTTPGET") - 1)) option = CURLOPT_HTTPGET;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTPPROXYTUNNEL", sizeof("CURLOPT_HTTPPROXYTUNNEL") - 1)) option = CURLOPT_HTTPPROXYTUNNEL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTP_TRANSFER_DECODING", sizeof("CURLOPT_HTTP_TRANSFER_DECODING") - 1)) option = CURLOPT_HTTP_TRANSFER_DECODING;
    else if (!pg_strncasecmp(option_char, "CURLOPT_HTTP_VERSION", sizeof("CURLOPT_HTTP_VERSION") - 1)) option = CURLOPT_HTTP_VERSION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_IGNORE_CONTENT_LENGTH", sizeof("CURLOPT_IGNORE_CONTENT_LENGTH") - 1)) option = CURLOPT_IGNORE_CONTENT_LENGTH;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_INFILESIZE", sizeof("CURLOPT_INFILESIZE") - 1)) option = CURLOPT_INFILESIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_IPRESOLVE", sizeof("CURLOPT_IPRESOLVE") - 1)) option = CURLOPT_IPRESOLVE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_KEEP_SENDING_ON_ERROR", sizeof("CURLOPT_KEEP_SENDING_ON_ERROR") - 1)) option = CURLOPT_KEEP_SENDING_ON_ERROR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_LOCALPORTRANGE", sizeof("CURLOPT_LOCALPORTRANGE") - 1)) option = CURLOPT_LOCALPORTRANGE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_LOCALPORT", sizeof("CURLOPT_LOCALPORT") - 1)) option = CURLOPT_LOCALPORT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_LOW_SPEED_LIMIT", sizeof("CURLOPT_LOW_SPEED_LIMIT") - 1)) option = CURLOPT_LOW_SPEED_LIMIT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_LOW_SPEED_TIME", sizeof("CURLOPT_LOW_SPEED_TIME") - 1)) option = CURLOPT_LOW_SPEED_TIME;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAXCONNECTS", sizeof("CURLOPT_MAXCONNECTS") - 1)) option = CURLOPT_MAXCONNECTS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAXFILESIZE", sizeof("CURLOPT_MAXFILESIZE") - 1)) option = CURLOPT_MAXFILESIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_MAXREDIRS", sizeof("CURLOPT_MAXREDIRS") - 1)) option = CURLOPT_MAXREDIRS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NETRC", sizeof("CURLOPT_NETRC") - 1)) option = CURLOPT_NETRC;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NEW_DIRECTORY_PERMS", sizeof("CURLOPT_NEW_DIRECTORY_PERMS") - 1)) option = CURLOPT_NEW_DIRECTORY_PERMS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NEW_FILE_PERMS", sizeof("CURLOPT_NEW_FILE_PERMS") - 1)) option = CURLOPT_NEW_FILE_PERMS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NOBODY", sizeof("CURLOPT_NOBODY") - 1)) option = CURLOPT_NOBODY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_NOSIGNAL", sizeof("CURLOPT_NOSIGNAL") - 1)) option = CURLOPT_NOSIGNAL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PATH_AS_IS", sizeof("CURLOPT_PATH_AS_IS") - 1)) option = CURLOPT_PATH_AS_IS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PIPEWAIT", sizeof("CURLOPT_PIPEWAIT") - 1)) option = CURLOPT_PIPEWAIT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PORT", sizeof("CURLOPT_PORT") - 1)) option = CURLOPT_PORT;
//    else if (!pg_strncasecmp(option_char, "CURLOPT_POSTFIELDSIZE", sizeof("CURLOPT_POSTFIELDSIZE") - 1)) option = CURLOPT_POSTFIELDSIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_POSTREDIR", sizeof("CURLOPT_POSTREDIR") - 1)) option = CURLOPT_POSTREDIR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_POST", sizeof("CURLOPT_POST") - 1)) option = CURLOPT_POST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROTOCOLS", sizeof("CURLOPT_PROTOCOLS") - 1)) option = CURLOPT_PROTOCOLS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYAUTH", sizeof("CURLOPT_PROXYAUTH") - 1)) option = CURLOPT_PROXYAUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYPORT", sizeof("CURLOPT_PROXYPORT") - 1)) option = CURLOPT_PROXYPORT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSL_OPTIONS", sizeof("CURLOPT_PROXY_SSL_OPTIONS") - 1)) option = CURLOPT_PROXY_SSL_OPTIONS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSL_VERIFYHOST", sizeof("CURLOPT_PROXY_SSL_VERIFYHOST") - 1)) option = CURLOPT_PROXY_SSL_VERIFYHOST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSL_VERIFYPEER", sizeof("CURLOPT_PROXY_SSL_VERIFYPEER") - 1)) option = CURLOPT_PROXY_SSL_VERIFYPEER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_SSLVERSION", sizeof("CURLOPT_PROXY_SSLVERSION") - 1)) option = CURLOPT_PROXY_SSLVERSION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXY_TRANSFER_MODE", sizeof("CURLOPT_PROXY_TRANSFER_MODE") - 1)) option = CURLOPT_PROXY_TRANSFER_MODE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PROXYTYPE", sizeof("CURLOPT_PROXYTYPE") - 1)) option = CURLOPT_PROXYTYPE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_PUT", sizeof("CURLOPT_PUT") - 1)) option = CURLOPT_PUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_REDIR_PROTOCOLS", sizeof("CURLOPT_REDIR_PROTOCOLS") - 1)) option = CURLOPT_REDIR_PROTOCOLS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RESUME_FROM", sizeof("CURLOPT_RESUME_FROM") - 1)) option = CURLOPT_RESUME_FROM;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RTSP_CLIENT_CSEQ", sizeof("CURLOPT_RTSP_CLIENT_CSEQ") - 1)) option = CURLOPT_RTSP_CLIENT_CSEQ;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RTSP_REQUEST", sizeof("CURLOPT_RTSP_REQUEST") - 1)) option = CURLOPT_RTSP_REQUEST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_RTSP_SERVER_CSEQ", sizeof("CURLOPT_RTSP_SERVER_CSEQ") - 1)) option = CURLOPT_RTSP_SERVER_CSEQ;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SASL_IR", sizeof("CURLOPT_SASL_IR") - 1)) option = CURLOPT_SASL_IR;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SERVER_RESPONSE_TIMEOUT", sizeof("CURLOPT_SERVER_RESPONSE_TIMEOUT") - 1)) option = CURLOPT_SERVER_RESPONSE_TIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SOCKS5_AUTH", sizeof("CURLOPT_SOCKS5_AUTH") - 1)) option = CURLOPT_SOCKS5_AUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SOCKS5_GSSAPI_NEC", sizeof("CURLOPT_SOCKS5_GSSAPI_NEC") - 1)) option = CURLOPT_SOCKS5_GSSAPI_NEC;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSH_AUTH_TYPES", sizeof("CURLOPT_SSH_AUTH_TYPES") - 1)) option = CURLOPT_SSH_AUTH_TYPES;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSH_COMPRESSION", sizeof("CURLOPT_SSH_COMPRESSION") - 1)) option = CURLOPT_SSH_COMPRESSION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_ENABLE_ALPN", sizeof("CURLOPT_SSL_ENABLE_ALPN") - 1)) option = CURLOPT_SSL_ENABLE_ALPN;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_ENABLE_NPN", sizeof("CURLOPT_SSL_ENABLE_NPN") - 1)) option = CURLOPT_SSL_ENABLE_NPN;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_FALSESTART", sizeof("CURLOPT_SSL_FALSESTART") - 1)) option = CURLOPT_SSL_FALSESTART;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_OPTIONS", sizeof("CURLOPT_SSL_OPTIONS") - 1)) option = CURLOPT_SSL_OPTIONS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_SESSIONID_CACHE", sizeof("CURLOPT_SSL_SESSIONID_CACHE") - 1)) option = CURLOPT_SSL_SESSIONID_CACHE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_VERIFYHOST", sizeof("CURLOPT_SSL_VERIFYHOST") - 1)) option = CURLOPT_SSL_VERIFYHOST;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_VERIFYPEER", sizeof("CURLOPT_SSL_VERIFYPEER") - 1)) option = CURLOPT_SSL_VERIFYPEER;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSL_VERIFYSTATUS", sizeof("CURLOPT_SSL_VERIFYSTATUS") - 1)) option = CURLOPT_SSL_VERIFYSTATUS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SSLVERSION", sizeof("CURLOPT_SSLVERSION") - 1)) option = CURLOPT_SSLVERSION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_STREAM_WEIGHT", sizeof("CURLOPT_STREAM_WEIGHT") - 1)) option = CURLOPT_STREAM_WEIGHT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_SUPPRESS_CONNECT_HEADERS", sizeof("CURLOPT_SUPPRESS_CONNECT_HEADERS") - 1)) option = CURLOPT_SUPPRESS_CONNECT_HEADERS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_FASTOPEN", sizeof("CURLOPT_TCP_FASTOPEN") - 1)) option = CURLOPT_TCP_FASTOPEN;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_KEEPALIVE", sizeof("CURLOPT_TCP_KEEPALIVE") - 1)) option = CURLOPT_TCP_KEEPALIVE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_KEEPIDLE", sizeof("CURLOPT_TCP_KEEPIDLE") - 1)) option = CURLOPT_TCP_KEEPIDLE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_KEEPINTVL", sizeof("CURLOPT_TCP_KEEPINTVL") - 1)) option = CURLOPT_TCP_KEEPINTVL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TCP_NODELAY", sizeof("CURLOPT_TCP_NODELAY") - 1)) option = CURLOPT_TCP_NODELAY;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TFTP_BLKSIZE", sizeof("CURLOPT_TFTP_BLKSIZE") - 1)) option = CURLOPT_TFTP_BLKSIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TFTP_NO_OPTIONS", sizeof("CURLOPT_TFTP_NO_OPTIONS") - 1)) option = CURLOPT_TFTP_NO_OPTIONS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMECONDITION", sizeof("CURLOPT_TIMECONDITION") - 1)) option = CURLOPT_TIMECONDITION;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMEOUT_MS", sizeof("CURLOPT_TIMEOUT_MS") - 1)) option = CURLOPT_TIMEOUT_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMEOUT", sizeof("CURLOPT_TIMEOUT") - 1)) option = CURLOPT_TIMEOUT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TIMEVALUE", sizeof("CURLOPT_TIMEVALUE") - 1)) option = CURLOPT_TIMEVALUE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TRANSFER_ENCODING", sizeof("CURLOPT_TRANSFER_ENCODING") - 1)) option = CURLOPT_TRANSFER_ENCODING;
    else if (!pg_strncasecmp(option_char, "CURLOPT_TRANSFERTEXT", sizeof("CURLOPT_TRANSFERTEXT") - 1)) option = CURLOPT_TRANSFERTEXT;
    else if (!pg_strncasecmp(option_char, "CURLOPT_UNRESTRICTED_AUTH", sizeof("CURLOPT_UNRESTRICTED_AUTH") - 1)) option = CURLOPT_UNRESTRICTED_AUTH;
    else if (!pg_strncasecmp(option_char, "CURLOPT_UPKEEP_INTERVAL_MS", sizeof("CURLOPT_UPKEEP_INTERVAL_MS") - 1)) option = CURLOPT_UPKEEP_INTERVAL_MS;
    else if (!pg_strncasecmp(option_char, "CURLOPT_UPLOAD_BUFFERSIZE", sizeof("CURLOPT_UPLOAD_BUFFERSIZE") - 1)) option = CURLOPT_UPLOAD_BUFFERSIZE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_UPLOAD", sizeof("CURLOPT_UPLOAD") - 1)) option = CURLOPT_UPLOAD;
    else if (!pg_strncasecmp(option_char, "CURLOPT_USE_SSL", sizeof("CURLOPT_USE_SSL") - 1)) option = CURLOPT_USE_SSL;
    else if (!pg_strncasecmp(option_char, "CURLOPT_VERBOSE", sizeof("CURLOPT_VERBOSE") - 1)) option = CURLOPT_VERBOSE;
    else if (!pg_strncasecmp(option_char, "CURLOPT_WILDCARDMATCH", sizeof("CURLOPT_WILDCARDMATCH") - 1)) option = CURLOPT_WILDCARDMATCH;
    else ereport(ERROR, (errmsg("unsupported option %s", option_char)));
    if ((res = curl_easy_setopt(curl, option, parameter_long)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %li): %s", option_char, parameter_long, curl_easy_strerror(res))));
    (void)pfree(option_char);
    PG_RETURN_BOOL(res == CURLE_OK);
}

inline static size_t header_callback(void *buffer, size_t size, size_t nitems, void *outstream) {
    size_t realsize = size * nitems;
//    elog(LOG, "buffer=%s, size=%lu, nitems=%lu, outstream=%s", (const char *)buffer, size, nitems, ((StringInfo)outstream)->data);
    (void)appendBinaryStringInfo((StringInfo)outstream, (const char *)buffer, (int)realsize);
    return realsize;
}

inline static size_t write_callback(void *buffer, size_t size, size_t nitems, void *outstream) {
    size_t realsize = size * nitems;
//    elog(LOG, "buffer=%s, size=%lu, nitems=%lu, outstream=%s", (const char *)buffer, size, nitems, ((StringInfo)outstream)->data);
    (void)appendBinaryStringInfo((StringInfo)outstream, (const char *)buffer, (int)realsize);
    return realsize;
}

inline static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_interrupt_requested; }

Datum pg_curl_easy_perform(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(pg_curl_easy_perform); Datum pg_curl_easy_perform(PG_FUNCTION_ARGS) {
    CURLcode res = CURL_LAST;
    if (!curl) ereport(ERROR, (errmsg("call pg_curl_easy_init before!")));
    (void)resetStringInfo(&header_buf);
    (void)resetStringInfo(&write_buf);
    if ((res = curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)(&header_buf))) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_HEADERDATA): %s", curl_easy_strerror(res))));
    if ((res = curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_HEADERFUNCTION): %s", curl_easy_strerror(res))));
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
    else if (!pg_strncasecmp(info_char, "CURLINFO_EFFECTIVE_URL", sizeof("CURLINFO_EFFECTIVE_URL") - 1)) info = CURLINFO_EFFECTIVE_URL;
    else if (!pg_strncasecmp(info_char, "CURLINFO_FTP_ENTRY_PATH", sizeof("CURLINFO_FTP_ENTRY_PATH") - 1)) info = CURLINFO_FTP_ENTRY_PATH;
    else if (!pg_strncasecmp(info_char, "CURLINFO_HEADERS", sizeof("CURLINFO_HEADERS") - 1)) { str = header_buf.data; goto ret; }
    else if (!pg_strncasecmp(info_char, "CURLINFO_LOCAL_IP", sizeof("CURLINFO_LOCAL_IP") - 1)) info = CURLINFO_LOCAL_IP;
    else if (!pg_strncasecmp(info_char, "CURLINFO_PRIMARY_IP", sizeof("CURLINFO_PRIMARY_IP") - 1)) info = CURLINFO_PRIMARY_IP;
    else if (!pg_strncasecmp(info_char, "CURLINFO_PRIVATE", sizeof("CURLINFO_PRIVATE") - 1)) info = CURLINFO_PRIVATE;
    else if (!pg_strncasecmp(info_char, "CURLINFO_REDIRECT_URL", sizeof("CURLINFO_REDIRECT_URL") - 1)) info = CURLINFO_REDIRECT_URL;
    else if (!pg_strncasecmp(info_char, "CURLINFO_RESPONSE", sizeof("CURLINFO_RESPONSE") - 1)) { str = write_buf.data; goto ret; }
    else if (!pg_strncasecmp(info_char, "CURLINFO_RTSP_SESSION_ID", sizeof("CURLINFO_RTSP_SESSION_ID") - 1)) info = CURLINFO_RTSP_SESSION_ID;
    else if (!pg_strncasecmp(info_char, "CURLINFO_SCHEME", sizeof("CURLINFO_SCHEME") - 1)) info = CURLINFO_SCHEME;
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
    else if (!pg_strncasecmp(info_char, "CURLINFO_CONDITION_UNMET", sizeof("CURLINFO_CONDITION_UNMET") - 1)) info = CURLINFO_CONDITION_UNMET;
    else if (!pg_strncasecmp(info_char, "CURLINFO_FILETIME", sizeof("CURLINFO_FILETIME") - 1)) info = CURLINFO_FILETIME;
    else if (!pg_strncasecmp(info_char, "CURLINFO_HEADER_SIZE", sizeof("CURLINFO_HEADER_SIZE") - 1)) info = CURLINFO_HEADER_SIZE;
    else if (!pg_strncasecmp(info_char, "CURLINFO_HTTPAUTH_AVAIL", sizeof("CURLINFO_HTTPAUTH_AVAIL") - 1)) info = CURLINFO_HTTPAUTH_AVAIL;
    else if (!pg_strncasecmp(info_char, "CURLINFO_HTTP_CONNECTCODE", sizeof("CURLINFO_HTTP_CONNECTCODE") - 1)) info = CURLINFO_HTTP_CONNECTCODE;
    else if (!pg_strncasecmp(info_char, "CURLINFO_HTTP_VERSION", sizeof("CURLINFO_HTTP_VERSION") - 1)) info = CURLINFO_HTTP_VERSION;
    else if (!pg_strncasecmp(info_char, "CURLINFO_LASTSOCKET", sizeof("CURLINFO_LASTSOCKET") - 1)) info = CURLINFO_LASTSOCKET;
    else if (!pg_strncasecmp(info_char, "CURLINFO_LOCAL_PORT", sizeof("CURLINFO_LOCAL_PORT") - 1)) info = CURLINFO_LOCAL_PORT;
    else if (!pg_strncasecmp(info_char, "CURLINFO_NUM_CONNECTS", sizeof("CURLINFO_NUM_CONNECTS") - 1)) info = CURLINFO_NUM_CONNECTS;
    else if (!pg_strncasecmp(info_char, "CURLINFO_OS_ERRNO", sizeof("CURLINFO_OS_ERRNO") - 1)) info = CURLINFO_OS_ERRNO;
    else if (!pg_strncasecmp(info_char, "CURLINFO_PRIMARY_PORT", sizeof("CURLINFO_PRIMARY_PORT") - 1)) info = CURLINFO_PRIMARY_PORT;
    else if (!pg_strncasecmp(info_char, "CURLINFO_PROTOCOL", sizeof("CURLINFO_PROTOCOL") - 1)) info = CURLINFO_PROTOCOL;
    else if (!pg_strncasecmp(info_char, "CURLINFO_PROXYAUTH_AVAIL", sizeof("CURLINFO_PROXYAUTH_AVAIL") - 1)) info = CURLINFO_PROXYAUTH_AVAIL;
    else if (!pg_strncasecmp(info_char, "CURLINFO_PROXY_SSL_VERIFYRESULT", sizeof("CURLINFO_PROXY_SSL_VERIFYRESULT") - 1)) info = CURLINFO_PROXY_SSL_VERIFYRESULT;
    else if (!pg_strncasecmp(info_char, "CURLINFO_REDIRECT_COUNT", sizeof("CURLINFO_REDIRECT_COUNT") - 1)) info = CURLINFO_REDIRECT_COUNT;
    else if (!pg_strncasecmp(info_char, "CURLINFO_REQUEST_SIZE", sizeof("CURLINFO_REQUEST_SIZE") - 1)) info = CURLINFO_REQUEST_SIZE;
    else if (!pg_strncasecmp(info_char, "CURLINFO_RESPONSE_CODE", sizeof("CURLINFO_RESPONSE_CODE") - 1)) info = CURLINFO_RESPONSE_CODE;
    else if (!pg_strncasecmp(info_char, "CURLINFO_RTSP_CLIENT_CSEQ", sizeof("CURLINFO_RTSP_CLIENT_CSEQ") - 1)) info = CURLINFO_RTSP_CLIENT_CSEQ;
    else if (!pg_strncasecmp(info_char, "CURLINFO_RTSP_CSEQ_RECV", sizeof("CURLINFO_RTSP_CSEQ_RECV") - 1)) info = CURLINFO_RTSP_CSEQ_RECV;
    else if (!pg_strncasecmp(info_char, "CURLINFO_RTSP_SERVER_CSEQ", sizeof("CURLINFO_RTSP_SERVER_CSEQ") - 1)) info = CURLINFO_RTSP_SERVER_CSEQ;
    else if (!pg_strncasecmp(info_char, "CURLINFO_SSL_VERIFYRESULT", sizeof("CURLINFO_SSL_VERIFYRESULT") - 1)) info = CURLINFO_SSL_VERIFYRESULT;
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
    PG_RETURN_VOID();
}

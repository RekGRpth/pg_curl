#include <postgres.h>
#include <fmgr.h>

#include <catalog/pg_type.h>
#include <curl/curl.h>
#include <signal.h>
#include <utils/builtins.h>

#define EXTENSION(function) Datum (function)(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(function); Datum (function)(PG_FUNCTION_ARGS)

PG_MODULE_MAGIC;

CURL *curl = NULL;
StringInfoData header_buf;
StringInfoData read_buf;
StringInfoData write_buf;
pqsigfunc pgsql_interrupt_handler = NULL;
int pg_curl_interrupt_requested = 0;
struct curl_slist *header = NULL;
struct curl_slist *recipient = NULL;
curl_mime *mime;
bool has_mime;

static void pg_curl_interrupt_handler(int sig) { pg_curl_interrupt_requested = sig; }
static void *custom_calloc(size_t nmemb, size_t size) { return palloc0(nmemb * size); }
static void custom_free(void *ptr) { if (ptr) (void)pfree(ptr); }

void _PG_init(void); void _PG_init(void) {
    if (curl_global_init_mem(CURL_GLOBAL_ALL, palloc, custom_free, repalloc, pstrdup, custom_calloc)) ereport(ERROR, (errmsg("curl_global_init_mem")));
    pgsql_interrupt_handler = pqsignal(SIGINT, pg_curl_interrupt_handler);
    pg_curl_interrupt_requested = 0;
    (void)initStringInfo(&header_buf);
    (void)initStringInfo(&read_buf);
    (void)initStringInfo(&write_buf);
}

void _PG_fini(void); void _PG_fini(void) {
    (pqsigfunc)pqsignal(SIGINT, pgsql_interrupt_handler);
    (void)curl_easy_cleanup(curl);
    (void)curl_mime_free(mime);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
    (void)curl_global_cleanup();
    (void)pfree(header_buf.data);
    (void)pfree(read_buf.data);
    (void)pfree(write_buf.data);
}

static void pg_curl_easy_init_internal(void) {
    if (curl) return;
    curl = curl_easy_init();
    if (!curl) ereport(ERROR, (errmsg("!curl")));
    mime = curl_mime_init(curl);
    if (!mime) ereport(ERROR, (errmsg("!mime")));
    has_mime = false;
}

EXTENSION(pg_curl_easy_init) { pg_curl_easy_init_internal(); PG_RETURN_BOOL(curl != NULL); }

static void pg_curl_easy_reset_internal(void) {
    if (!curl) return;
    (void)curl_easy_reset(curl);
    (void)curl_mime_free(mime);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
    header = NULL;
    recipient = NULL;
    mime = curl_mime_init(curl);
    if (!mime) ereport(ERROR, (errmsg("!mime")));
    has_mime = false;
    (void)resetStringInfo(&header_buf);
    (void)resetStringInfo(&read_buf);
    (void)resetStringInfo(&write_buf);
}

EXTENSION(pg_curl_easy_reset) { pg_curl_easy_reset_internal(); PG_RETURN_VOID(); }

static void pg_curl_easy_cleanup_internal(void) {
    if (curl) { (void)curl_easy_cleanup(curl); curl = NULL; }
    (void)curl_mime_free(mime);
    (void)curl_slist_free_all(header);
    (void)curl_slist_free_all(recipient);
}

EXTENSION(pg_curl_easy_cleanup) { pg_curl_easy_cleanup_internal(); PG_RETURN_VOID(); }

EXTENSION(pg_curl_easy_escape) {
    text *string;
    char *escape;
    if (!curl) pg_curl_easy_init_internal();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("string is null!")));
    string = PG_GETARG_TEXT_P(0);
    escape = curl_easy_escape(curl, VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string));
    (void)pfree(string);
    if (!escape) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(escape));
}

EXTENSION(pg_curl_easy_unescape) {
    text *url;
    char *unescape;
    int outlength;
    if (!curl) pg_curl_easy_init_internal();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("url is null!")));
    url = PG_GETARG_TEXT_P(0);
    unescape = curl_easy_unescape(curl, VARDATA_ANY(url), VARSIZE_ANY_EXHDR(url), &outlength);
    (void)pfree(url);
    if (!unescape) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(unescape, outlength));
}

EXTENSION(pg_curl_header_append) {
    char *name, *value;
    StringInfoData buf;
    struct curl_slist *temp = header;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("name is null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("value is null!")));
    value = TextDatumGetCString(PG_GETARG_DATUM(1));
    (void)initStringInfo(&buf);
    (void)appendStringInfo(&buf, "%s: %s", name, value);
    if ((temp = curl_slist_append(temp, buf.data))) header = temp; else ereport(ERROR, (errmsg("curl_slist_append")));
    (void)pfree(name);
    (void)pfree(value);
    (void)pfree(buf.data);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_header_append_array) {
    Datum *elemsp;
    bool *nullsp;
    int nelemsp;
    char *name;
    StringInfoData buf;
    struct curl_slist *temp = header;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("name is null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("value is null!")));
    if (array_contains_nulls(DatumGetArrayTypeP(PG_GETARG_DATUM(1)))) ereport(ERROR, (errcode(ERRCODE_ARRAY_ELEMENT_ERROR), errmsg("array_contains_nulls")));
    (void)initStringInfo(&buf);
    (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(1)), TEXTOID, -1, false, 'i', &elemsp, &nullsp, &nelemsp);
    for (int i = 0; i < nelemsp; i++) {
        char *value = TextDatumGetCString(elemsp[i]);
        (void)resetStringInfo(&buf);
        (void)appendStringInfo(&buf, "%s: %s", name, value);
        if ((temp = curl_slist_append(temp, buf.data))) header = temp; else ereport(ERROR, (errmsg("curl_slist_append")));
        (void)pfree(value);
    }
    (void)pfree(name);
    (void)pfree(buf.data);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_header_append_array_array) {
    Datum *name_elemsp, *value_elemsp;
    bool *name_nullsp, *value_nullsp;
    int name_nelemsp, value_nelemsp;
    StringInfoData buf;
    struct curl_slist *temp = header;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("name is null!")));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("value is null!")));
    if (array_contains_nulls(DatumGetArrayTypeP(PG_GETARG_DATUM(0)))) ereport(ERROR, (errcode(ERRCODE_ARRAY_ELEMENT_ERROR), errmsg("array_contains_nulls")));
    if (array_contains_nulls(DatumGetArrayTypeP(PG_GETARG_DATUM(1)))) ereport(ERROR, (errcode(ERRCODE_ARRAY_ELEMENT_ERROR), errmsg("array_contains_nulls")));
    (void)initStringInfo(&buf);
    (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(0)), TEXTOID, -1, false, 'i', &name_elemsp, &name_nullsp, &name_nelemsp);
    (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(1)), TEXTOID, -1, false, 'i', &value_elemsp, &value_nullsp, &value_nelemsp);
    if (name_nelemsp != value_nelemsp) ereport(ERROR, (errmsg("name_nelemsp != value_nelemsp")));
    for (int i = 0; i < name_nelemsp; i++) {
        char *name = TextDatumGetCString(name_elemsp[i]);
        char *value = TextDatumGetCString(value_elemsp[i]);
        (void)resetStringInfo(&buf);
        (void)appendStringInfo(&buf, "%s: %s", name, value);
        if ((temp = curl_slist_append(temp, buf.data))) header = temp; else ereport(ERROR, (errmsg("curl_slist_append")));
        (void)pfree(name);
        (void)pfree(value);
    }
    (void)pfree(buf.data);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_recipient_append) {
    char *email;
    struct curl_slist *temp = recipient;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("email is null!")));
    email = TextDatumGetCString(PG_GETARG_DATUM(0));
    if ((temp = curl_slist_append(temp, email))) recipient = temp; else ereport(ERROR, (errmsg("curl_slist_append")));
    (void)pfree(email);
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_recipient_append_array) {
    Datum *elemsp;
    bool *nullsp;
    int nelemsp;
    struct curl_slist *temp = recipient;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("email is null!")));
    if (array_contains_nulls(DatumGetArrayTypeP(PG_GETARG_DATUM(0)))) ereport(ERROR, (errcode(ERRCODE_ARRAY_ELEMENT_ERROR), errmsg("array_contains_nulls")));
    (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(0)), TEXTOID, -1, false, 'i', &elemsp, &nullsp, &nelemsp);
    for (int i = 0; i < nelemsp; i++) {
        text *email = DatumGetTextP(elemsp[i]);
        if ((temp = curl_slist_append(temp, VARDATA_ANY(email)))) recipient = temp; else ereport(ERROR, (errmsg("curl_slist_append")));
        (void)pfree(email);
    }
    PG_RETURN_BOOL(temp != NULL);
}

EXTENSION(pg_curl_mime_data) {
    CURLcode res = CURL_LAST;
    text *data;
    char *name = NULL, *file = NULL, *type = NULL, *code = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("data is null!")));
    data = PG_GETARG_TEXT_P(0);
    if (!PG_ARGISNULL(1)) name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) file = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) type = TextDatumGetCString(PG_GETARG_DATUM(3));
    if (!PG_ARGISNULL(4)) code = TextDatumGetCString(PG_GETARG_DATUM(4));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_data(part, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data))) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", VARDATA_ANY(data), curl_easy_strerror(res))));
    if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_name(%s): %s", name, curl_easy_strerror(res))));
    if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_filename(%s): %s", file, curl_easy_strerror(res))));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
    if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res))));
    (void)pfree(data);
    if (name) (void)pfree(name);
    if (file) (void)pfree(file);
    if (type) (void)pfree(type);
    if (code) (void)pfree(code);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_mime_data_array) {
    CURLcode res = CURL_LAST;
    Datum *data_elemsp, *name_elemsp = NULL, *file_elemsp = NULL, *type_elemsp = NULL, *code_elemsp = NULL;
    bool *data_nullsp, *name_nullsp = NULL, *file_nullsp = NULL, *type_nullsp = NULL, *code_nullsp = NULL;
    int data_nelemsp, name_nelemsp = 0, file_nelemsp = 0, type_nelemsp = 0, code_nelemsp = 0;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("data is null!")));
    if (array_contains_nulls(DatumGetArrayTypeP(PG_GETARG_DATUM(0)))) ereport(ERROR, (errcode(ERRCODE_ARRAY_ELEMENT_ERROR), errmsg("array_contains_nulls")));
    (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(0)), TEXTOID, -1, false, 'i', &data_elemsp, &data_nullsp, &data_nelemsp);
    if (!PG_ARGISNULL(1)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(1)), TEXTOID, -1, false, 'i', &name_elemsp, &name_nullsp, &name_nelemsp);
        if (data_nelemsp != name_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != name_nelemsp")));
    }
    if (!PG_ARGISNULL(2)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(2)), TEXTOID, -1, false, 'i', &file_elemsp, &file_nullsp, &file_nelemsp);
        if (data_nelemsp != file_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != file_nelemsp")));
    }
    if (!PG_ARGISNULL(3)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(3)), TEXTOID, -1, false, 'i', &type_elemsp, &type_nullsp, &type_nelemsp);
        if (data_nelemsp != type_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != type_nelemsp")));
    }
    if (!PG_ARGISNULL(4)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(4)), TEXTOID, -1, false, 'i', &code_elemsp, &code_nullsp, &code_nelemsp);
        if (data_nelemsp != code_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != code_nelemsp")));
    }
    for (int i = 0; i < data_nelemsp; i++) {
        curl_mimepart *part = curl_mime_addpart(mime);
        char *name = NULL, *file = NULL, *type = NULL, *code = NULL;
        text *data = DatumGetTextP(data_elemsp[i]);
        if (name_nelemsp && !name_nullsp[i]) name = TextDatumGetCString(name_elemsp[i]);
        if (file_nelemsp && !file_nullsp[i]) file = TextDatumGetCString(file_elemsp[i]);
        if (type_nelemsp && !type_nullsp[i]) type = TextDatumGetCString(type_elemsp[i]);
        if (code_nelemsp && !code_nullsp[i]) code = TextDatumGetCString(code_elemsp[i]);
        if ((res = curl_mime_data(part, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data))) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_data(%s): %s", VARDATA_ANY(data), curl_easy_strerror(res))));
        if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_name(%s): %s", name, curl_easy_strerror(res))));
        if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_filename(%s): %s", file, curl_easy_strerror(res))));
        if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
        if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res))));
        (void)pfree(data);
        if (name) (void)pfree(name);
        if (file) (void)pfree(file);
        if (type) (void)pfree(type);
        if (code) (void)pfree(code);
    }
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_mime_file) {
    CURLcode res = CURL_LAST;
    char *data, *name = NULL, *file = NULL, *type = NULL, *code = NULL;
    curl_mimepart *part;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("data is null!")));
    data = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1)) name = TextDatumGetCString(PG_GETARG_DATUM(1));
    if (!PG_ARGISNULL(2)) file = TextDatumGetCString(PG_GETARG_DATUM(2));
    if (!PG_ARGISNULL(3)) type = TextDatumGetCString(PG_GETARG_DATUM(3));
    if (!PG_ARGISNULL(4)) code = TextDatumGetCString(PG_GETARG_DATUM(4));
    part = curl_mime_addpart(mime);
    if ((res = curl_mime_filedata(part, data)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_filedata(%s): %s", data, curl_easy_strerror(res))));
    if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_name(%s): %s", name, curl_easy_strerror(res))));
    if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_filename(%s): %s", file, curl_easy_strerror(res))));
    if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
    if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res))));
    (void)pfree(data);
    if (name) (void)pfree(name);
    if (file) (void)pfree(file);
    if (type) (void)pfree(type);
    if (code) (void)pfree(code);
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_mime_file_array) {
    CURLcode res = CURL_LAST;
    Datum *data_elemsp, *name_elemsp = NULL, *file_elemsp = NULL, *type_elemsp = NULL, *code_elemsp = NULL;
    bool *data_nullsp, *name_nullsp = NULL, *file_nullsp = NULL, *type_nullsp = NULL, *code_nullsp = NULL;
    int data_nelemsp, name_nelemsp = 0, file_nelemsp = 0, type_nelemsp = 0, code_nelemsp = 0;
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("data is null!")));
    if (array_contains_nulls(DatumGetArrayTypeP(PG_GETARG_DATUM(0)))) ereport(ERROR, (errcode(ERRCODE_ARRAY_ELEMENT_ERROR), errmsg("array_contains_nulls")));
    (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(0)), TEXTOID, -1, false, 'i', &data_elemsp, &data_nullsp, &data_nelemsp);
    if (!PG_ARGISNULL(1)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(1)), TEXTOID, -1, false, 'i', &name_elemsp, &name_nullsp, &name_nelemsp);
        if (data_nelemsp != name_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != name_nelemsp")));
    }
    if (!PG_ARGISNULL(2)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(2)), TEXTOID, -1, false, 'i', &file_elemsp, &file_nullsp, &file_nelemsp);
        if (data_nelemsp != file_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != file_nelemsp")));
    }
    if (!PG_ARGISNULL(3)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(3)), TEXTOID, -1, false, 'i', &type_elemsp, &type_nullsp, &type_nelemsp);
        if (data_nelemsp != type_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != type_nelemsp")));
    }
    if (!PG_ARGISNULL(4)) {
        (void)deconstruct_array(DatumGetArrayTypeP(PG_GETARG_DATUM(4)), TEXTOID, -1, false, 'i', &code_elemsp, &code_nullsp, &code_nelemsp);
        if (data_nelemsp != code_nelemsp) ereport(ERROR, (errmsg("data_nelemsp != code_nelemsp")));
    }
    for (int i = 0; i < data_nelemsp; i++) {
        curl_mimepart *part = curl_mime_addpart(mime);
        char *name = NULL, *file = NULL, *type = NULL, *code = NULL, *data = TextDatumGetCString(data_elemsp[i]);
        if (name_nelemsp && !name_nullsp[i]) name = TextDatumGetCString(name_elemsp[i]);
        if (file_nelemsp && !file_nullsp[i]) file = TextDatumGetCString(file_elemsp[i]);
        if (type_nelemsp && !type_nullsp[i]) type = TextDatumGetCString(type_elemsp[i]);
        if (code_nelemsp && !code_nullsp[i]) code = TextDatumGetCString(code_elemsp[i]);
        if ((res = curl_mime_filedata(part, data)) != CURLE_OK) ereport(ERROR, (errmsg("curl_mime_filedata(%s): %s", data, curl_easy_strerror(res))));
        if (name && ((res = curl_mime_name(part, name)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_name(%s): %s", name, curl_easy_strerror(res))));
        if (file && ((res = curl_mime_filename(part, file)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_filename(%s): %s", file, curl_easy_strerror(res))));
        if (type && ((res = curl_mime_type(part, type)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_type(%s): %s", type, curl_easy_strerror(res))));
        if (code && ((res = curl_mime_encoder(part, code)) != CURLE_OK)) ereport(ERROR, (errmsg("curl_mime_encoder(%s): %s", code, curl_easy_strerror(res))));
        (void)pfree(data);
        if (name) (void)pfree(name);
        if (file) (void)pfree(file);
        if (type) (void)pfree(type);
        if (code) (void)pfree(code);
    }
    has_mime = true;
    PG_RETURN_BOOL(res == CURLE_OK);
}

static size_t read_callback(void *buffer, size_t size, size_t nitems, void *instream) {	
    size_t reqsize = size * nitems;
    StringInfo si = (StringInfo)instream;
    size_t remaining = si->len - si->cursor;
    size_t readsize = reqsize < remaining ? reqsize : remaining;
//    elog(LOG, "read_callback: buffer=%s, size=%lu, nitems=%lu, instream=%s", (const char *)buffer, size, nitems, ((StringInfo)instream)->data);
    memcpy(buffer, si->data + si->cursor, readsize);
    si->cursor += readsize;
    return readsize;
}

EXTENSION(pg_curl_easy_setopt_char) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *name, *value_char;
    text *value_text;
    if (!curl) pg_curl_easy_init_internal();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("option is null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("parameter is null!")));
    value_char = TextDatumGetCString(PG_GETARG_DATUM(1));
    value_text = DatumGetTextP(PG_GETARG_DATUM(1));
    if (false);
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ABSTRACT_UNIX_SOCKET", sizeof("CURLOPT_ABSTRACT_UNIX_SOCKET") - 1)) option = CURLOPT_ABSTRACT_UNIX_SOCKET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ACCEPT_ENCODING", sizeof("CURLOPT_ACCEPT_ENCODING") - 1)) option = CURLOPT_ACCEPT_ENCODING;
//    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ALTSVC", sizeof("CURLOPT_ALTSVC") - 1)) option = CURLOPT_ALTSVC;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CAINFO", sizeof("CURLOPT_CAINFO") - 1)) option = CURLOPT_CAINFO;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CAPATH", sizeof("CURLOPT_CAPATH") - 1)) option = CURLOPT_CAPATH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_COOKIEFILE", sizeof("CURLOPT_COOKIEFILE") - 1)) option = CURLOPT_COOKIEFILE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_COOKIEJAR", sizeof("CURLOPT_COOKIEJAR") - 1)) option = CURLOPT_COOKIEJAR;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_COOKIELIST", sizeof("CURLOPT_COOKIELIST") - 1)) option = CURLOPT_COOKIELIST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_COOKIE", sizeof("CURLOPT_COOKIE") - 1)) option = CURLOPT_COOKIE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_COPYPOSTFIELDS", sizeof("CURLOPT_COPYPOSTFIELDS") - 1)) option = CURLOPT_COPYPOSTFIELDS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CRLFILE", sizeof("CURLOPT_CRLFILE") - 1)) option = CURLOPT_CRLFILE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CUSTOMREQUEST", sizeof("CURLOPT_CUSTOMREQUEST") - 1)) option = CURLOPT_CUSTOMREQUEST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DEFAULT_PROTOCOL", sizeof("CURLOPT_DEFAULT_PROTOCOL") - 1)) option = CURLOPT_DEFAULT_PROTOCOL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_INTERFACE", sizeof("CURLOPT_DNS_INTERFACE") - 1)) option = CURLOPT_DNS_INTERFACE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_LOCAL_IP4", sizeof("CURLOPT_DNS_LOCAL_IP4") - 1)) option = CURLOPT_DNS_LOCAL_IP4;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_LOCAL_IP6", sizeof("CURLOPT_DNS_LOCAL_IP6") - 1)) option = CURLOPT_DNS_LOCAL_IP6;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_SERVERS", sizeof("CURLOPT_DNS_SERVERS") - 1)) option = CURLOPT_DNS_SERVERS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DOH_URL", sizeof("CURLOPT_DOH_URL") - 1)) option = CURLOPT_DOH_URL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_EGDSOCKET", sizeof("CURLOPT_EGDSOCKET") - 1)) option = CURLOPT_EGDSOCKET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_ACCOUNT", sizeof("CURLOPT_FTP_ACCOUNT") - 1)) option = CURLOPT_FTP_ACCOUNT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_ALTERNATIVE_TO_USER", sizeof("CURLOPT_FTP_ALTERNATIVE_TO_USER") - 1)) option = CURLOPT_FTP_ALTERNATIVE_TO_USER;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTPPORT", sizeof("CURLOPT_FTPPORT") - 1)) option = CURLOPT_FTPPORT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_INTERFACE", sizeof("CURLOPT_INTERFACE") - 1)) option = CURLOPT_INTERFACE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ISSUERCERT", sizeof("CURLOPT_ISSUERCERT") - 1)) option = CURLOPT_ISSUERCERT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_KEYPASSWD", sizeof("CURLOPT_KEYPASSWD") - 1)) option = CURLOPT_KEYPASSWD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_KRBLEVEL", sizeof("CURLOPT_KRBLEVEL") - 1)) option = CURLOPT_KRBLEVEL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_LOGIN_OPTIONS", sizeof("CURLOPT_LOGIN_OPTIONS") - 1)) option = CURLOPT_LOGIN_OPTIONS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_MAIL_AUTH", sizeof("CURLOPT_MAIL_AUTH") - 1)) option = CURLOPT_MAIL_AUTH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_MAIL_FROM", sizeof("CURLOPT_MAIL_FROM") - 1)) option = CURLOPT_MAIL_FROM;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_NOPROXY", sizeof("CURLOPT_NOPROXY") - 1)) option = CURLOPT_NOPROXY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PASSWORD", sizeof("CURLOPT_PASSWORD") - 1)) option = CURLOPT_PASSWORD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PINNEDPUBLICKEY", sizeof("CURLOPT_PINNEDPUBLICKEY") - 1)) option = CURLOPT_PINNEDPUBLICKEY;
//    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_POSTFIELDS", sizeof("CURLOPT_POSTFIELDS") - 1)) option = CURLOPT_POSTFIELDS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PRE_PROXY", sizeof("CURLOPT_PRE_PROXY") - 1)) option = CURLOPT_PRE_PROXY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_CAINFO", sizeof("CURLOPT_PROXY_CAINFO") - 1)) option = CURLOPT_PROXY_CAINFO;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_CAPATH", sizeof("CURLOPT_PROXY_CAPATH") - 1)) option = CURLOPT_PROXY_CAPATH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_CRLFILE", sizeof("CURLOPT_PROXY_CRLFILE") - 1)) option = CURLOPT_PROXY_CRLFILE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_KEYPASSWD", sizeof("CURLOPT_PROXY_KEYPASSWD") - 1)) option = CURLOPT_PROXY_KEYPASSWD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXYPASSWORD", sizeof("CURLOPT_PROXYPASSWORD") - 1)) option = CURLOPT_PROXYPASSWORD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_PINNEDPUBLICKEY", sizeof("CURLOPT_PROXY_PINNEDPUBLICKEY") - 1)) option = CURLOPT_PROXY_PINNEDPUBLICKEY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SERVICE_NAME", sizeof("CURLOPT_PROXY_SERVICE_NAME") - 1)) option = CURLOPT_PROXY_SERVICE_NAME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY", sizeof("CURLOPT_PROXY") - 1)) option = CURLOPT_PROXY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSLCERT", sizeof("CURLOPT_PROXY_SSLCERT") - 1)) option = CURLOPT_PROXY_SSLCERT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSLCERTTYPE", sizeof("CURLOPT_PROXY_SSLCERTTYPE") - 1)) option = CURLOPT_PROXY_SSLCERTTYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSL_CIPHER_LIST", sizeof("CURLOPT_PROXY_SSL_CIPHER_LIST") - 1)) option = CURLOPT_PROXY_SSL_CIPHER_LIST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSLKEY", sizeof("CURLOPT_PROXY_SSLKEY") - 1)) option = CURLOPT_PROXY_SSLKEY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSLKEYTYPE", sizeof("CURLOPT_PROXY_SSLKEYTYPE") - 1)) option = CURLOPT_PROXY_SSLKEYTYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_TLS13_CIPHERS", sizeof("CURLOPT_PROXY_TLS13_CIPHERS") - 1)) option = CURLOPT_PROXY_TLS13_CIPHERS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_TLSAUTH_PASSWORD", sizeof("CURLOPT_PROXY_TLSAUTH_PASSWORD") - 1)) option = CURLOPT_PROXY_TLSAUTH_PASSWORD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_TLSAUTH_TYPE", sizeof("CURLOPT_PROXY_TLSAUTH_TYPE") - 1)) option = CURLOPT_PROXY_TLSAUTH_TYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_TLSAUTH_USERNAME", sizeof("CURLOPT_PROXY_TLSAUTH_USERNAME") - 1)) option = CURLOPT_PROXY_TLSAUTH_USERNAME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXYUSERNAME", sizeof("CURLOPT_PROXYUSERNAME") - 1)) option = CURLOPT_PROXYUSERNAME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXYUSERPWD", sizeof("CURLOPT_PROXYUSERPWD") - 1)) option = CURLOPT_PROXYUSERPWD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RANDOM_FILE", sizeof("CURLOPT_RANDOM_FILE") - 1)) option = CURLOPT_RANDOM_FILE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RANGE", sizeof("CURLOPT_RANGE") - 1)) option = CURLOPT_RANGE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_READDATA", sizeof("CURLOPT_READDATA") - 1)) {
        (void)appendBinaryStringInfo(&read_buf, VARDATA_ANY(value_text), VARSIZE_ANY_EXHDR(value_text));
        if ((res = curl_easy_setopt(curl, CURLOPT_INFILESIZE, VARSIZE_ANY_EXHDR(value_text))) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_INFILESIZE): %s", curl_easy_strerror(res))));
        if ((res = curl_easy_setopt(curl, CURLOPT_READDATA, (void *)&read_buf)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_READDATA, %s): %s", read_buf.data, curl_easy_strerror(res))));
        if ((res = curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_READFUNCTION): %s", curl_easy_strerror(res))));
        if ((res = curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(CURLOPT_UPLOAD): %s", curl_easy_strerror(res))));
        goto ret;
    }
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_REFERER", sizeof("CURLOPT_REFERER") - 1)) option = CURLOPT_REFERER;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_REQUEST_TARGET", sizeof("CURLOPT_REQUEST_TARGET") - 1)) option = CURLOPT_REQUEST_TARGET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RTSP_SESSION_ID", sizeof("CURLOPT_RTSP_SESSION_ID") - 1)) option = CURLOPT_RTSP_SESSION_ID;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RTSP_STREAM_URI", sizeof("CURLOPT_RTSP_STREAM_URI") - 1)) option = CURLOPT_RTSP_STREAM_URI;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RTSP_TRANSPORT", sizeof("CURLOPT_RTSP_TRANSPORT") - 1)) option = CURLOPT_RTSP_TRANSPORT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SERVICE_NAME", sizeof("CURLOPT_SERVICE_NAME") - 1)) option = CURLOPT_SERVICE_NAME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SOCKS5_GSSAPI_SERVICE", sizeof("CURLOPT_SOCKS5_GSSAPI_SERVICE") - 1)) option = CURLOPT_SOCKS5_GSSAPI_SERVICE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSH_HOST_PUBLIC_KEY_MD5", sizeof("CURLOPT_SSH_HOST_PUBLIC_KEY_MD5") - 1)) option = CURLOPT_SSH_HOST_PUBLIC_KEY_MD5;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSH_KNOWNHOSTS", sizeof("CURLOPT_SSH_KNOWNHOSTS") - 1)) option = CURLOPT_SSH_KNOWNHOSTS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSH_PRIVATE_KEYFILE", sizeof("CURLOPT_SSH_PRIVATE_KEYFILE") - 1)) option = CURLOPT_SSH_PRIVATE_KEYFILE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSH_PUBLIC_KEYFILE", sizeof("CURLOPT_SSH_PUBLIC_KEYFILE") - 1)) option = CURLOPT_SSH_PUBLIC_KEYFILE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSLCERT", sizeof("CURLOPT_SSLCERT") - 1)) option = CURLOPT_SSLCERT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSLCERTTYPE", sizeof("CURLOPT_SSLCERTTYPE") - 1)) option = CURLOPT_SSLCERTTYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_CIPHER_LIST", sizeof("CURLOPT_SSL_CIPHER_LIST") - 1)) option = CURLOPT_SSL_CIPHER_LIST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSLENGINE", sizeof("CURLOPT_SSLENGINE") - 1)) option = CURLOPT_SSLENGINE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSLKEY", sizeof("CURLOPT_SSLKEY") - 1)) option = CURLOPT_SSLKEY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSLKEYTYPE", sizeof("CURLOPT_SSLKEYTYPE") - 1)) option = CURLOPT_SSLKEYTYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TLS13_CIPHERS", sizeof("CURLOPT_TLS13_CIPHERS") - 1)) option = CURLOPT_TLS13_CIPHERS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TLSAUTH_PASSWORD", sizeof("CURLOPT_TLSAUTH_PASSWORD") - 1)) option = CURLOPT_TLSAUTH_PASSWORD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TLSAUTH_TYPE", sizeof("CURLOPT_TLSAUTH_TYPE") - 1)) option = CURLOPT_TLSAUTH_TYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TLSAUTH_USERNAME", sizeof("CURLOPT_TLSAUTH_USERNAME") - 1)) option = CURLOPT_TLSAUTH_USERNAME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_UNIX_SOCKET_PATH", sizeof("CURLOPT_UNIX_SOCKET_PATH") - 1)) option = CURLOPT_UNIX_SOCKET_PATH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_URL", sizeof("CURLOPT_URL") - 1)) option = CURLOPT_URL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_USERAGENT", sizeof("CURLOPT_USERAGENT") - 1)) option = CURLOPT_USERAGENT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_USERNAME", sizeof("CURLOPT_USERNAME") - 1)) option = CURLOPT_USERNAME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_USERPWD", sizeof("CURLOPT_USERPWD") - 1)) option = CURLOPT_USERPWD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_XOAUTH2_BEARER", sizeof("CURLOPT_XOAUTH2_BEARER") - 1)) option = CURLOPT_XOAUTH2_BEARER;
    else ereport(ERROR, (errmsg("unsupported option %s", VARDATA_ANY(name))));
    if ((res = curl_easy_setopt(curl, option, VARDATA_ANY(value_char))) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %s): %s", name, value_char, curl_easy_strerror(res))));
ret:
    (void)pfree(name);
    (void)pfree(value_char);
    (void)pfree(value_text);
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_setopt_long) {
    CURLcode res = CURL_LAST;
    CURLoption option;
    char *name;
    long value;
    if (!curl) pg_curl_easy_init_internal();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("option is null!")));
    name = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (PG_ARGISNULL(1)) ereport(ERROR, (errmsg("parameter is null!")));
    value = PG_GETARG_INT64(1);
    if (false);
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ACCEPTTIMEOUT_MS", sizeof("CURLOPT_ACCEPTTIMEOUT_MS") - 1)) option = CURLOPT_ACCEPTTIMEOUT_MS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ADDRESS_SCOPE", sizeof("CURLOPT_ADDRESS_SCOPE") - 1)) option = CURLOPT_ADDRESS_SCOPE;
//    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_ALTSVC_CTRL", sizeof("CURLOPT_ALTSVC_CTRL") - 1)) option = CURLOPT_ALTSVC_CTRL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_APPEND", sizeof("CURLOPT_APPEND") - 1)) option = CURLOPT_APPEND;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_AUTOREFERER", sizeof("CURLOPT_AUTOREFERER") - 1)) option = CURLOPT_AUTOREFERER;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_BUFFERSIZE", sizeof("CURLOPT_BUFFERSIZE") - 1)) option = CURLOPT_BUFFERSIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CERTINFO", sizeof("CURLOPT_CERTINFO") - 1)) option = CURLOPT_CERTINFO;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CONNECT_ONLY", sizeof("CURLOPT_CONNECT_ONLY") - 1)) option = CURLOPT_CONNECT_ONLY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CONNECTTIMEOUT_MS", sizeof("CURLOPT_CONNECTTIMEOUT_MS") - 1)) option = CURLOPT_CONNECTTIMEOUT_MS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CONNECTTIMEOUT", sizeof("CURLOPT_CONNECTTIMEOUT") - 1)) option = CURLOPT_CONNECTTIMEOUT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_COOKIESESSION", sizeof("CURLOPT_COOKIESESSION") - 1)) option = CURLOPT_COOKIESESSION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_CRLF", sizeof("CURLOPT_CRLF") - 1)) option = CURLOPT_CRLF;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DIRLISTONLY", sizeof("CURLOPT_DIRLISTONLY") - 1)) option = CURLOPT_DIRLISTONLY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DISALLOW_USERNAME_IN_URL", sizeof("CURLOPT_DISALLOW_USERNAME_IN_URL") - 1)) option = CURLOPT_DISALLOW_USERNAME_IN_URL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_CACHE_TIMEOUT", sizeof("CURLOPT_DNS_CACHE_TIMEOUT") - 1)) option = CURLOPT_DNS_CACHE_TIMEOUT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_SHUFFLE_ADDRESSES", sizeof("CURLOPT_DNS_SHUFFLE_ADDRESSES") - 1)) option = CURLOPT_DNS_SHUFFLE_ADDRESSES;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_DNS_USE_GLOBAL_CACHE", sizeof("CURLOPT_DNS_USE_GLOBAL_CACHE") - 1)) option = CURLOPT_DNS_USE_GLOBAL_CACHE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_EXPECT_100_TIMEOUT_MS", sizeof("CURLOPT_EXPECT_100_TIMEOUT_MS") - 1)) option = CURLOPT_EXPECT_100_TIMEOUT_MS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FAILONERROR", sizeof("CURLOPT_FAILONERROR") - 1)) option = CURLOPT_FAILONERROR;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FILETIME", sizeof("CURLOPT_FILETIME") - 1)) option = CURLOPT_FILETIME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FOLLOWLOCATION", sizeof("CURLOPT_FOLLOWLOCATION") - 1)) option = CURLOPT_FOLLOWLOCATION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FORBID_REUSE", sizeof("CURLOPT_FORBID_REUSE") - 1)) option = CURLOPT_FORBID_REUSE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FRESH_CONNECT", sizeof("CURLOPT_FRESH_CONNECT") - 1)) option = CURLOPT_FRESH_CONNECT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_CREATE_MISSING_DIRS", sizeof("CURLOPT_FTP_CREATE_MISSING_DIRS") - 1)) option = CURLOPT_FTP_CREATE_MISSING_DIRS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_FILEMETHOD", sizeof("CURLOPT_FTP_FILEMETHOD") - 1)) option = CURLOPT_FTP_FILEMETHOD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_SKIP_PASV_IP", sizeof("CURLOPT_FTP_SKIP_PASV_IP") - 1)) option = CURLOPT_FTP_SKIP_PASV_IP;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTPSSLAUTH", sizeof("CURLOPT_FTPSSLAUTH") - 1)) option = CURLOPT_FTPSSLAUTH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_SSL_CCC", sizeof("CURLOPT_FTP_SSL_CCC") - 1)) option = CURLOPT_FTP_SSL_CCC;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_USE_EPRT", sizeof("CURLOPT_FTP_USE_EPRT") - 1)) option = CURLOPT_FTP_USE_EPRT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_USE_EPSV", sizeof("CURLOPT_FTP_USE_EPSV") - 1)) option = CURLOPT_FTP_USE_EPSV;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_FTP_USE_PRET", sizeof("CURLOPT_FTP_USE_PRET") - 1)) option = CURLOPT_FTP_USE_PRET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_GSSAPI_DELEGATION", sizeof("CURLOPT_GSSAPI_DELEGATION") - 1)) option = CURLOPT_GSSAPI_DELEGATION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS", sizeof("CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS") - 1)) option = CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HAPROXYPROTOCOL", sizeof("CURLOPT_HAPROXYPROTOCOL") - 1)) option = CURLOPT_HAPROXYPROTOCOL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HEADER", sizeof("CURLOPT_HEADER") - 1)) option = CURLOPT_HEADER;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTP09_ALLOWED", sizeof("CURLOPT_HTTP09_ALLOWED") - 1)) option = CURLOPT_HTTP09_ALLOWED;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTPAUTH", sizeof("CURLOPT_HTTPAUTH") - 1)) option = CURLOPT_HTTPAUTH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTP_CONTENT_DECODING", sizeof("CURLOPT_HTTP_CONTENT_DECODING") - 1)) option = CURLOPT_HTTP_CONTENT_DECODING;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTPGET", sizeof("CURLOPT_HTTPGET") - 1)) option = CURLOPT_HTTPGET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTPPROXYTUNNEL", sizeof("CURLOPT_HTTPPROXYTUNNEL") - 1)) option = CURLOPT_HTTPPROXYTUNNEL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTP_TRANSFER_DECODING", sizeof("CURLOPT_HTTP_TRANSFER_DECODING") - 1)) option = CURLOPT_HTTP_TRANSFER_DECODING;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_HTTP_VERSION", sizeof("CURLOPT_HTTP_VERSION") - 1)) option = CURLOPT_HTTP_VERSION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_IGNORE_CONTENT_LENGTH", sizeof("CURLOPT_IGNORE_CONTENT_LENGTH") - 1)) option = CURLOPT_IGNORE_CONTENT_LENGTH;
//    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_INFILESIZE", sizeof("CURLOPT_INFILESIZE") - 1)) option = CURLOPT_INFILESIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_IPRESOLVE", sizeof("CURLOPT_IPRESOLVE") - 1)) option = CURLOPT_IPRESOLVE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_KEEP_SENDING_ON_ERROR", sizeof("CURLOPT_KEEP_SENDING_ON_ERROR") - 1)) option = CURLOPT_KEEP_SENDING_ON_ERROR;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_LOCALPORTRANGE", sizeof("CURLOPT_LOCALPORTRANGE") - 1)) option = CURLOPT_LOCALPORTRANGE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_LOCALPORT", sizeof("CURLOPT_LOCALPORT") - 1)) option = CURLOPT_LOCALPORT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_LOW_SPEED_LIMIT", sizeof("CURLOPT_LOW_SPEED_LIMIT") - 1)) option = CURLOPT_LOW_SPEED_LIMIT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_LOW_SPEED_TIME", sizeof("CURLOPT_LOW_SPEED_TIME") - 1)) option = CURLOPT_LOW_SPEED_TIME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_MAXCONNECTS", sizeof("CURLOPT_MAXCONNECTS") - 1)) option = CURLOPT_MAXCONNECTS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_MAXFILESIZE", sizeof("CURLOPT_MAXFILESIZE") - 1)) option = CURLOPT_MAXFILESIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_MAXREDIRS", sizeof("CURLOPT_MAXREDIRS") - 1)) option = CURLOPT_MAXREDIRS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_NETRC", sizeof("CURLOPT_NETRC") - 1)) option = CURLOPT_NETRC;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_NEW_DIRECTORY_PERMS", sizeof("CURLOPT_NEW_DIRECTORY_PERMS") - 1)) option = CURLOPT_NEW_DIRECTORY_PERMS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_NEW_FILE_PERMS", sizeof("CURLOPT_NEW_FILE_PERMS") - 1)) option = CURLOPT_NEW_FILE_PERMS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_NOBODY", sizeof("CURLOPT_NOBODY") - 1)) option = CURLOPT_NOBODY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_NOSIGNAL", sizeof("CURLOPT_NOSIGNAL") - 1)) option = CURLOPT_NOSIGNAL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PATH_AS_IS", sizeof("CURLOPT_PATH_AS_IS") - 1)) option = CURLOPT_PATH_AS_IS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PIPEWAIT", sizeof("CURLOPT_PIPEWAIT") - 1)) option = CURLOPT_PIPEWAIT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PORT", sizeof("CURLOPT_PORT") - 1)) option = CURLOPT_PORT;
//    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_POSTFIELDSIZE", sizeof("CURLOPT_POSTFIELDSIZE") - 1)) option = CURLOPT_POSTFIELDSIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_POSTREDIR", sizeof("CURLOPT_POSTREDIR") - 1)) option = CURLOPT_POSTREDIR;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_POST", sizeof("CURLOPT_POST") - 1)) option = CURLOPT_POST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROTOCOLS", sizeof("CURLOPT_PROTOCOLS") - 1)) option = CURLOPT_PROTOCOLS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXYAUTH", sizeof("CURLOPT_PROXYAUTH") - 1)) option = CURLOPT_PROXYAUTH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXYPORT", sizeof("CURLOPT_PROXYPORT") - 1)) option = CURLOPT_PROXYPORT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSL_OPTIONS", sizeof("CURLOPT_PROXY_SSL_OPTIONS") - 1)) option = CURLOPT_PROXY_SSL_OPTIONS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSL_VERIFYHOST", sizeof("CURLOPT_PROXY_SSL_VERIFYHOST") - 1)) option = CURLOPT_PROXY_SSL_VERIFYHOST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSL_VERIFYPEER", sizeof("CURLOPT_PROXY_SSL_VERIFYPEER") - 1)) option = CURLOPT_PROXY_SSL_VERIFYPEER;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_SSLVERSION", sizeof("CURLOPT_PROXY_SSLVERSION") - 1)) option = CURLOPT_PROXY_SSLVERSION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXY_TRANSFER_MODE", sizeof("CURLOPT_PROXY_TRANSFER_MODE") - 1)) option = CURLOPT_PROXY_TRANSFER_MODE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PROXYTYPE", sizeof("CURLOPT_PROXYTYPE") - 1)) option = CURLOPT_PROXYTYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_PUT", sizeof("CURLOPT_PUT") - 1)) option = CURLOPT_PUT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_REDIR_PROTOCOLS", sizeof("CURLOPT_REDIR_PROTOCOLS") - 1)) option = CURLOPT_REDIR_PROTOCOLS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RESUME_FROM", sizeof("CURLOPT_RESUME_FROM") - 1)) option = CURLOPT_RESUME_FROM;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RTSP_CLIENT_CSEQ", sizeof("CURLOPT_RTSP_CLIENT_CSEQ") - 1)) option = CURLOPT_RTSP_CLIENT_CSEQ;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RTSP_REQUEST", sizeof("CURLOPT_RTSP_REQUEST") - 1)) option = CURLOPT_RTSP_REQUEST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_RTSP_SERVER_CSEQ", sizeof("CURLOPT_RTSP_SERVER_CSEQ") - 1)) option = CURLOPT_RTSP_SERVER_CSEQ;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SASL_IR", sizeof("CURLOPT_SASL_IR") - 1)) option = CURLOPT_SASL_IR;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SERVER_RESPONSE_TIMEOUT", sizeof("CURLOPT_SERVER_RESPONSE_TIMEOUT") - 1)) option = CURLOPT_SERVER_RESPONSE_TIMEOUT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SOCKS5_AUTH", sizeof("CURLOPT_SOCKS5_AUTH") - 1)) option = CURLOPT_SOCKS5_AUTH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SOCKS5_GSSAPI_NEC", sizeof("CURLOPT_SOCKS5_GSSAPI_NEC") - 1)) option = CURLOPT_SOCKS5_GSSAPI_NEC;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSH_AUTH_TYPES", sizeof("CURLOPT_SSH_AUTH_TYPES") - 1)) option = CURLOPT_SSH_AUTH_TYPES;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSH_COMPRESSION", sizeof("CURLOPT_SSH_COMPRESSION") - 1)) option = CURLOPT_SSH_COMPRESSION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_ENABLE_ALPN", sizeof("CURLOPT_SSL_ENABLE_ALPN") - 1)) option = CURLOPT_SSL_ENABLE_ALPN;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_ENABLE_NPN", sizeof("CURLOPT_SSL_ENABLE_NPN") - 1)) option = CURLOPT_SSL_ENABLE_NPN;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_FALSESTART", sizeof("CURLOPT_SSL_FALSESTART") - 1)) option = CURLOPT_SSL_FALSESTART;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_OPTIONS", sizeof("CURLOPT_SSL_OPTIONS") - 1)) option = CURLOPT_SSL_OPTIONS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_SESSIONID_CACHE", sizeof("CURLOPT_SSL_SESSIONID_CACHE") - 1)) option = CURLOPT_SSL_SESSIONID_CACHE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_VERIFYHOST", sizeof("CURLOPT_SSL_VERIFYHOST") - 1)) option = CURLOPT_SSL_VERIFYHOST;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_VERIFYPEER", sizeof("CURLOPT_SSL_VERIFYPEER") - 1)) option = CURLOPT_SSL_VERIFYPEER;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSL_VERIFYSTATUS", sizeof("CURLOPT_SSL_VERIFYSTATUS") - 1)) option = CURLOPT_SSL_VERIFYSTATUS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SSLVERSION", sizeof("CURLOPT_SSLVERSION") - 1)) option = CURLOPT_SSLVERSION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_STREAM_WEIGHT", sizeof("CURLOPT_STREAM_WEIGHT") - 1)) option = CURLOPT_STREAM_WEIGHT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_SUPPRESS_CONNECT_HEADERS", sizeof("CURLOPT_SUPPRESS_CONNECT_HEADERS") - 1)) option = CURLOPT_SUPPRESS_CONNECT_HEADERS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TCP_FASTOPEN", sizeof("CURLOPT_TCP_FASTOPEN") - 1)) option = CURLOPT_TCP_FASTOPEN;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TCP_KEEPALIVE", sizeof("CURLOPT_TCP_KEEPALIVE") - 1)) option = CURLOPT_TCP_KEEPALIVE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TCP_KEEPIDLE", sizeof("CURLOPT_TCP_KEEPIDLE") - 1)) option = CURLOPT_TCP_KEEPIDLE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TCP_KEEPINTVL", sizeof("CURLOPT_TCP_KEEPINTVL") - 1)) option = CURLOPT_TCP_KEEPINTVL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TCP_NODELAY", sizeof("CURLOPT_TCP_NODELAY") - 1)) option = CURLOPT_TCP_NODELAY;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TFTP_BLKSIZE", sizeof("CURLOPT_TFTP_BLKSIZE") - 1)) option = CURLOPT_TFTP_BLKSIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TFTP_NO_OPTIONS", sizeof("CURLOPT_TFTP_NO_OPTIONS") - 1)) option = CURLOPT_TFTP_NO_OPTIONS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TIMECONDITION", sizeof("CURLOPT_TIMECONDITION") - 1)) option = CURLOPT_TIMECONDITION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TIMEOUT_MS", sizeof("CURLOPT_TIMEOUT_MS") - 1)) option = CURLOPT_TIMEOUT_MS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TIMEOUT", sizeof("CURLOPT_TIMEOUT") - 1)) option = CURLOPT_TIMEOUT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TIMEVALUE", sizeof("CURLOPT_TIMEVALUE") - 1)) option = CURLOPT_TIMEVALUE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TRANSFER_ENCODING", sizeof("CURLOPT_TRANSFER_ENCODING") - 1)) option = CURLOPT_TRANSFER_ENCODING;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_TRANSFERTEXT", sizeof("CURLOPT_TRANSFERTEXT") - 1)) option = CURLOPT_TRANSFERTEXT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_UNRESTRICTED_AUTH", sizeof("CURLOPT_UNRESTRICTED_AUTH") - 1)) option = CURLOPT_UNRESTRICTED_AUTH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_UPKEEP_INTERVAL_MS", sizeof("CURLOPT_UPKEEP_INTERVAL_MS") - 1)) option = CURLOPT_UPKEEP_INTERVAL_MS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_UPLOAD_BUFFERSIZE", sizeof("CURLOPT_UPLOAD_BUFFERSIZE") - 1)) option = CURLOPT_UPLOAD_BUFFERSIZE;
//    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_UPLOAD", sizeof("CURLOPT_UPLOAD") - 1)) option = CURLOPT_UPLOAD;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_USE_SSL", sizeof("CURLOPT_USE_SSL") - 1)) option = CURLOPT_USE_SSL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_VERBOSE", sizeof("CURLOPT_VERBOSE") - 1)) option = CURLOPT_VERBOSE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLOPT_WILDCARDMATCH", sizeof("CURLOPT_WILDCARDMATCH") - 1)) option = CURLOPT_WILDCARDMATCH;
    else ereport(ERROR, (errmsg("unsupported option %s", VARDATA_ANY(name))));
    if ((res = curl_easy_setopt(curl, option, value)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_setopt(%s, %li): %s", VARDATA_ANY(name), value, curl_easy_strerror(res))));
    (void)pfree(name);
    PG_RETURN_BOOL(res == CURLE_OK);
}

static size_t header_callback(void *buffer, size_t size, size_t nitems, void *outstream) {
    size_t realsize = size * nitems;
//    elog(LOG, "header_callback: buffer=%s, size=%lu, nitems=%lu, outstream=%s", (const char *)buffer, size, nitems, ((StringInfo)outstream)->data);
    (void)appendBinaryStringInfo((StringInfo)outstream, (const char *)buffer, (int)realsize);
    return realsize;
}

static size_t write_callback(void *buffer, size_t size, size_t nitems, void *outstream) {
    size_t realsize = size * nitems;
//    elog(LOG, "write_callback: buffer=%s, size=%lu, nitems=%lu, outstream=%s", (const char *)buffer, size, nitems, ((StringInfo)outstream)->data);
    (void)appendBinaryStringInfo((StringInfo)outstream, (const char *)buffer, (int)realsize);
    return realsize;
}

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) { return pg_curl_interrupt_requested; }

EXTENSION(pg_curl_easy_perform) {
    CURLcode res = CURL_LAST;
    if (!curl) pg_curl_easy_init_internal();
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
    pg_curl_interrupt_requested = 0;
    switch (res = curl_easy_perform(curl)) {
        case CURLE_OK: break;
        case CURLE_ABORTED_BY_CALLBACK: if (pgsql_interrupt_handler && pg_curl_interrupt_requested) { (*pgsql_interrupt_handler)(pg_curl_interrupt_requested); pg_curl_interrupt_requested = 0; }
        default: ereport(ERROR, (errmsg("curl_easy_perform: %s", curl_easy_strerror(res))));
    }
    PG_RETURN_BOOL(res == CURLE_OK);
}

EXTENSION(pg_curl_easy_getinfo_char) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    text *name;
    int len;
    char *value = NULL;
    if (!curl) pg_curl_easy_init_internal();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("info is null!")));
    name = PG_GETARG_TEXT_P(0);
    if (false);
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_CONTENT_TYPE", sizeof("CURLINFO_CONTENT_TYPE") - 1)) info = CURLINFO_CONTENT_TYPE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_EFFECTIVE_URL", sizeof("CURLINFO_EFFECTIVE_URL") - 1)) info = CURLINFO_EFFECTIVE_URL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_FTP_ENTRY_PATH", sizeof("CURLINFO_FTP_ENTRY_PATH") - 1)) info = CURLINFO_FTP_ENTRY_PATH;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_HEADERS", sizeof("CURLINFO_HEADERS") - 1)) { value = header_buf.data; len = header_buf.len; goto ret; }
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_LOCAL_IP", sizeof("CURLINFO_LOCAL_IP") - 1)) info = CURLINFO_LOCAL_IP;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_PRIMARY_IP", sizeof("CURLINFO_PRIMARY_IP") - 1)) info = CURLINFO_PRIMARY_IP;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_PRIVATE", sizeof("CURLINFO_PRIVATE") - 1)) info = CURLINFO_PRIVATE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_REDIRECT_URL", sizeof("CURLINFO_REDIRECT_URL") - 1)) info = CURLINFO_REDIRECT_URL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_RESPONSE", sizeof("CURLINFO_RESPONSE") - 1)) { value = write_buf.data; len = write_buf.len; goto ret; }
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_RTSP_SESSION_ID", sizeof("CURLINFO_RTSP_SESSION_ID") - 1)) info = CURLINFO_RTSP_SESSION_ID;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_SCHEME", sizeof("CURLINFO_SCHEME") - 1)) info = CURLINFO_SCHEME;
    else ereport(ERROR, (errmsg("unsupported option %s", VARDATA_ANY(name))));
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_getinfo(%s): %s", VARDATA_ANY(name), curl_easy_strerror(res))));
    len = value ? strlen(value) : 0;
ret:
    (void)pfree(name);
    if (!value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(value, len));
}

EXTENSION(pg_curl_easy_getinfo_long) {
    CURLcode res = CURL_LAST;
    CURLINFO info;
    text *name;
    long value;
    if (!curl) pg_curl_easy_init_internal();
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("info is null!")));
    name = PG_GETARG_TEXT_P(0);
    if (false);
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_CONDITION_UNMET", sizeof("CURLINFO_CONDITION_UNMET") - 1)) info = CURLINFO_CONDITION_UNMET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_FILETIME", sizeof("CURLINFO_FILETIME") - 1)) info = CURLINFO_FILETIME;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_HEADER_SIZE", sizeof("CURLINFO_HEADER_SIZE") - 1)) info = CURLINFO_HEADER_SIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_HTTPAUTH_AVAIL", sizeof("CURLINFO_HTTPAUTH_AVAIL") - 1)) info = CURLINFO_HTTPAUTH_AVAIL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_HTTP_CONNECTCODE", sizeof("CURLINFO_HTTP_CONNECTCODE") - 1)) info = CURLINFO_HTTP_CONNECTCODE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_HTTP_VERSION", sizeof("CURLINFO_HTTP_VERSION") - 1)) info = CURLINFO_HTTP_VERSION;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_LASTSOCKET", sizeof("CURLINFO_LASTSOCKET") - 1)) info = CURLINFO_LASTSOCKET;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_LOCAL_PORT", sizeof("CURLINFO_LOCAL_PORT") - 1)) info = CURLINFO_LOCAL_PORT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_NUM_CONNECTS", sizeof("CURLINFO_NUM_CONNECTS") - 1)) info = CURLINFO_NUM_CONNECTS;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_OS_ERRNO", sizeof("CURLINFO_OS_ERRNO") - 1)) info = CURLINFO_OS_ERRNO;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_PRIMARY_PORT", sizeof("CURLINFO_PRIMARY_PORT") - 1)) info = CURLINFO_PRIMARY_PORT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_PROTOCOL", sizeof("CURLINFO_PROTOCOL") - 1)) info = CURLINFO_PROTOCOL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_PROXYAUTH_AVAIL", sizeof("CURLINFO_PROXYAUTH_AVAIL") - 1)) info = CURLINFO_PROXYAUTH_AVAIL;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_PROXY_SSL_VERIFYRESULT", sizeof("CURLINFO_PROXY_SSL_VERIFYRESULT") - 1)) info = CURLINFO_PROXY_SSL_VERIFYRESULT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_REDIRECT_COUNT", sizeof("CURLINFO_REDIRECT_COUNT") - 1)) info = CURLINFO_REDIRECT_COUNT;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_REQUEST_SIZE", sizeof("CURLINFO_REQUEST_SIZE") - 1)) info = CURLINFO_REQUEST_SIZE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_RESPONSE_CODE", sizeof("CURLINFO_RESPONSE_CODE") - 1)) info = CURLINFO_RESPONSE_CODE;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_RTSP_CLIENT_CSEQ", sizeof("CURLINFO_RTSP_CLIENT_CSEQ") - 1)) info = CURLINFO_RTSP_CLIENT_CSEQ;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_RTSP_CSEQ_RECV", sizeof("CURLINFO_RTSP_CSEQ_RECV") - 1)) info = CURLINFO_RTSP_CSEQ_RECV;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_RTSP_SERVER_CSEQ", sizeof("CURLINFO_RTSP_SERVER_CSEQ") - 1)) info = CURLINFO_RTSP_SERVER_CSEQ;
    else if (!pg_strncasecmp(VARDATA_ANY(name), "CURLINFO_SSL_VERIFYRESULT", sizeof("CURLINFO_SSL_VERIFYRESULT") - 1)) info = CURLINFO_SSL_VERIFYRESULT;
    else ereport(ERROR, (errmsg("unsupported option %s", VARDATA_ANY(name))));
    if ((res = curl_easy_getinfo(curl, info, &value)) != CURLE_OK) ereport(ERROR, (errmsg("curl_easy_getinfo(%s): %s", VARDATA_ANY(name), curl_easy_strerror(res))));
    (void)pfree(name);
    PG_RETURN_INT64(value);
}

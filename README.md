PostgreSQL cURL allows most curl actions, including data transfer with URL syntax via HTTP, HTTPS, FTP, FTPS, GOPHER, TFTP, SCP, SFTP, SMB, TELNET, DICT, LDAP, LDAPS, FILE, IMAP, SMTP, POP3, RTSP and RTMP

# http get
```sql
CREATE OR REPLACE FUNCTION get(url TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_url(url),
        curl_easy_perform(),
        curl_easy_getinfo_data_in()
    ) SELECT convert_from(curl_easy_getinfo_data_in, 'utf-8') FROM s;
$BODY$;
```

# http urlencoded form post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        ( WITH s AS (
            SELECT (json_each_text(request)).*
        ) SELECT array_agg(curl_postfield_append(key, value)) FROM s),
        curl_easy_setopt_url(url),
        curl_easy_perform(),
        curl_easy_getinfo_data_in()
    ) SELECT convert_from(curl_easy_getinfo_data_in, 'utf-8') FROM s;
$BODY$;
```

# http multipart/form-data form post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        ( WITH s AS (
            SELECT (json_each_text(request)).*
        ) SELECT array_agg(curl_mime_data(value, name:=key)) FROM s),
        curl_easy_setopt_url(url),
        curl_easy_perform(),
        curl_easy_getinfo_data_in()
    ) SELECT convert_from(curl_easy_getinfo_data_in, 'utf-8') FROM s;
$BODY$;
```

# http json post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_postfields(convert_to(request::TEXT, 'utf-8')),
        curl_easy_setopt_url(url),
        curl_header_append('Content-Type', 'application/json; charset=utf-8'),
        curl_easy_perform(),
        curl_easy_getinfo_data_in()
    ) SELECT convert_from(curl_easy_getinfo_data_in, 'utf-8') FROM s;
$BODY$;
```

# email
```sql
CREATE OR REPLACE FUNCTION email(url TEXT, username TEXT, password TEXT, subject TEXT, sender TEXT, recipient TEXT, body TEXT, type TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_mail_from(sender),
        curl_easy_setopt_password(password),
        curl_easy_setopt_url(url),
        curl_easy_setopt_username(username),
        curl_header_append('From', sender),
        curl_header_append('Subject', subject),
        curl_header_append('To', recipient),
        curl_mime_data(body, type:=type),
        curl_recipient_append(recipient),
        curl_easy_perform(),
        curl_easy_getinfo_header_in()
    ) SELECT curl_easy_getinfo_header_in FROM s;
$BODY$;
```

# ftp upload
```sql
CREATE OR REPLACE FUNCTION upload(url TEXT, username TEXT, password TEXT, file BYTEA) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_password(password),
        curl_easy_setopt_readdata(file),
        curl_easy_setopt_url(url),
        curl_easy_setopt_username(username),
        curl_easy_perform(),
        curl_easy_getinfo_header_in()
    ) SELECT curl_easy_getinfo_header_in FROM s;
$BODY$;
```
# ftp download
```sql
CREATE OR REPLACE FUNCTION download(url TEXT, username TEXT, password TEXT) RETURNS BYTEA LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_password(password),
        curl_easy_setopt_url(url),
        curl_easy_setopt_username(username),
        curl_easy_perform(),
        curl_easy_getinfo_data_in()
    ) SELECT curl_easy_getinfo_data_in FROM s;
$BODY$;
```

# convert http headers to table
```sql
WITH s AS (
    SELECT regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') AS s
) SELECT s[1] AS key, s[2] AS value FROM s;
```

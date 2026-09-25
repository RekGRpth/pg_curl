PostgreSQL tool for transferring data with URL syntax, supporting DICT, FILE, FTP, FTPS, GOPHER, GOPHERS, HTTP, HTTPS, IMAP, IMAPS, LDAP, LDAPS, MQTT, POP3, POP3S, RTMP, RTMPS, RTSP, SCP, SFTP, SMB, SMBS, SMTP, SMTPS, TELNET, TFTP, WS and WSS.

# configuration

pg_curl exposes two GUCs:

- `pg_curl.whitelist` (superuser-settable, e.g. via `ALTER ROLE ... SET`) — comma-separated `file://` and `http(s)://` prefixes the current role is allowed to reach. A `file://` entry ending in `/` allows anything under that directory; without a trailing slash it allows only that exact file. An `http(s)://` entry allows any URL sharing that prefix.

  It is checked before every request URL (`curl_easy_setopt_url`) and before every option that has curl open a local file or socket: `curl_mime_file`, `curl_easy_setopt_cookiefile`, `curl_easy_setopt_cookiejar`, `curl_easy_setopt_crlfile`, `curl_easy_setopt_cainfo`, `curl_easy_setopt_capath`, `curl_easy_setopt_issuercert`, `curl_easy_setopt_sslcert`, `curl_easy_setopt_sslkey`, `curl_easy_setopt_pinnedpublickey` (when given a file rather than `sha256//` hashes), their `curl_easy_setopt_proxy_*` counterparts (including `proxy_crlfile`), `curl_easy_setopt_ssh_private_keyfile`, `curl_easy_setopt_ssh_public_keyfile`, `curl_easy_setopt_ssh_knownhosts`, `curl_easy_setopt_random_file`, `curl_easy_setopt_egdsocket` and `curl_easy_setopt_unix_socket_path`. The path is resolved with `realpath()`, so it must already exist — including the `cookiejar` file, which curl writes. `curl_easy_setopt_abstract_unix_socket` has no path to whitelist and is denied whenever the whitelist applies.

  A request URL is classified the way libcurl parses it (scheme case-insensitive, scheme-less URLs guessed): `http(s)://` URLs are matched against `http(s)://` entries, `file://` URLs against `file://` entries, and any other scheme (`ftp://`, `gopher://`, `dict://`, ...) is denied whenever the whitelist applies.

  While `pg_curl.whitelist` is unset, superusers are unrestricted; once it is set, it limits superusers to the listed prefixes too. Every other role is denied by default until a superuser grants specific prefixes:
  ```sql
  ALTER ROLE app_user SET pg_curl.whitelist = 'https://api.example.com/,file:///var/lib/postgresql/uploads/';
  ```

- `pg_curl.transaction` (boolean, default `true`) — keep curl handles in the current transaction's memory context instead of the top memory context, so they are cleaned up automatically at transaction end.

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

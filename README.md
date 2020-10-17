# get
```sql
CREATE OR REPLACE FUNCTION get(url TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_reset(),
        pg_curl_easy_setopt('CURLOPT_URL', url),
        pg_curl_easy_perform(),
        pg_curl_easy_response()
    ) SELECT convert_from(pg_curl_easy_response, 'utf-8') FROM s;
$BODY$;
```

# urlencoded post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_reset(),
        pg_curl_easy_setopt('CURLOPT_URL', url),
        pg_curl_easy_setopt('CURLOPT_COPYPOSTFIELDS', (
            WITH s AS (
                SELECT (json_each_text(request)).*
            ) SELECT convert_to(array_to_string(array_agg(concat_ws('=',
                pg_curl_easy_escape(key),
                pg_curl_easy_escape(value)
            )), '&'), 'utf-8') FROM s
        )),
        pg_curl_easy_perform(),
        pg_curl_easy_response()
    ) SELECT convert_from(pg_curl_easy_response, 'utf-8') FROM s;
$BODY$;
```

# json post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_reset(),
        pg_curl_easy_setopt('CURLOPT_URL', url),
        pg_curl_header_append('Content-Type', 'application/json; charset=utf-8'),
        pg_curl_easy_setopt('CURLOPT_COPYPOSTFIELDS', convert_to(request::TEXT, 'utf-8')),
        pg_curl_easy_perform(),
        pg_curl_easy_response()
    ) SELECT convert_from(pg_curl_easy_response, 'utf-8') FROM s;
$BODY$;
```

# send email
```sql
CREATE OR REPLACE FUNCTION email(url TEXT, username TEXT, password TEXT, subject TEXT, "from" TEXT, "to" TEXT[], data TEXT, type TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_reset(),
        pg_curl_easy_setopt('CURLOPT_URL', url),
        pg_curl_easy_setopt('CURLOPT_USERNAME', username),
        pg_curl_easy_setopt('CURLOPT_PASSWORD', password),
        pg_curl_recipient_append("to"),
        pg_curl_header_append('Subject', subject),
        pg_curl_header_append('From', "from"),
        pg_curl_header_append('To', "to"),
        pg_curl_mime_data(data, type:=type),
        pg_curl_easy_perform(),
        pg_curl_easy_headers()
    ) SELECT pg_curl_easy_headers FROM s;
$BODY$;
```

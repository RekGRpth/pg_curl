# get
```sql
CREATE OR REPLACE FUNCTION get(url TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_init(),
        pg_curl_easy_reset(),
        pg_curl_easy_setopt_char('CURLOPT_URL', url),
        pg_curl_header_append('Connection', 'close'),
        pg_curl_easy_perform(),
        pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        pg_curl_easy_cleanup()
    ) SELECT pg_curl_easy_getinfo_char FROM s;
$BODY$;
```

# urlencoded post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request RECORD) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_init(),
        pg_curl_easy_reset(),
        pg_curl_easy_setopt_char('CURLOPT_URL', url),
        pg_curl_header_append('Connection', 'close'),
        pg_curl_easy_setopt_char('CURLOPT_COPYPOSTFIELDS', (
            WITH s AS (
                SELECT (json_each_text(row_to_json(request))).*
            ) SELECT array_to_string(array_agg(concat_ws('=',
                pg_curl_easy_escape(key),
                pg_curl_easy_escape(value)
            )), '&') FROM s
        )),
        pg_curl_easy_perform(),
        pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        pg_curl_easy_cleanup()
    ) SELECT pg_curl_easy_getinfo_char FROM s;
$BODY$;
```

# json post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        pg_curl_easy_init(),
        pg_curl_easy_reset(),
        pg_curl_easy_setopt_char('CURLOPT_URL', url),
        pg_curl_header_append('Content-Type', 'application/json; charset=utf-8'),
        pg_curl_header_append('Connection', 'close'),
        pg_curl_easy_setopt_char('CURLOPT_COPYPOSTFIELDS', request::TEXT),
        pg_curl_easy_perform(),
        pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        pg_curl_easy_cleanup()
    ) SELECT pg_curl_easy_getinfo_char FROM s;
$BODY$;
```

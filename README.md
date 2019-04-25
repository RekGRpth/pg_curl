# get
```sql
create or replace function get(url text) returns text language sql as $body$
    with s as (select
        pg_curl_easy_init(),
        pg_curl_easy_reset(),
        pg_curl_easy_setopt_char('CURLOPT_URL', url),
        pg_curl_header_append('Connection', 'close'),
        pg_curl_easy_perform(),
        pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        pg_curl_easy_cleanup()
    ) select pg_curl_easy_getinfo_char from s;
$body$;
```

# urlencoded post
```sql
create or replace function post(url text, request json) returns text language sql as $body$
    with s as (select
        pg_curl_easy_init(),
        pg_curl_easy_reset(),
        pg_curl_easy_setopt_char('CURLOPT_URL', url),
        pg_curl_header_append('Connection', 'close'),
        pg_curl_easy_setopt_char('CURLOPT_COPYPOSTFIELDS', (
            with s as (
                select (json_each_text(request)).*
            ) select array_to_string(array_agg(concat_ws('=',
                pg_curl_easy_escape(key),
                pg_curl_easy_escape(value)
            )), '&') from s
        )),
        pg_curl_easy_perform(),
        pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        pg_curl_easy_cleanup()
    ) select pg_curl_easy_getinfo_char from s;
$body$;
```

# json post
```sql
create or replace function post(url text, request json) returns text language sql as $body$
    with s as (select
        pg_curl_easy_init(),
        pg_curl_easy_reset(),
        pg_curl_easy_setopt_char('CURLOPT_URL', url),
        pg_curl_header_append('Content-Type', 'application/json; charset=utf-8'),
        pg_curl_header_append('Connection', 'close'),
        pg_curl_easy_setopt_char('CURLOPT_COPYPOSTFIELDS', request::text),
        pg_curl_easy_perform(),
        pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        pg_curl_easy_cleanup()
    ) select pg_curl_easy_getinfo_char from s;
$body$;
```

# urlencoded form post
```sql
create or replace function post(url text, request json) returns text language sql as $body$
    with s as (select
        curl.pg_curl_easy_init(),
        curl.pg_curl_easy_reset(),
        --curl.pg_curl_easy_setopt_long('CURLOPT_VERBOSE', 1),
        --curl.pg_curl_easy_setopt_long('CURLOPT_FORBID_REUSE', 1),
        curl.pg_curl_easy_setopt_char('CURLOPT_URL', url),
        --curl.pg_curl_header_append('Content-Type', 'application/x-www-form-urlencoded'),
        curl.pg_curl_header_append('Connection', 'close'),
        curl.pg_curl_easy_setopt_char('CURLOPT_COPYPOSTFIELDS', (
            with s as (
                select (json_each_text(request)).*
            ) select array_to_string(array_agg(concat_ws('=',
                curl.pg_curl_easy_escape(key),
                curl.pg_curl_easy_escape(value)
            )), '&') from s
        )),
        curl.pg_curl_easy_perform(),
        curl.pg_curl_easy_getinfo_char('CURLINFO_RESPONSE'),
        curl.pg_curl_easy_cleanup()
    ) select pg_curl_easy_getinfo_char from s;
$body$;
```

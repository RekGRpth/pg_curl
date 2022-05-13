\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
\set ON_ERROR_ROLLBACK 1
\set ON_ERROR_STOP true
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_reset();
select curl_easy_setopt_url('https://httpbin.org/get?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) - '{origin}'::text[]);
select curl_easy_reset();
select curl_easy_setopt_postfields(convert_to('{"e":"f","g":"","h":null}', 'utf-8'));
select curl_easy_setopt_url('https://httpbin.org/post?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_header_append('Content-Type', 'application/json; charset=utf-8');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) - '{origin}'::text[]);
select curl_easy_reset();
select curl_postfield_append('e', 'f');
select curl_postfield_append('g', '');
select curl_postfield_append('h');
select curl_easy_setopt_url('https://httpbin.org/post?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) - '{origin}'::text[]);
select curl_easy_reset();
select curl_easy_setopt_url('https://httpbin.org/post?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_mime_data('f', name:='e');
select curl_mime_data('', name:='g');
select curl_mime_data(null, name:='h');
select curl_mime_data('content', name:='filename.txt', file:='filename.txt');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select jsonb_pretty(((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) #- '{headers,Content-Type}'::text[]) - '{origin}'::text[]);
ROLLBACK;

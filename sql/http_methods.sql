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
select curl_easy_setopt_url('https://httpbin.org/get?a=b&c=&d');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date');
select  jsonb_pretty(jsonb_set_lax(convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb, '{headers,X-Amzn-Trace-Id}', null, true, 'delete_key'));

select curl_easy_reset();
select curl_easy_setopt_postfields(convert_to('{"e":"f","g":"","h":null}', 'utf-8'));
select curl_easy_setopt_url('https://httpbin.org/post?a=b&c=&d');
select curl_header_append('Content-Type', 'application/json; charset=utf-8');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date');
select  jsonb_pretty(jsonb_set_lax(convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb, '{headers,X-Amzn-Trace-Id}', null, true, 'delete_key'));
ROLLBACK;

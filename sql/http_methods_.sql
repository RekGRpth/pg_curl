\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_setopt_url('https://httpbin.org/get?', conname:='1');
select curl_url_append('a', 'b', conname:='1');
select curl_url_append('c', '', conname:='1');
select curl_url_append('d', conname:='1');
select curl_easy_setopt_postfields(convert_to('{"e":"f","g":"","h":null}', 'utf-8'), conname:='2');
select curl_easy_setopt_url('https://httpbin.org/post?', conname:='2');
select curl_url_append('a', 'b', conname:='2');
select curl_url_append('c', '', conname:='2');
select curl_url_append('d', conname:='2');
select curl_header_append('Content-Type', 'application/json; charset=utf-8', conname:='2');
select curl_postfield_append('e', 'f', conname:='3');
select curl_postfield_append('g', '', conname:='3');
select curl_postfield_append('h', conname:='3');
select curl_easy_setopt_url('https://httpbin.org/post?', conname:='3');
select curl_url_append('a', 'b', conname:='3');
select curl_url_append('c', '', conname:='3');
select curl_url_append('d', conname:='3');
select curl_easy_setopt_url('https://httpbin.org/post?', conname:='4');
select curl_url_append('a', 'b', conname:='4');
select curl_url_append('c', '', conname:='4');
select curl_url_append('d', conname:='4');
select curl_mime_data('f', name:='e', conname:='4');
select curl_mime_data('', name:='g', conname:='4');
select curl_mime_data(null, name:='h', conname:='4');
select curl_mime_data('content', name:='filename.txt', file:='filename.txt', conname:='4');
select curl_multi_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='1'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='2'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='3'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='4'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(conname:='1'), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) - 'origin');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(conname:='2'), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) - 'origin');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(conname:='3'), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) - 'origin');
select jsonb_pretty((((convert_from(curl_easy_getinfo_data_in(conname:='4'), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[]) #- '{headers,Content-Type}'::text[]) #- '{headers,Content-Length}'::text[]) - 'origin');
select curl_easy_getinfo_errcode(conname:='1'), curl_easy_getinfo_errdesc(conname:='1'), curl_easy_getinfo_errbuf(conname:='1');
select curl_easy_getinfo_errcode(conname:='2'), curl_easy_getinfo_errdesc(conname:='2'), curl_easy_getinfo_errbuf(conname:='2');
select curl_easy_getinfo_errcode(conname:='3'), curl_easy_getinfo_errdesc(conname:='3'), curl_easy_getinfo_errbuf(conname:='3');
select curl_easy_getinfo_errcode(conname:='4'), curl_easy_getinfo_errdesc(conname:='4'), curl_easy_getinfo_errbuf(conname:='4');
ROLLBACK;

\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
\set ON_ERROR_ROLLBACK 1
\set ON_ERROR_STOP true
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_setopt_password('wrong', conname:='1');
select curl_easy_setopt_url('https://httpbin.org/basic-auth/username/password', conname:='1');
select curl_easy_setopt_username('username', conname:='1');
select curl_easy_setopt_password('password', conname:='2');
select curl_easy_setopt_url('https://httpbin.org/basic-auth/username/password', conname:='2');
select curl_easy_setopt_username('username', conname:='2');
select curl_easy_setopt_url('https://httpbin.org/bearer', conname:='3');
select curl_header_append('Authorization', 'Bearer token', conname:='3');
select curl_easy_perform(conname:='1');
select curl_easy_perform(conname:='2');
select curl_easy_perform(conname:='3');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='1'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='2'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='3'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select convert_from(curl_easy_getinfo_data_in(conname:='1'), 'utf-8');
select convert_from(curl_easy_getinfo_data_in(conname:='2'), 'utf-8');
select convert_from(curl_easy_getinfo_data_in(conname:='3'), 'utf-8');
ROLLBACK;

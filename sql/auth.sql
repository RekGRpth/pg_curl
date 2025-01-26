\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_reset();
select curl_easy_setopt_password('wrong');
select curl_easy_setopt_url('https://httpbin.org/basic-auth/username/password');
select curl_easy_setopt_username('username');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select convert_from(curl_easy_getinfo_data_in(), 'utf-8');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
select curl_easy_reset();
select curl_easy_setopt_password('password');
select curl_easy_setopt_url('https://httpbin.org/basic-auth/username/password');
select curl_easy_setopt_username('username');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select convert_from(curl_easy_getinfo_data_in(), 'utf-8');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
select curl_easy_reset();
select curl_easy_setopt_url('https://httpbin.org/bearer');
select curl_header_append('Authorization', 'Bearer token');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select s[1] as key, s[2] as value from s where s[1] not in ('date', 'server', 'content-length');
select convert_from(curl_easy_getinfo_data_in(), 'utf-8');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
ROLLBACK;

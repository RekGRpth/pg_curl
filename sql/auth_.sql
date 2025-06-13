\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
DO $plpgsql$ BEGIN
    BEGIN
        PERFORM curl_easy_reset();
        PERFORM curl_easy_setopt_timeout(1);
        PERFORM curl_easy_setopt_url('http://localhost/status/202');
        PERFORM curl_easy_perform();
        SET pg_curl.httpbin = 'http://localhost';
    EXCEPTION WHEN OTHERS THEN
        SET pg_curl.httpbin = 'https://httpbin.org';
    END;
END;$plpgsql$;
BEGIN;
select curl_easy_reset(conname:='1');
select curl_easy_reset(conname:='2');
select curl_easy_reset(conname:='3');
select curl_easy_setopt_password('wrong', conname:='1');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/basic-auth/username/password', conname:='1');
select curl_easy_setopt_username('username', conname:='1');
select curl_easy_setopt_password('password', conname:='2');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/basic-auth/username/password', conname:='2');
select curl_easy_setopt_username('username', conname:='2');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/bearer', conname:='3');
select curl_header_append('Authorization', 'Bearer token', conname:='3');
select curl_multi_add_handle(conname:='1');
select curl_multi_add_handle(conname:='2');
select curl_multi_add_handle(conname:='3');
select curl_multi_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='1'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='2'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='3'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select convert_from(curl_easy_getinfo_data_in(conname:='1'), 'utf-8')::jsonb;
select convert_from(curl_easy_getinfo_data_in(conname:='2'), 'utf-8')::jsonb;
select convert_from(curl_easy_getinfo_data_in(conname:='3'), 'utf-8')::jsonb;
select curl_easy_getinfo_errcode(conname:='1'), curl_easy_getinfo_errdesc(conname:='1'), curl_easy_getinfo_errbuf(conname:='1');
select curl_easy_getinfo_errcode(conname:='2'), curl_easy_getinfo_errdesc(conname:='2'), curl_easy_getinfo_errbuf(conname:='2');
select curl_easy_getinfo_errcode(conname:='3'), curl_easy_getinfo_errdesc(conname:='3'), curl_easy_getinfo_errbuf(conname:='3');
END;

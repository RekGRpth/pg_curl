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
select curl_easy_reset();
select curl_easy_setopt_password('wrong');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/basic-auth/username/password');
select curl_easy_setopt_username('username');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb;
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_password('password');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/basic-auth/username/password');
select curl_easy_setopt_username('username');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb;
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/bearer');
select curl_header_append('Authorization', 'Bearer token');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb;
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;

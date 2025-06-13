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
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[] #- '{headers,Host}'::text[]) - 'origin' - 'url');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_postfields(convert_to('{"e":"f","g":"","h":null}', 'utf-8'));
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_header_append('Content-Type', 'application/json; charset=utf-8');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[] #- '{headers,Host}'::text[]) - 'origin' - 'url');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;
BEGIN;
select curl_easy_reset();
select curl_postfield_append('e', 'f');
select curl_postfield_append('g', '');
select curl_postfield_append('h');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?');
select curl_url_append('a', 'b');
select curl_url_append('c', '');
select curl_url_append('d');
select curl_easy_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select jsonb_pretty((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[] #- '{headers,Host}'::text[]) - 'origin' - 'url');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?');
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
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
select jsonb_pretty((((convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb #- '{headers,X-Amzn-Trace-Id}'::text[] #- '{headers,Host}'::text[]) #- '{headers,Content-Type}'::text[]) #- '{headers,Content-Length}'::text[]) - 'origin' - 'url');
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc(), curl_easy_getinfo_errbuf();
END;

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
        PERFORM curl_easy_getinfo_http_connectcode();
        SET pg_curl.httpbin = 'http://localhost';
    EXCEPTION WHEN OTHERS THEN
        SET pg_curl.httpbin = 'https://httpbin.org';
    END;
END;$plpgsql$;
BEGIN;
select curl_easy_reset(conname:='1');
select curl_easy_reset(conname:='2');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/delay/2', conname:='1');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/delay/3', conname:='2');
set statement_timeout = '1s';
select curl_multi_add_handle(conname:='1');
select curl_multi_add_handle(conname:='2');
select curl_multi_perform();
END;

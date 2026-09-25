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
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/headers');
select curl_header_append('X-Pg-Curl-Test', 'one');
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb -> 'headers' ->> 'X-Pg-Curl-Test') = 'one' as first_sent;
-- curl_easy_header_reset() frees the header list; the easy handle must be
-- detached from it too, or the next perform on the same handle walks freed
-- memory (garbage headers or a backend crash) instead of sending none
select curl_easy_header_reset();
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb -> 'headers' ->> 'X-Pg-Curl-Test') is null as reset_not_sent;
-- a header appended after the reset is sent as usual
select curl_header_append('X-Pg-Curl-Test', 'two');
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb -> 'headers' ->> 'X-Pg-Curl-Test') = 'two' as new_sent;
END;

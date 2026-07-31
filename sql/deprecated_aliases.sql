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
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get');
select curl_easy_perform();
-- curl_easy_getinfo_headers/_response are deprecated aliases that must warn
-- and still delegate to header_in/data_in with identical results
select curl_easy_getinfo_headers() = curl_easy_getinfo_header_in() as headers_alias_matches;
select curl_easy_getinfo_response() = curl_easy_getinfo_data_in() as response_alias_matches;
END;

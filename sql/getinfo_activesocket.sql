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
-- the handle is removed from the multi once the transfer is done, so there
-- is no active socket left: CURL_SOCKET_BAD must come back as exactly -1,
-- not as a curl_socket_t written into the low half of an uninitialized long
select curl_easy_getinfo_activesocket() = -1 as no_active_socket;
END;

\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
-- a freshly reset connection has not performed any request yet, so
-- errcode/errdesc must not falsely report success
BEGIN;
select curl_easy_reset();
select curl_easy_getinfo_errcode() <> 0 as errcode_not_ok;
select curl_easy_getinfo_errdesc() <> 'No error' as errdesc_not_ok;
END;
-- and getters that gate on pg_curl_check_error() must refuse to report
-- data from a connection that has never been performed
BEGIN;
select curl_easy_reset();
select curl_easy_getinfo_data_in();
END;
-- sanity: after an actual successful perform, errcode/errdesc go back
-- to reporting success
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
select curl_easy_getinfo_errcode(), curl_easy_getinfo_errdesc();
END;

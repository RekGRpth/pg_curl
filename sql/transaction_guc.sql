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
-- default pg_curl.transaction = true: connection state lives in
-- CurTransactionContext, so it is torn down when the transaction commits
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get');
select curl_easy_perform();
select curl_easy_getinfo_errcode() = 0 as ran_ok;
COMMIT;
select curl_easy_getinfo_errcode() <> 0 as state_reset_between_transactions;
-- pg_curl.transaction = false: connection state lives in TopMemoryContext,
-- which is not tied to the transaction lifecycle, so it must survive
select set_config('pg_curl.transaction', 'false', false);
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get');
select curl_easy_perform();
select curl_easy_getinfo_errcode() = 0 as ran_ok;
COMMIT;
select curl_easy_getinfo_errcode() = 0 as state_persists_across_transactions;

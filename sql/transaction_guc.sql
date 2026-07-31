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
-- CurTransactionContext, so it is torn down when the transaction commits.
-- MemoryContextRegisterResetCallback (needed for that) only exists since
-- PostgreSQL 9.5, so on 9.4 pg_curl always falls back to TopMemoryContext
-- regardless of this GUC -- state surviving there too is expected on 9.4,
-- not a bug, hence comparing against the server's actual capability below
-- instead of hardcoding "must reset".
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get');
select curl_easy_perform();
select curl_easy_getinfo_errcode() = 0 as ran_ok;
COMMIT;
select (curl_easy_getinfo_errcode() <> 0) = (current_setting('server_version_num')::int >= 90500) as state_reset_matches_capability;
-- pg_curl.transaction = false: connection state lives in TopMemoryContext,
-- which is not tied to the transaction lifecycle, so it must survive
-- regardless of PostgreSQL version. set_config()'s return value is
-- compared loosely since older PostgreSQL versions may not canonicalize
-- a custom boolean GUC's display value to on/off.
select set_config('pg_curl.transaction', 'false', false) IN ('off', 'false') as transaction_guc_set;
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get');
select curl_easy_perform();
select curl_easy_getinfo_errcode() = 0 as ran_ok;
COMMIT;
select curl_easy_getinfo_errcode() = 0 as state_persists_across_transactions;

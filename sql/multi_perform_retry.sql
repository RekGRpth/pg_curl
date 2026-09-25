\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
-- terse: the WARNING detail carries curl's timing ("after 0 ms"), which is
-- not stable across runs
\set VERBOSITY terse
-- nothing listens on port 1, so every attempt is refused immediately. A
-- failed attempt that is going to be retried raises a WARNING, so try := 3
-- must give exactly two WARNINGs (the third attempt is the last one), and
-- the call must still report the failure.
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url('http://127.0.0.1:1/');
select curl_easy_perform(try := 3, sleep := 0) as should_be_false;
select curl_easy_getinfo_errcode();
END;

\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
\set ON_ERROR_ROLLBACK 1
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_reset();
select curl_easy_setopt_url('https://httpbin.org/delay/10');
set statement_timeout = '5s';
select curl_easy_perform();
ROLLBACK;

\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
\set ON_ERROR_ROLLBACK 1
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_setopt_url('https://httpbin.org/delay/10', conname:='1');
select curl_easy_setopt_url('https://httpbin.org/delay/7', conname:='2');
set statement_timeout = '5s';
select curl_multi_perform();
ROLLBACK;

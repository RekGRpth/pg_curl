\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
CREATE EXTENSION pg_curl;
select curl_easy_setopt_url('https://httpbin.org/delay/2', conname:='1');
select curl_easy_setopt_url('https://httpbin.org/delay/3', conname:='2');
set statement_timeout = '1s';
select curl_multi_perform();
ROLLBACK;

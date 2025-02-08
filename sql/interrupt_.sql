\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
BEGIN;
select curl_easy_reset(conname:='1');
select curl_easy_reset(conname:='2');
select curl_easy_setopt_url('https://httpbin.org/delay/2', conname:='1');
select curl_easy_setopt_url('https://httpbin.org/delay/3', conname:='2');
set statement_timeout = '1s';
select curl_multi_add_handle(conname:='1');
select curl_multi_add_handle(conname:='2');
select curl_multi_perform();
END;

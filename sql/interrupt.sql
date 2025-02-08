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
select curl_easy_reset();
select curl_easy_setopt_url('https://httpbin.org/delay/2');
set statement_timeout = '1s';
select curl_easy_perform();
END;

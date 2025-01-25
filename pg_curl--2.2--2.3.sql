-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

DROP FUNCTION curl_easy_perform(try int, sleep bigint);
CREATE FUNCTION curl_easy_perform(try int DEFAULT 1, sleep bigint DEFAULT 1000000, timeout_ms int DEFAULT 1000) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_multi_perform' LANGUAGE 'c';

DROP FUNCTION curl_multi_perform(try int, sleep bigint, timeout_ms);
CREATE FUNCTION curl_multi_perform(try int DEFAULT 1, sleep bigint DEFAULT 1000000, timeout_ms int DEFAULT 1000) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_multi_perform' LANGUAGE 'c';

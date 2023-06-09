-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

DROP FUNCTION curl_easy_perform;

CREATE FUNCTION curl_easy_perform(try int DEFAULT 1, sleep bigint DEFAULT 1000000) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_perform' LANGUAGE 'c';

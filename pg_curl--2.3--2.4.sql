-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

DROP FUNCTION curl_easy_header_reset(conname NAME);
CREATE FUNCTION curl_easy_header_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_header_reset' LANGUAGE 'c';
DROP FUNCTION curl_easy_mime_reset(conname NAME);
CREATE FUNCTION curl_easy_mime_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_mime_reset' LANGUAGE 'c';
DROP FUNCTION curl_easy_postquote_reset(conname NAME);
CREATE FUNCTION curl_easy_postquote_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_postquote_reset' LANGUAGE 'c';
DROP FUNCTION curl_easy_prequote_reset(conname NAME);
CREATE FUNCTION curl_easy_prequote_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_prequote_reset' LANGUAGE 'c';
DROP FUNCTION curl_easy_quote_reset(conname NAME);
CREATE FUNCTION curl_easy_quote_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_quote_reset' LANGUAGE 'c';
DROP FUNCTION curl_easy_recipient_reset(conname NAME);
CREATE FUNCTION curl_easy_recipient_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_recipient_reset' LANGUAGE 'c';
DROP FUNCTION curl_easy_reset(conname NAME);
CREATE FUNCTION curl_easy_reset(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_reset' LANGUAGE 'c';

CREATE FUNCTION curl_multi_add_handle(conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_multi_add_handle' LANGUAGE 'c';

DROP FUNCTION curl_easy_perform(try int, sleep bigint, timeout_ms int);
CREATE FUNCTION curl_easy_perform(try int DEFAULT 1, sleep bigint DEFAULT 1000000, timeout_ms int DEFAULT 1000) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_perform' LANGUAGE 'c';

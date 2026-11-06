-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

CREATE FUNCTION curl_mime_data(data bytea, head text[], name text DEFAULT null, file text DEFAULT null, type text DEFAULT null, code text DEFAULT null, conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_data_bytea' LANGUAGE 'c';
CREATE FUNCTION curl_mime_data(data text, head text[], name text DEFAULT null, file text DEFAULT null, type text DEFAULT null, code text DEFAULT null, conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_data_text' LANGUAGE 'c';
CREATE FUNCTION curl_mime_file(data text, head text[], name text DEFAULT null, file text DEFAULT null, type text DEFAULT null, code text DEFAULT null, conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_file' LANGUAGE 'c';

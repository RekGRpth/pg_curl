-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

CREATE FUNCTION curl_easy_setopt_readdata(parameter bytea, conname NAME DEFAULT NULL) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_readdata' LANGUAGE 'c';

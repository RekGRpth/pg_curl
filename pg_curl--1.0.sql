-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

CREATE OR REPLACE FUNCTION pg_curl_easy_init() RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_init' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_reset' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_setopt(option text, parameter text) RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_setopt(option text, parameter bigint) RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_perform() RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_perform' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_cleanup() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_cleanup' LANGUAGE 'c';

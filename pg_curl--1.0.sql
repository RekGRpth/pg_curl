-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

CREATE OR REPLACE FUNCTION pg_curl_easy_init() RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_init' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_reset' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_cleanup() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_cleanup' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_escape(string text, length int default 0) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_escape' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_unescape(url text, length int default 0) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_unescape' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_header_append(name text, value text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_header_append' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_header_append(name text, value text[]) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_header_append_array' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_header_append(name text[], value text[]) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_header_append_array_array' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_recipient_append(email text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_recipient_append' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_recipient_append(email text[]) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_recipient_append_array' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_mime_data(data text, name text default null, file text default null, type text default null, code text default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_data' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_mime_data(data text[], name text[] default null, file text[] default null, type text[] default null, code text[] default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_data_array' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_mime_file(data text, name text default null, type text default null, code text default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_file' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_mime_file(data text[], name text[] default null, type text[] default null, code text[] default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_file_array' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_setopt_char(option text, parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_char' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_setopt_long(option text, parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_long' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_setopt(option text, parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_char' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_setopt(option text, parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_long' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_perform() RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_perform' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION pg_curl_easy_getinfo_char(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_char' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION pg_curl_easy_getinfo_long(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_long' LANGUAGE 'c';

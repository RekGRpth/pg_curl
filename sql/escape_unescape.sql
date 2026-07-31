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
-- exact RFC 3986 percent-encoding (space -> %20, not +)
select curl_easy_escape('a b/c?d=e&f%g');
select curl_easy_unescape('a%20b%2Fc%3Fd%3De%26f%25g');
-- unreserved characters (ALPHA / DIGIT / "-" / "." / "_" / "~") are left untouched
select curl_easy_escape('abcXYZ019-._~') = 'abcXYZ019-._~' as unreserved_untouched;
-- round-trip, including multi-byte UTF-8 input
select curl_easy_unescape(curl_easy_escape('a b/c?d=e&f%g россия')) = 'a b/c?d=e&f%g россия' as round_trip_ok;
END;

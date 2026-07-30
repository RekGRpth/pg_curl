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
-- curl_easy_escape() results returned inside curl_url_append()/curl_postfield_append()
-- must be released with curl_free(). If they leak, 50000 calls grow backend memory
-- usage by several MB; well-behaved code stays far below the threshold below.
DO $do$
DECLARE
    before_bytes bigint;
    after_bytes  bigint;
    delta        bigint;
BEGIN
    SELECT coalesce(sum(used_bytes), 0) INTO before_bytes FROM pg_backend_memory_contexts;
    FOR i IN 1..50000 LOOP
        PERFORM curl_easy_setopt_url('');
        PERFORM curl_url_append('a', 'b');
    END LOOP;
    SELECT coalesce(sum(used_bytes), 0) INTO after_bytes FROM pg_backend_memory_contexts;
    delta := after_bytes - before_bytes;
    IF delta > 1048576 THEN
        RAISE EXCEPTION 'curl_url_append leaks memory: % bytes after 50000 calls', delta;
    END IF;
END;
$do$;
END;

BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
BEGIN;
select curl_easy_reset();
-- curl_easy_escape() results returned inside curl_url_append()/curl_postfield_append()
-- must be released with curl_free(). If they leak, 50000 calls grow backend memory
-- usage by several MB; well-behaved code stays far below the threshold below.
-- pg_backend_memory_contexts only exists since PostgreSQL 14; on older server
-- versions the memory check is skipped since there is no portable equivalent.
DO $do$
DECLARE
    has_view     boolean := to_regclass('pg_catalog.pg_backend_memory_contexts') IS NOT NULL;
    before_bytes bigint;
    after_bytes  bigint;
    delta        bigint;
BEGIN
    IF has_view THEN
        EXECUTE 'SELECT coalesce(sum(used_bytes), 0) FROM pg_backend_memory_contexts' INTO before_bytes;
    END IF;
    FOR i IN 1..50000 LOOP
        PERFORM curl_easy_setopt_url('');
        PERFORM curl_url_append('a', 'b');
    END LOOP;
    IF has_view THEN
        EXECUTE 'SELECT coalesce(sum(used_bytes), 0) FROM pg_backend_memory_contexts' INTO after_bytes;
        delta := after_bytes - before_bytes;
        IF delta > 1048576 THEN
            RAISE EXCEPTION 'curl_url_append leaks memory: % bytes after 50000 calls', delta;
        END IF;
    END IF;
END;
$do$;
END;

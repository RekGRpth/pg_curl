\unset ECHO
\set QUIET 1
\pset format unaligned
\pset tuples_only true
\pset pager off
BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
-- Unlike the other test files, this one does not probe for a local httpbin
-- fallback: pg_whitelist_check_url() runs entirely before any connection is
-- attempted (see pg_curl_easy_prepare() in pg_curl.c), so what is being
-- tested here -- whether a request is granted or denied -- never actually
-- depends on a real round trip completing. "Denied" cases use
-- 192.0.2.1 (the RFC 5737 documentation range: reserved and guaranteed
-- non-routable) purely so that, if the ordering ever regressed and a
-- connection were attempted anyway, the failure would be obvious (a slow
-- connect timeout) rather than silently looking like a pass. "Granted"
-- cases use https://example.com/, which is fast and reliable, but the
-- assertion around it never inspects whether the request itself
-- succeeded -- only that it was not rejected by the whitelist -- so a
-- transient failure of that host would not make this test flaky.
SELECT current_user AS pg_curl_test_orig_user \gset

-- pg_curl has no predefined-role gate of its own, so pg_curl_privileged()
-- (see pg_curl.c) treats a caller holding both pg_read_server_files and
-- pg_execute_server_program as "privileged", the same pair pg_htmldoc
-- checks -- has_privs_of_role() gives a superuser every role's privileges
-- automatically, so a superuser is privileged too without needing an
-- explicit grant. This session (pg_curl_test_orig_user) stays fully
-- unrestricted while pg_curl.whitelist is unset. PERFORM inside a plain DO
-- block (no EXCEPTION clause) is used to assert "not denied" without
-- depending on whether the request itself actually succeeds --
-- curl_easy_perform() never raises on its own for a connection failure, so
-- the DO block only fails here if pg_whitelist_check_url() itself raises.
DO $$
BEGIN
    PERFORM curl_easy_reset();
    PERFORM curl_easy_setopt_url('https://example.com/');
    PERFORM curl_easy_perform();
END
$$;

-- Once pg_curl.whitelist is non-empty, though, it narrows even a
-- privileged (superuser) caller down to the listed prefixes -- it is not
-- merely an alternative grant for the unprivileged case tested further
-- down. curl_easy_reset()/setopt_url()/perform() are wrapped in one
-- explicit transaction: pg_curl's connection state lives in
-- CurTransactionContext (see pg_curl.transaction), which is torn down
-- and recreated between separate autocommit statements when run through
-- psql -f (as pg_regress does) -- unlike -c with several statements in
-- one string, each top-level statement here would otherwise run in its
-- own implicit transaction and lose the URL set two statements earlier.
BEGIN;
SELECT set_config('pg_curl.whitelist', 'https://example.com/', false);
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('http://192.0.2.1/forbidden');
SELECT curl_easy_perform();
COMMIT;
SELECT set_config('pg_curl.whitelist', '', false);

-- Role names can't start with "pg_" (reserved). Both need LOGIN: the
-- pg_curl.whitelist tests below need to \c into them directly, since
-- ALTER ROLE ... SET only takes effect for a new connection as that role,
-- not retroactively via SET ROLE in an already-open session.
CREATE ROLE curl_test_none LOGIN;
CREATE ROLE curl_test_full LOGIN;

-- pg_read_server_files/pg_execute_server_program don't exist before PG 11
-- (see the PGCURL_ROLE_* guard in pg_curl.c, mirroring pg_htmldoc's own);
-- make curl_test_full a superuser instead on those older servers so it
-- still ends up satisfying whichever check pg_curl_privileged() actually
-- performs here.
DO $$
BEGIN
    IF current_setting('server_version_num')::int >= 110000 THEN
        EXECUTE 'GRANT pg_read_server_files TO curl_test_full';
        EXECUTE 'GRANT pg_execute_server_program TO curl_test_full';
    ELSE
        EXECUTE 'ALTER ROLE curl_test_full SUPERUSER';
    END IF;
END
$$;

-- curl_test_full holds the two predefined roles pg_curl_privileged()
-- checks, but is deliberately NOT a superuser -- this isolates the
-- role-membership path from the has_privs_of_role() superuser bypass
-- exercised by pg_curl_test_orig_user above, confirming privilege here
-- really does come from role membership rather than superuser status.
\c - curl_test_full
DO $$
BEGIN
    PERFORM curl_easy_reset();
    PERFORM curl_easy_setopt_url('https://example.com/');
    PERFORM curl_easy_perform();
END
$$;

-- ... and, like any privileged caller, still gets narrowed once
-- pg_curl.whitelist is non-empty.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_full SET pg_curl.whitelist = 'https://example.com/';
\c - curl_test_full
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('http://192.0.2.1/forbidden');
SELECT curl_easy_perform();
COMMIT;

\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_full RESET pg_curl.whitelist;
DROP ROLE curl_test_full;

-- Without any pg_curl.whitelist configured, a non-superuser role is
-- denied outright -- pg_curl.whitelist is that role's only possible
-- grant, not merely a narrowing of an already-authorized caller.
\c - curl_test_none
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('https://example.com/');
SELECT curl_easy_perform();
COMMIT;

-- An explicit pg_curl.whitelist entry grants curl_test_none access
-- despite it being non-superuser, but only for the listed prefix.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none SET pg_curl.whitelist = 'https://example.com/';
\c - curl_test_none
DO $$
BEGIN
    PERFORM curl_easy_reset();
    PERFORM curl_easy_setopt_url('https://example.com/');
    PERFORM curl_easy_perform();
END
$$;

BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('http://192.0.2.1/forbidden');
SELECT curl_easy_perform();
COMMIT;

-- pg_curl.whitelist is registered PGC_SUSET, so only a superuser can set
-- it -- confirm curl_test_none, connected directly (not merely SET
-- ROLE'd) and thus genuinely non-superuser, can't loosen its own scope
-- with a plain SET. pg_curl.so is already loaded in this backend by the
-- calls above, so the PGC_SUSET definition pg_whitelist_init() registers
-- is enforced here, not just an unreconciled placeholder.
DO $$
BEGIN
    BEGIN
        PERFORM set_config('pg_curl.whitelist', 'http://192.0.2.1/', false);
        RAISE EXCEPTION 'curl_test_none was able to loosen its own pg_curl.whitelist with a plain SET';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;
    END;
END
$$;

-- curl_mime_file() reads its content from an actual local file (unlike
-- curl_mime_data(), which takes the content in-memory) and never touches
-- the network by itself, so it goes through pg_whitelist_check_local()
-- the same way pg_htmldoc's read_fileurl() does for local files -- purely
-- a local realpath()-based decision, so these assertions are
-- deterministic regardless of network conditions.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none RESET pg_curl.whitelist;
COPY (SELECT 'pg_curl whitelist local file test content') TO '/tmp/pg_curl_whitelist_test.txt';

\c - curl_test_none
SELECT curl_easy_reset();
SELECT curl_mime_file('/tmp/pg_curl_whitelist_test.txt', name := 'upload');

-- A combined file:// + http(s):// list grants curl_test_none both the
-- specific local file and a request URL, independently of each other.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none SET pg_curl.whitelist = 'file:///tmp/pg_curl_whitelist_test.txt,https://example.com/';
\c - curl_test_none
SELECT curl_easy_reset();
SELECT curl_mime_file('/tmp/pg_curl_whitelist_test.txt', name := 'upload');
DO $$
BEGIN
    PERFORM curl_easy_setopt_url('https://example.com/');
    PERFORM curl_easy_perform();
END
$$;

-- The exact file:// entry does not extend to any other path, e.g. a
-- classic local-file-read attempt against /etc/passwd.
SELECT curl_easy_reset();
SELECT curl_mime_file('/etc/passwd', name := 'upload');

-- A file:// entry with a trailing slash permits anything under that
-- directory, but the resolved path is realpath()-canonicalized before
-- comparison, so ".." can't be used to climb back out of it even though
-- the raw string would otherwise start with the whitelisted prefix.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none SET pg_curl.whitelist = 'file:///tmp/';
\c - curl_test_none
SELECT curl_easy_reset();
SELECT curl_mime_file('/tmp/pg_curl_whitelist_test.txt', name := 'upload');
SELECT curl_mime_file('/tmp/../etc/passwd', name := 'upload2');

\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none RESET pg_curl.whitelist;
DROP ROLE curl_test_none;

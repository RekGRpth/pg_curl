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

-- pg_curl_privileged() (see pg_curl.c) is simply superuser(): a superuser
-- stays fully unrestricted while pg_curl.whitelist is unset, and every
-- other role's access is governed entirely by pg_curl.whitelist. This
-- session (pg_curl_test_orig_user) is a superuser. PERFORM inside a plain DO
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

-- Needs LOGIN: the pg_curl.whitelist tests below need to \c into it
-- directly, since ALTER ROLE ... SET only takes effect for a new
-- connection as that role, not retroactively via SET ROLE in an
-- already-open session.
CREATE ROLE curl_test_none LOGIN;

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

-- pg_whitelist_check_url() by itself only recognizes lowercase http(s)://
-- URLs and lets anything else through, so pg_curl classifies the request
-- URL the way libcurl will first: an uppercase scheme, a scheme-less URL,
-- a non-http(s) scheme or a file:// URL must not slip past the whitelist,
-- not even via curl_easy_setopt_default_protocol(). The network cases set
-- a 1 s connect timeout, so if one ever slipped through again it would fail
-- fast against 192.0.2.1 instead of hanging for curl's default 300 s.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none SET pg_curl.whitelist = 'https://example.com/,file:///tmp/pg_curl_whitelist_test.txt';
\c - curl_test_none
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_connecttimeout(1);
SELECT curl_easy_setopt_url('HTTP://192.0.2.1/forbidden');
SELECT curl_easy_perform();
COMMIT;
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_connecttimeout(1);
SELECT curl_easy_setopt_url('192.0.2.1/forbidden');
SELECT curl_easy_perform();
COMMIT;
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_connecttimeout(1);
SELECT curl_easy_setopt_url('ftp://192.0.2.1/forbidden');
SELECT curl_easy_perform();
COMMIT;
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('file:///etc/passwd');
SELECT curl_easy_perform();
COMMIT;
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_default_protocol('file');
SELECT curl_easy_setopt_url('/etc/passwd');
SELECT curl_easy_perform();
COMMIT;

-- An uppercase scheme is still matched against a lowercase entry, and a
-- whitelisted file:// URL is actually fetched.
DO $$
BEGIN
    PERFORM curl_easy_reset();
    PERFORM curl_easy_setopt_url('HTTPS://example.com/');
    PERFORM curl_easy_perform();
END
$$;
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('file:///tmp/pg_curl_whitelist_test.txt');
SELECT curl_easy_perform();
SELECT convert_from(curl_easy_getinfo_data_in(), 'utf-8');
COMMIT;

-- Every option that has curl open a local file or socket goes through the
-- same local check as curl_mime_file(): cookiejar (written) and the
-- certificate/key/known_hosts files (read) only for whitelisted paths, a
-- pinned public key only when it is a file rather than sha256// hashes, and
-- an abstract socket (no path on disk to whitelist) not at all.
SELECT curl_easy_setopt_cookiejar('/tmp/pg_curl_whitelist_test.txt');
SELECT curl_easy_setopt_cookiejar('/etc/passwd');
SELECT curl_easy_setopt_cainfo('/etc/passwd');
SELECT curl_easy_setopt_sslkey('/etc/passwd');
SELECT curl_easy_setopt_ssh_knownhosts('/etc/passwd');
SELECT curl_easy_setopt_unix_socket_path('/etc/passwd');
SELECT curl_easy_setopt_abstract_unix_socket('pg_curl_test');
SELECT curl_easy_setopt_pinnedpublickey('sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=');
SELECT curl_easy_setopt_pinnedpublickey('/etc/passwd');

-- pg_whitelist_check_local() skips anything that looks like an http(s) URL,
-- but for these it is always a path (relative to the data directory).
SELECT curl_easy_setopt_cookiejar('https://example.com/../../etc/passwd');
SELECT curl_mime_file('http://example.com/../../etc/passwd', name := 'upload');

-- Options the whitelist has no way to scope are unavailable altogether: an
-- OpenSSL engine (a shared library loaded into the backend) and .netrc (the
-- postgres OS user's credentials), except for leaving .netrc ignored.
SELECT curl_easy_setopt_sslengine('dynamic');
SELECT curl_easy_setopt_netrc(curl_netrc_optional());
SELECT curl_easy_setopt_netrc(curl_netrc_required());
SELECT curl_easy_setopt_netrc(curl_netrc_ignored());

-- Only the request URL itself is checked, so following redirects -- which
-- could lead to any host -- is unavailable too; turning it off is fine.
SELECT curl_easy_setopt_followlocation(1);
SELECT curl_easy_setopt_followlocation(0);

-- The same goes for hosts curl connects to besides the request URL.
SELECT curl_easy_setopt_proxy('http://192.0.2.1:3128');
SELECT curl_easy_setopt_pre_proxy('socks5://192.0.2.1:1080');
SELECT curl_easy_setopt_doh_url('https://192.0.2.1/dns-query');

-- An entry without a trailing slash matches only up to a URL delimiter, so
-- it can't be stretched to another host via userinfo or a longer name.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none SET pg_curl.whitelist = 'https://example.com';
\c - curl_test_none
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('https://example.com@192.0.2.1/');
SELECT curl_easy_perform();
COMMIT;
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_url('https://example.com.invalid/');
SELECT curl_easy_perform();
COMMIT;

\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_none RESET pg_curl.whitelist;
DROP ROLE curl_test_none;

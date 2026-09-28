BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
DO $plpgsql$ BEGIN
    BEGIN
        PERFORM curl_easy_reset();
        PERFORM curl_easy_setopt_timeout(1);
        PERFORM curl_easy_setopt_url('http://localhost/status/202');
        PERFORM curl_easy_perform();
        PERFORM curl_easy_getinfo_http_connectcode();
        SET pg_curl.httpbin = 'http://localhost';
    EXCEPTION WHEN OTHERS THEN
        SET pg_curl.httpbin = 'https://httpbin.org';
    END;
END;$plpgsql$;
-- A role pg_curl.whitelist applies to may follow redirects, but every
-- redirect target is checked against the whitelist just like the request
-- URL: before libcurl opens a connection to it, and before each request,
-- including one on a connection reused from the previous hop. Which httpbin
-- is in use varies, so it is masked out of the output below.
SELECT current_user AS pg_curl_test_orig_user, current_setting('pg_curl.httpbin') AS httpbin, current_setting('pg_curl.httpbin') || '/' AS whitelist_all, current_setting('pg_curl.httpbin') || '/redirect-to' AS whitelist_redirect \gset
CREATE ROLE curl_test_redirect LOGIN;
ALTER ROLE curl_test_redirect SET pg_curl.httpbin = :'httpbin';
ALTER ROLE curl_test_redirect SET pg_curl.whitelist = :'whitelist_all';
\c - curl_test_redirect

-- A redirect to a whitelisted URL is followed.
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_followlocation(1);
SELECT curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/redirect-to?url=%2Fget');
SELECT curl_easy_perform();
SELECT curl_easy_getinfo_response_code();
SELECT curl_easy_getinfo_redirect_count();
SELECT replace(curl_easy_getinfo_effective_url(), current_setting('pg_curl.httpbin'), '<httpbin>') AS effective_url;
COMMIT;

-- A redirect to a host the whitelist doesn't list is refused before that
-- host is contacted: were a connection attempted, 192.0.2.1 (RFC 5737,
-- non-routable) would fail with a connect timeout instead.
BEGIN;
SELECT curl_easy_reset();
SELECT curl_easy_setopt_connecttimeout(1);
SELECT curl_easy_setopt_followlocation(1);
SELECT curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/redirect-to?url=http%3A%2F%2F192.0.2.1%2F');
SELECT curl_easy_perform();
COMMIT;

-- A redirect to another path on the same host goes over the connection
-- already open to it, so only the check before the request can refuse it.
\c - :pg_curl_test_orig_user
ALTER ROLE curl_test_redirect SET pg_curl.whitelist = :'whitelist_redirect';
\c - curl_test_redirect
DO $$
BEGIN
    PERFORM curl_easy_reset();
    PERFORM curl_easy_setopt_followlocation(1);
    PERFORM curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/redirect-to?url=%2Fget');
    PERFORM curl_easy_perform();
    RAISE EXCEPTION 'redirect to /get was followed';
EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE '%', replace(SQLERRM, current_setting('pg_curl.httpbin'), '<httpbin>');
END
$$;

\c - :pg_curl_test_orig_user
DROP ROLE curl_test_redirect;

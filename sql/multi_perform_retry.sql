BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
-- nothing listens on port 1, so every attempt is refused immediately. Each
-- failed attempt that is going to be retried raises a WARNING, but its text
-- comes from libcurl and differs between versions ("Couldn't connect to
-- server" vs "Could not connect to server"), so the WARNINGs are hidden and
-- the retries are counted by time instead: try := 3 means two sleeps
-- between attempts, so at least 2 * 0.2 s must pass. The call must still
-- report the failure.
BEGIN;
SET LOCAL client_min_messages = ERROR;
select curl_easy_reset();
select curl_easy_setopt_url('http://127.0.0.1:1/');
select clock_timestamp() as started \gset
select curl_easy_perform(try := 3, sleep := 200000) as should_be_false;
select clock_timestamp() - :'started'::timestamptz >= interval '0.4 seconds' as retried_twice;
select curl_easy_getinfo_errcode();
END;

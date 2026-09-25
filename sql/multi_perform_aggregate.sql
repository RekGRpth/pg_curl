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
-- one handle fails immediately (connection refused), the other succeeds
-- but takes longer to complete: the failure's CURLMSG_DONE is read out
-- in an earlier iteration than the later success, so a return value that
-- only reflects the last-processed handle would mask the failure
BEGIN;
select curl_easy_reset(conname:='fail');
select curl_easy_setopt_timeout(2, conname:='fail');
select curl_easy_setopt_url('http://127.0.0.1:1/', conname:='fail');
select curl_multi_add_handle(conname:='fail');
select curl_easy_reset(conname:='slow_ok');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/delay/1', conname:='slow_ok');
select curl_multi_add_handle(conname:='slow_ok');
select curl_multi_perform(1, 100000, 2000) as should_be_false;
select curl_easy_getinfo_errcode(conname:='fail') <> 0 as fail_did_fail;
select curl_easy_getinfo_errcode(conname:='slow_ok') as slow_ok_errcode;
END;

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
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/put');
select curl_easy_setopt_readdata(convert_to('first-body', 'utf-8'));
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb ->> 'data') = 'first-body' as first_ok;
-- reuse the same connection without re-setting readdata: the upload body
-- must still be sent correctly on the second perform, not empty/EOF-failed
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb ->> 'data') = 'first-body' as second_ok;
END;

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
-- libcurl keeps options across transfers, so once a body is emptied the
-- next perform on the same handle must not keep sending the previous one:
-- a stale POSTFIELDS used to resend the old buffer with its old size (first
-- byte zeroed), a stale UPLOAD used to fail with CURLE_READ_ERROR.
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/anything');
select curl_easy_setopt_postfields(convert_to('secret-body', 'utf-8'));
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb ->> 'method') = 'POST' as first_post;
select curl_easy_setopt_postfields(convert_to('', 'utf-8'));
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb ->> 'method') = 'GET' as emptied_postfields_get;
select convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb -> 'form' = '{}' as no_stale_body;
END;
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/anything');
select curl_easy_setopt_readdata(convert_to('put-body', 'utf-8'));
select curl_easy_perform();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb ->> 'method') = 'PUT' as first_put;
select curl_easy_setopt_readdata(convert_to('', 'utf-8'));
select curl_easy_perform();
select curl_easy_getinfo_errcode();
select (convert_from(curl_easy_getinfo_data_in(), 'utf-8')::jsonb ->> 'method') = 'GET' as emptied_readdata_get;
END;

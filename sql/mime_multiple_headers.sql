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
-- Null and empty arrays mean no custom headers.
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', head := null::text[]);
select curl_mime_data('x', head := '{}'::text[]);
END;
-- Every array element is checked before any header list is built.
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', head := array['X-Part: 1', null]);
END;
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', head := array['X-Part: 1', E'X-Injected: pwned\r\nX-Another: pwned']);
END;
BEGIN;
select curl_easy_reset();
select curl_mime_data(convert_to('x', 'utf-8'), head := array[E'X-Injected: pwned\nX-Another: pwned']);
END;
BEGIN;
select curl_easy_reset();
select curl_mime_file('/dev/null', head := array[E'X-Injected: pwned\rX-Another: pwned']);
END;
-- Verify the actual outgoing MIME body for all three functions.
COPY (select 'pg_curl multiple mime headers file test content') TO '/tmp/pg_curl_mime_multiple_headers_test.txt';
BEGIN;
select curl_easy_reset();
select curl_easy_setopt_verbose(1);
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?');
select curl_mime_data('text content', name := 'text_part', head := array['Content-ID: <text>', 'X-Text-Part: one', 'X-Text-Part: two']);
select curl_mime_data(convert_to('bytea content', 'utf-8'), name := 'bytea_part', head := array['Content-ID: <bytea>', 'X-Bytea-Part: present']);
select curl_mime_file('/tmp/pg_curl_mime_multiple_headers_test.txt', name := 'file_part', head := array['Content-ID: <file>', 'X-File-Part: present']);
select curl_easy_perform();
WITH outgoing AS (
    select convert_from(curl_easy_getinfo_data_out(), 'utf-8') as body
) select
    strpos(body, 'Content-ID: <text>') > 0 and strpos(body, 'X-Text-Part: one') > 0 and strpos(body, 'X-Text-Part: two') > 0 as text_headers,
    strpos(body, 'Content-ID: <text>') < strpos(body, 'X-Text-Part: one') and strpos(body, 'X-Text-Part: one') < strpos(body, 'X-Text-Part: two') as text_headers_in_order,
    strpos(body, 'Content-ID: <bytea>') > 0 and strpos(body, 'X-Bytea-Part: present') > 0 as bytea_headers,
    strpos(body, 'Content-ID: <file>') > 0 and strpos(body, 'X-File-Part: present') > 0 as file_headers
FROM outgoing;
END;

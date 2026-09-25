BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
BEGIN;
select curl_easy_reset();
-- no name/file/type/code/head supplied: the mime part is still added
-- successfully and must be reported as such
select curl_mime_data('content');
select curl_mime_data(convert_to('content', 'utf-8'));
select curl_mime_file('/dev/null');
END;

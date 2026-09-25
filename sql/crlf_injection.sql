BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
-- embedded CR/LF must be rejected before it ever reaches the wire as a
-- second, attacker-controlled protocol line (HTTP header/FTP command/
-- SMTP recipient injection)
BEGIN;
select curl_easy_reset();
select curl_header_append('X-Test', E'value1\r\nX-Injected: pwned');
END;
BEGIN;
select curl_easy_reset();
select curl_header_append(E'X-Test\r\nX-Injected: pwned', 'value1');
END;
BEGIN;
select curl_easy_reset();
select curl_quote_append(E'PWD\r\nDELE important.txt');
END;
BEGIN;
select curl_easy_reset();
select curl_prequote_append(E'PWD\r\nDELE important.txt');
END;
BEGIN;
select curl_easy_reset();
select curl_postquote_append(E'PWD\r\nDELE important.txt');
END;
BEGIN;
select curl_easy_reset();
select curl_recipient_append(E'a@example.com\r\nRCPT TO:<b@example.com>');
END;
-- mime part name/file/type/head end up in the part's header lines
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', head := E'X-Part: 1\r\nX-Injected: pwned');
END;
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', type := E'text/plain\r\nX-Injected: pwned');
END;
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', name := E'field\r\nX-Injected: pwned');
END;
BEGIN;
select curl_easy_reset();
select curl_mime_data('x', file := E'a.txt\r\nX-Injected: pwned');
END;
-- sanity: legitimate values without CR/LF must keep working
BEGIN;
select curl_easy_reset();
select curl_header_append('X-Test', 'value1');
select curl_quote_append('PWD');
select curl_prequote_append('PWD');
select curl_postquote_append('PWD');
select curl_recipient_append('a@example.com');
select curl_mime_data('x', name := 'field', file := 'a.txt', type := 'text/plain', head := 'X-Part: 1');
END;

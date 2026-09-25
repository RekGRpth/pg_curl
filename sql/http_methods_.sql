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
select curl_easy_reset(conname:='1');
select curl_easy_reset(conname:='2');
select curl_easy_reset(conname:='3');
select curl_easy_reset(conname:='4');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/get?', conname:='1');
select curl_url_append('a', 'b', conname:='1');
select curl_url_append('c', '', conname:='1');
select curl_url_append('d', conname:='1');
select curl_easy_setopt_postfields(convert_to('{"e":"f","g":"","h":null}', 'utf-8'), conname:='2');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?', conname:='2');
select curl_url_append('a', 'b', conname:='2');
select curl_url_append('c', '', conname:='2');
select curl_url_append('d', conname:='2');
select curl_header_append('Content-Type', 'application/json; charset=utf-8', conname:='2');
select curl_postfield_append('e', 'f', conname:='3');
select curl_postfield_append('g', '', conname:='3');
select curl_postfield_append('h', conname:='3');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?', conname:='3');
select curl_url_append('a', 'b', conname:='3');
select curl_url_append('c', '', conname:='3');
select curl_url_append('d', conname:='3');
select curl_easy_setopt_url(current_setting('pg_curl.httpbin') || '/post?', conname:='4');
select curl_url_append('a', 'b', conname:='4');
select curl_url_append('c', '', conname:='4');
select curl_url_append('d', conname:='4');
select curl_mime_data('f', name:='e', conname:='4');
select curl_mime_data('', name:='g', conname:='4');
select curl_mime_data(null, name:='h', conname:='4');
select curl_mime_data('content', name:='filename.txt', file:='filename.txt', conname:='4');
select curl_multi_add_handle(conname:='1');
select curl_multi_add_handle(conname:='2');
select curl_multi_add_handle(conname:='3');
select curl_multi_add_handle(conname:='4');
select curl_multi_perform();
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='1'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='2'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='3'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
with s as (
    select regexp_matches(curl_easy_getinfo_header_in(conname:='4'), E'([^ \t\r\n\f]+): ?([^\t\r\n\f]+)', 'g') as s
) select lower(s[1]) as key, s[2] as value from s where lower(s[1]) not in ('date', 'server', 'content-length', 'connection');
-- args/headers are selected explicitly (rather than deleting the noisy keys
-- with jsonb's -/#- operators, which only exist since PostgreSQL 9.5) so this
-- keeps working back to PostgreSQL 9.4, which has -> and json_build_object
with j as (
    select convert_from(curl_easy_getinfo_data_in(conname:='1'), 'utf-8')::jsonb as data
) select json_build_object(
    'args', j.data->'args',
    'headers', json_build_object('Accept', j.data->'headers'->>'Accept')
) from j;
with j as (
    select convert_from(curl_easy_getinfo_data_in(conname:='2'), 'utf-8')::jsonb as data
) select json_build_object(
    'args', j.data->'args',
    'data', j.data->>'data',
    'form', j.data->'form',
    'json', j.data->'json',
    'files', j.data->'files',
    'headers', json_build_object(
        'Accept', j.data->'headers'->>'Accept',
        'Content-Type', j.data->'headers'->>'Content-Type',
        'Content-Length', j.data->'headers'->>'Content-Length'
    )
) from j;
with j as (
    select convert_from(curl_easy_getinfo_data_in(conname:='3'), 'utf-8')::jsonb as data
) select json_build_object(
    'args', j.data->'args',
    'data', j.data->>'data',
    'form', j.data->'form',
    'json', j.data->'json',
    'files', j.data->'files',
    'headers', json_build_object(
        'Accept', j.data->'headers'->>'Accept',
        'Content-Type', j.data->'headers'->>'Content-Type',
        'Content-Length', j.data->'headers'->>'Content-Length'
    )
) from j;
with j as (
    select convert_from(curl_easy_getinfo_data_in(conname:='4'), 'utf-8')::jsonb as data
) select json_build_object(
    'args', j.data->'args',
    'data', j.data->>'data',
    'form', j.data->'form',
    'json', j.data->'json',
    'files', j.data->'files',
    'headers', json_build_object('Accept', j.data->'headers'->>'Accept')
) from j;
select curl_easy_getinfo_errcode(conname:='1'), curl_easy_getinfo_errdesc(conname:='1'), curl_easy_getinfo_errbuf(conname:='1');
select curl_easy_getinfo_errcode(conname:='2'), curl_easy_getinfo_errdesc(conname:='2'), curl_easy_getinfo_errbuf(conname:='2');
select curl_easy_getinfo_errcode(conname:='3'), curl_easy_getinfo_errdesc(conname:='3'), curl_easy_getinfo_errbuf(conname:='3');
select curl_easy_getinfo_errcode(conname:='4'), curl_easy_getinfo_errdesc(conname:='4'), curl_easy_getinfo_errbuf(conname:='4');
END;

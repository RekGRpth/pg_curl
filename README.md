# get
```sql
CREATE OR REPLACE FUNCTION get(url TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_url(url),
        curl_easy_perform(),
        curl_easy_getinfo_response()
    ) SELECT convert_from(curl_easy_getinfo_response, 'utf-8') FROM s;
$BODY$;
```

# urlencoded post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_url(url),
        curl_easy_setopt_copypostfields((
            WITH s AS (
                SELECT (json_each_text(request)).*
            ) SELECT convert_to(array_to_string(array_agg(concat_ws('=',
                curl_easy_escape(key),
                curl_easy_escape(value)
            )), '&'), 'utf-8') FROM s
        )),
        curl_easy_perform(),
        curl_easy_getinfo_response()
    ) SELECT convert_from(curl_easy_getinfo_response, 'utf-8') FROM s;
$BODY$;
```

# json post
```sql
CREATE OR REPLACE FUNCTION post(url TEXT, request JSON) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_url(url),
        curl_header_append('Content-Type', 'application/json; charset=utf-8'),
        curl_easy_setopt_copypostfields(convert_to(request::TEXT, 'utf-8')),
        curl_easy_perform(),
        curl_easy_getinfo_response()
    ) SELECT convert_from(curl_easy_getinfo_response, 'utf-8') FROM s;
$BODY$;
```

# send email
```sql
CREATE OR REPLACE FUNCTION email(url TEXT, username TEXT, password TEXT, subject TEXT, "from" TEXT, "to" TEXT[], data TEXT, type TEXT) RETURNS TEXT LANGUAGE SQL AS $BODY$
    WITH s AS (SELECT
        curl_easy_reset(),
        curl_easy_setopt_url(url),
        curl_easy_setopt_username(username),
        curl_easy_setopt_password(password),
        curl_recipient_append("to"),
        curl_header_append('Subject', subject),
        curl_header_append('From', "from"),
        curl_header_append('To', "to"),
        curl_mime_data(data, type:=type),
        curl_easy_perform(),
        curl_easy_getinfo_headers()
    ) SELECT curl_easy_getinfo_headers FROM s;
$BODY$;
```

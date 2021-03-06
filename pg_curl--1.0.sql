-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_curl" to load this file. \quit

CREATE OR REPLACE FUNCTION curl_easy_header_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_header_reset' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_mime_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_mime_reset' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_readdata_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_readdata_reset' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_recipient_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_recipient_reset' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_reset() RETURNS void AS 'MODULE_PATHNAME', 'pg_curl_easy_reset' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_escape(string text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_escape' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_unescape(url text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_unescape' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_header_append(name text, value text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_header_append' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_recipient_append(email text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_recipient_append' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_mime_data(data bytea, name text default null, file text default null, type text default null, code text default null, head text default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_data_bytea' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_mime_data(data text, name text default null, file text default null, type text default null, code text default null, head text default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_data' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_mime_file(data text, name text default null, file text default null, type text default null, code text default null, head text default null) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_mime_file' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_setopt_copypostfields(parameter bytea) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_copypostfields' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_readdata(parameter bytea) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_readdata' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_setopt_abstract_unix_socket(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_abstract_unix_socket' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_accept_encoding(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_accept_encoding' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_cainfo(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_cainfo' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_capath(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_capath' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_cookiefile(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_cookiefile' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_cookiejar(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_cookiejar' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_cookielist(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_cookielist' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_cookie(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_cookie' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_crlfile(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_crlfile' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_customrequest(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_customrequest' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_default_protocol(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_default_protocol' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_interface(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_interface' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_local_ip4(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_local_ip4' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_local_ip6(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_local_ip6' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_servers(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_servers' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_doh_url(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_doh_url' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_egdsocket(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_egdsocket' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_account(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_account' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_alternative_to_user(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_alternative_to_user' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftpport(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftpport' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_interface(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_interface' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_issuercert(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_issuercert' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_keypasswd(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_keypasswd' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_krblevel(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_krblevel' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_login_options(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_login_options' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_mail_auth(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_mail_auth' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_mail_from(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_mail_from' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_noproxy(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_noproxy' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_password(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_password' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_pinnedpublickey(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_pinnedpublickey' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_pre_proxy(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_pre_proxy' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_cainfo(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_cainfo' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_capath(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_capath' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_crlfile(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_crlfile' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_keypasswd(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_keypasswd' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxypassword(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxypassword' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_pinnedpublickey(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_pinnedpublickey' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_service_name(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_service_name' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_sslcert(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_sslcert' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_sslcerttype(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_sslcerttype' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_ssl_cipher_list(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_ssl_cipher_list' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_sslkey(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_sslkey' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_sslkeytype(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_sslkeytype' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_tls13_ciphers(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_tls13_ciphers' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_tlsauth_password(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_tlsauth_password' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_tlsauth_type(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_tlsauth_type' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_tlsauth_username(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_tlsauth_username' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxyusername(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxyusername' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxyuserpwd(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxyuserpwd' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_random_file(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_random_file' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_range(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_range' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_referer(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_referer' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_request_target(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_request_target' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_rtsp_session_id(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_rtsp_session_id' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_rtsp_stream_uri(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_rtsp_stream_uri' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_rtsp_transport(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_rtsp_transport' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_service_name(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_service_name' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_socks5_gssapi_service(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_socks5_gssapi_service' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssh_host_public_key_md5(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssh_host_public_key_md5' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssh_knownhosts(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssh_knownhosts' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssh_private_keyfile(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssh_private_keyfile' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssh_public_keyfile(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssh_public_keyfile' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sslcert(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sslcert' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sslcerttype(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sslcerttype' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_cipher_list(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_cipher_list' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sslengine(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sslengine' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sslkey(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sslkey' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sslkeytype(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sslkeytype' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tls13_ciphers(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tls13_ciphers' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tlsauth_password(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tlsauth_password' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tlsauth_type(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tlsauth_type' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tlsauth_username(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tlsauth_username' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_unix_socket_path(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_unix_socket_path' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_url(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_url' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_useragent(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_useragent' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_username(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_username' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_userpwd(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_userpwd' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_xoauth2_bearer(parameter text) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_xoauth2_bearer' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_setopt_accepttimeout_ms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_accepttimeout_ms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_address_scope(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_address_scope' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_append(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_append' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_autoreferer(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_autoreferer' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_buffersize(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_buffersize' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_certinfo(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_certinfo' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_connect_only(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_connect_only' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_connecttimeout_ms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_connecttimeout_ms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_connecttimeout(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_connecttimeout' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_cookiesession(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_cookiesession' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_crlf(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_crlf' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dirlistonly(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dirlistonly' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_disallow_username_in_url(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_disallow_username_in_url' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_cache_timeout(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_cache_timeout' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_shuffle_addresses(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_shuffle_addresses' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_dns_use_global_cache(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_dns_use_global_cache' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_expect_100_timeout_ms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_expect_100_timeout_ms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_failonerror(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_failonerror' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_filetime(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_filetime' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_followlocation(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_followlocation' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_forbid_reuse(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_forbid_reuse' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_fresh_connect(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_fresh_connect' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_create_missing_dirs(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_create_missing_dirs' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_filemethod(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_filemethod' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_skip_pasv_ip(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_skip_pasv_ip' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftpsslauth(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftpsslauth' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_ssl_ccc(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_ssl_ccc' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_use_eprt(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_use_eprt' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_use_epsv(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_use_epsv' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ftp_use_pret(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ftp_use_pret' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_gssapi_delegation(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_gssapi_delegation' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_happy_eyeballs_timeout_ms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_happy_eyeballs_timeout_ms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_haproxyprotocol(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_haproxyprotocol' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_header(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_header' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_http09_allowed(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_http09_allowed' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_httpauth(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_httpauth' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_http_content_decoding(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_http_content_decoding' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_httpget(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_httpget' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_httpproxytunnel(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_httpproxytunnel' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_http_transfer_decoding(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_http_transfer_decoding' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_http_version(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_http_version' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ignore_content_length(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ignore_content_length' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ipresolve(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ipresolve' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_keep_sending_on_error(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_keep_sending_on_error' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_localportrange(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_localportrange' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_localport(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_localport' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_low_speed_limit(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_low_speed_limit' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_low_speed_time(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_low_speed_time' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_maxconnects(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_maxconnects' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_maxfilesize(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_maxfilesize' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_maxredirs(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_maxredirs' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_netrc(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_netrc' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_new_directory_perms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_new_directory_perms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_new_file_perms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_new_file_perms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_nobody(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_nobody' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_nosignal(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_nosignal' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_path_as_is(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_path_as_is' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_pipewait(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_pipewait' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_port(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_port' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_postredir(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_postredir' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_post(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_post' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_protocols(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_protocols' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxyauth(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxyauth' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxyport(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxyport' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_ssl_options(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_ssl_options' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_ssl_verifyhost(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_ssl_verifyhost' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_ssl_verifypeer(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_ssl_verifypeer' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_sslversion(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_sslversion' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxy_transfer_mode(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxy_transfer_mode' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_proxytype(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_proxytype' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_put(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_put' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_redir_protocols(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_redir_protocols' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_resume_from(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_resume_from' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_rtsp_client_cseq(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_rtsp_client_cseq' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_rtsp_request(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_rtsp_request' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_rtsp_server_cseq(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_rtsp_server_cseq' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sasl_ir(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sasl_ir' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_server_response_timeout(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_server_response_timeout' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_socks5_auth(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_socks5_auth' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_socks5_gssapi_nec(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_socks5_gssapi_nec' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssh_auth_types(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssh_auth_types' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssh_compression(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssh_compression' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_enable_alpn(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_enable_alpn' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_enable_npn(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_enable_npn' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_falsestart(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_falsestart' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_options(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_options' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_sessionid_cache(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_sessionid_cache' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_verifyhost(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_verifyhost' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_verifypeer(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_verifypeer' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_ssl_verifystatus(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_ssl_verifystatus' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_sslversion(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_sslversion' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_stream_weight(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_stream_weight' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_suppress_connect_headers(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_suppress_connect_headers' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tcp_fastopen(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tcp_fastopen' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tcp_keepalive(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tcp_keepalive' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tcp_keepidle(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tcp_keepidle' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tcp_keepintvl(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tcp_keepintvl' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tcp_nodelay(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tcp_nodelay' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tftp_blksize(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tftp_blksize' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_tftp_no_options(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_tftp_no_options' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_timecondition(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_timecondition' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_timeout_ms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_timeout_ms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_timeout(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_timeout' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_timevalue(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_timevalue' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_transfer_encoding(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_transfer_encoding' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_transfertext(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_transfertext' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_unrestricted_auth(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_unrestricted_auth' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_upkeep_interval_ms(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_upkeep_interval_ms' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_upload_buffersize(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_upload_buffersize' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_use_ssl(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_use_ssl' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_verbose(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_verbose' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_setopt_wildcardmatch(parameter bigint) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_setopt_wildcardmatch' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_perform(try int default 1, sleep bigint default 1000000) RETURNS boolean AS 'MODULE_PATHNAME', 'pg_curl_easy_perform' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_getinfo_headers() RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_headers' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_response() RETURNS bytea AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_response' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_getinfo_content_type(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_content_type' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_effective_url(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_effective_url' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_ftp_entry_path(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_ftp_entry_path' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_local_ip(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_local_ip' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_primary_ip(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_primary_ip' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_private(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_private' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_redirect_url(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_redirect_url' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_rtsp_session_id(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_rtsp_session_id' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_scheme(info text) RETURNS text AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_scheme' LANGUAGE 'c';

CREATE OR REPLACE FUNCTION curl_easy_getinfo_condition_unmet(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_condition_unmet' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_filetime(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_filetime' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_header_size(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_header_size' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_httpauth_avail(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_httpauth_avail' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_http_connectcode(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_http_connectcode' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_http_version(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_http_version' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_lastsocket(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_lastsocket' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_local_port(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_local_port' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_num_connects(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_num_connects' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_os_errno(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_os_errno' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_primary_port(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_primary_port' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_protocol(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_protocol' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_proxyauth_avail(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_proxyauth_avail' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_proxy_ssl_verifyresult(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_proxy_ssl_verifyresult' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_redirect_count(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_redirect_count' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_request_size(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_request_size' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_response_code(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_response_code' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_rtsp_client_cseq(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_rtsp_client_cseq' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_rtsp_cseq_recv(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_rtsp_cseq_recv' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_rtsp_server_cseq(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_rtsp_server_cseq' LANGUAGE 'c';
CREATE OR REPLACE FUNCTION curl_easy_getinfo_ssl_verifyresult(info text) RETURNS bigint AS 'MODULE_PATHNAME', 'pg_curl_easy_getinfo_ssl_verifyresult' LANGUAGE 'c';

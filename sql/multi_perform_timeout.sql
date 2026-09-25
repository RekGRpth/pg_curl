BEGIN;
SET LOCAL client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_curl;
END;
BEGIN;
-- try (arg 1) is valid, timeout_ms (arg 3) is invalid: must be rejected because
-- of timeout_ms, not silently accepted by misreading the try argument instead
select curl_multi_perform(5, 0, -5);
END;

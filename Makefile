MODULES = pg_curl
EXTENSION = pg_curl
CURL_CONFIG = curl-config
PG_CONFIG = pg_config

CFLAGS += $(shell $(CURL_CONFIG) --cflags)
LIBS += $(shell $(CURL_CONFIG) --libs)
SHLIB_LINK := $(LIBS)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

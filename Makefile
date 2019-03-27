EXTENSION = pg_curl
MODULE_big = $(EXTENSION)

CURL_CONFIG = curl-config
PG_CONFIG = pg_config
OBJS = $(EXTENSION).o
DATA = pg_curl--1.0.sql

CFLAGS += $(shell $(CURL_CONFIG) --cflags)
LIBS += $(shell $(CURL_CONFIG) --libs)
SHLIB_LINK := $(LIBS)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

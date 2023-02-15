$(OBJS): Makefile
DATA = pg_curl--1.0.sql
EXTENSION = pg_curl
MODULE_big = $(EXTENSION)
OBJS = $(EXTENSION).o
PG_CONFIG = pg_config
PGXS = $(shell $(PG_CONFIG) --pgxs)
REGRESS = $(patsubst sql/%.sql,%,$(TESTS))
SHLIB_LINK = -lcurl
TESTS = $(wildcard sql/*.sql)
include $(PGXS)

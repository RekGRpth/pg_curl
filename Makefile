$(OBJS): Makefile
DATA = $(wildcard *--*.sql)
EXTENSION = pg_curl
MODULE_big = $(EXTENSION)
OBJS = $(EXTENSION).o pg_whitelist/pg_whitelist.o
PG_CONFIG = pg_config
PG_CPPFLAGS = -Ipg_whitelist
PGXS = $(shell $(PG_CONFIG) --pgxs)
REGRESS = $(patsubst sql/%.sql,%,$(TESTS))
SHLIB_LINK = -lcurl
TESTS = $(wildcard sql/*.sql)
include $(PGXS)

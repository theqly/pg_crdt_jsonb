MODULE_big = pg_crdt_jsonb
OBJS = \
	$(WIN32RES) \
	pg_crdt_jsonb.o

EXTENSION = pg_crdt_jsonb
DATA = pg_crdt_jsonb--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR = $(shell $(PG_CONFIG) --includedir-server)

CFLAGS = -I$(INCLUDEDIR)

include $(PGXS)

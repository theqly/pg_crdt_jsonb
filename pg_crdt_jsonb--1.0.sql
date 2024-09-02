CREATE TYPE crdt_jsonb;

CREATE OR REPLACE FUNCTION crdt_jsonb_in(cstring) RETURNS crdt_jsonb
AS 'MODULE_PATHNAME', 'crdt_jsonb_in'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION crdt_jsonb_out(crdt_jsonb) RETURNS cstring
AS 'MODULE_PATHNAME', 'crdt_jsonb_out'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION crdt_jsonb_recv(internal) RETURNS crdt_jsonb
AS 'MODULE_PATHNAME', 'crdt_jsonb_recv'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION crdt_jsonb_send(crdt_jsonb) RETURNS bytea
AS 'MODULE_PATHNAME', 'crdt_jsonb_send'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION crdt_jsonb_append(crdt_jsonb, jsonb) RETURNS crdt_jsonb
AS 'MODULE_PATHNAME', 'crdt_jsonb_append'
LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION get_jsonb(crdt_jsonb) RETURNS jsonb
AS 'MODULE_PATHNAME', 'get_jsonb'
LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE crdt_jsonb (
    INTERNALLENGTH = variable,
    INPUT = crdt_jsonb_in,
    OUTPUT = crdt_jsonb_out,
    RECEIVE = crdt_jsonb_recv,
    SEND = crdt_jsonb_send,
    ALIGNMENT = int4
);

CREATE CAST (crdt_jsonb AS jsonb)
WITH FUNCTION get_jsonb (crdt_jsonb)
AS ASSIGNMENT;
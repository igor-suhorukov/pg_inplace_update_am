/* pg_inplace_update_am--0.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_inplace_update_am" to load this file. \quit

CREATE FUNCTION iheap_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD iheap TYPE TABLE HANDLER iheap_tableam_handler;
COMMENT ON ACCESS METHOD iheap IS 'table access method with in-place updates and undo';

-- Inspection functions (tests and diagnostics); superuser only.

CREATE FUNCTION iheap_metapage_info(rel regclass,
    OUT discard_ptr int8,
    OUT next_lpage int8,
    OUT nextents int4,
    OUT live_undo_pages int8,
    OUT free_extent_pages int8,
    OUT undo_peak_pages int8)
AS 'MODULE_PATHNAME', 'iheap_metapage_info'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION iheap_page_items(rel regclass, blkno int8,
    OUT lp int4,
    OUT lp_flags int4,
    OUT lp_off int4,
    OUT lp_len int4,
    OUT xid xid,
    OUT urec int8,
    OUT frozen bool,
    OUT natts int4,
    OUT hoff int4,
    OUT flags text[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'iheap_page_items'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION iheap_undo_page_records(rel regclass, blkno int8,
    OUT urp int8,
    OUT type text,
    OUT flags int4,
    OUT xid xid,
    OUT cid int8,
    OUT target_block int8,
    OUT target_offset int4,
    OUT len int4,
    OUT xact_prev int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'iheap_undo_page_records'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION iheap_relation_stats(rel regclass,
    OUT nblocks int8,
    OUT data_pages int8,
    OUT undo_pages int8,
    OUT other_pages int8,
    OUT live_items int8,
    OUT frozen_items int8,
    OUT deleted_items int8,
    OUT stub_items int8,
    OUT dead_items int8,
    OUT unused_items int8,
    OUT free_bytes int8)
AS 'MODULE_PATHNAME', 'iheap_relation_stats'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION iheap_discard(rel regclass)
RETURNS int8
AS 'MODULE_PATHNAME', 'iheap_discard_undo'
LANGUAGE C STRICT PARALLEL UNSAFE;

REVOKE ALL ON FUNCTION iheap_metapage_info(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_page_items(regclass, int8) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_undo_page_records(regclass, int8) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_relation_stats(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_discard(regclass) FROM PUBLIC;

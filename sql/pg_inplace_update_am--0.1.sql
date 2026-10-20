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

-- Verification (DESIGN.md §15): one row per problem found.
CREATE FUNCTION iheap_check(rel regclass, check_indexes boolean DEFAULT true,
    OUT blkno int8,
    OUT offnum int4,
    OUT msg text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'iheap_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

REVOKE ALL ON FUNCTION iheap_metapage_info(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_page_items(regclass, int8) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_undo_page_records(regclass, int8) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_relation_stats(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_discard(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_check(regclass, boolean) FROM PUBLIC;

-- Cumulative statistics (DESIGN.md §15), readable by everybody as core's
-- are; resetting them takes a superuser.
CREATE FUNCTION iheap_stat(rel regclass,
    OUT inplace_updates int8,
    OUT inplace_key_updates int8,
    OUT moved_updates int8,
    OUT moved_for_index int8,
    OUT moved_for_trigger int8,
    OUT moved_for_size int8,
    OUT moved_for_setting int8,
    OUT undo_records int8,
    OUT undo_bytes int8,
    OUT rollback_eager int8,
    OUT rollback_lazy int8,
    OUT rollback_discard int8,
    OUT rollback_bytes int8,
    OUT discarded_pages int8,
    OUT discarded_bytes int8,
    OUT undo_walks int8,
    OUT undo_walk_steps int8,
    OUT undo_walk_max int8,
    OUT stats_reset timestamptz)
AS 'MODULE_PATHNAME', 'iheap_stat'
LANGUAGE C STRICT STABLE PARALLEL RESTRICTED;

CREATE FUNCTION iheap_stat_reset(rel regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'iheap_stat_reset'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION iheap_stat_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'iheap_stat_reset_all'
LANGUAGE C PARALLEL UNSAFE;

REVOKE ALL ON FUNCTION iheap_stat_reset(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION iheap_stat_reset() FROM PUBLIC;

CREATE VIEW iheap_stat_tables AS
SELECT c.oid AS relid, n.nspname AS schemaname, c.relname, s.*
FROM pg_class c
     JOIN pg_namespace n ON n.oid = c.relnamespace
     JOIN pg_am a ON a.oid = c.relam
     CROSS JOIN LATERAL iheap_stat(c.oid) s
WHERE a.amname = 'iheap' AND c.relkind IN ('r', 'm');

GRANT SELECT ON iheap_stat_tables TO PUBLIC;

-- dmbtree: B-tree with delete-marking, for in-place updates of the key
-- columns of iheap tables (DESIGN.md §11.5).

CREATE FUNCTION dmbtree_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'dmbthandler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD dmbtree TYPE INDEX HANDLER dmbtree_handler;
COMMENT ON ACCESS METHOD dmbtree IS 'b-tree index access method that lets iheap update key columns in place';

-- Operator families and classes: copies of btree's, with the same
-- operators and support functions (so the planner's ordering and equality
-- reasoning, which maps operators to btree families, carries over), and
-- btree's storage types.  CREATE EXTENSION copies the built-in ones.  Called
-- again (by a superuser), it copies the ones created since, by CREATE
-- OPERATOR CLASS or by other extensions, and the members added to btree
-- families since.  The copies made then are not part of this extension: they
-- depend on the other extension's objects as its own btree classes do, so
-- dropping that extension takes CASCADE, and keeps them out of this one.
CREATE FUNCTION dmbtree_copy_btree_opclasses(builtin_only boolean DEFAULT false)
RETURNS integer
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	btree_am oid := (SELECT oid FROM pg_am WHERE amname = 'btree');
	dm_am oid := (SELECT oid FROM pg_am WHERE amname = 'dmbtree');
	fam record;
	twin oid;
	c record;
	items text;
	n integer := 0;
BEGIN
	FOR fam IN
		SELECT f.oid, f.opfname, f.opfnamespace::regnamespace AS nsp
		FROM pg_opfamily f
		WHERE f.opfmethod = btree_am
		  AND (NOT builtin_only OR f.oid < 16384)
		ORDER BY f.oid
	LOOP
		SELECT d.oid INTO twin FROM pg_opfamily d
		WHERE d.opfmethod = dm_am AND d.opfname = fam.opfname
		  AND d.opfnamespace = fam.nsp;
		IF twin IS NULL THEN
			EXECUTE format('CREATE OPERATOR FAMILY %s.%I USING dmbtree',
						   fam.nsp, fam.opfname);
			n := n + 1;
		END IF;
		-- the members the copy lacks: all of them, for a new one
		SELECT string_agg(item, ', ' ORDER BY kind, ord) INTO items FROM (
			SELECT format('OPERATOR %s %s', o.amopstrategy, o.amopopr::regoperator) AS item,
				   1 AS kind,
				   row_number() OVER (ORDER BY o.amoplefttype, o.amoprighttype, o.amopstrategy) AS ord
			FROM pg_amop o
			WHERE o.amopfamily = fam.oid AND o.amoppurpose = 's'
			  AND NOT EXISTS (SELECT 1 FROM pg_amop t
							  WHERE t.amopfamily = twin
								AND t.amoplefttype = o.amoplefttype
								AND t.amoprighttype = o.amoprighttype
								AND t.amopstrategy = o.amopstrategy)
			UNION ALL
			SELECT format('FUNCTION %s (%s, %s) %s', p.amprocnum,
						  p.amproclefttype::regtype, p.amprocrighttype::regtype,
						  p.amproc::regprocedure),
				   2,
				   row_number() OVER (ORDER BY p.amproclefttype, p.amprocrighttype, p.amprocnum)
			FROM pg_amproc p
			WHERE p.amprocfamily = fam.oid
			  AND NOT EXISTS (SELECT 1 FROM pg_amproc t
							  WHERE t.amprocfamily = twin
								AND t.amproclefttype = p.amproclefttype
								AND t.amprocrighttype = p.amprocrighttype
								AND t.amprocnum = p.amprocnum)
		) members;
		IF items IS NOT NULL THEN
			EXECUTE format('ALTER OPERATOR FAMILY %s.%I USING dmbtree ADD %s',
						   fam.nsp, fam.opfname, items);
		END IF;
	END LOOP;

	FOR c IN
		SELECT oc.opcname, oc.opcnamespace::regnamespace AS nsp,
			   oc.opcintype::regtype AS typ, oc.opcdefault,
			   coalesce(nullif(oc.opckeytype, 0::oid), oc.opcintype)::regtype AS storage,
			   f.opfname, f.opfnamespace::regnamespace AS fnsp
		FROM pg_opclass oc JOIN pg_opfamily f ON f.oid = oc.opcfamily
		WHERE oc.opcmethod = btree_am
		  AND (NOT builtin_only OR oc.oid < 16384)
		  AND NOT EXISTS (SELECT 1 FROM pg_opclass d
						  WHERE d.opcmethod = dm_am AND d.opcname = oc.opcname
							AND d.opcnamespace = oc.opcnamespace)
		ORDER BY oc.oid
	LOOP
		-- the members are the family's; the storage type is btree's (name_ops
		-- stores cstrings)
		EXECUTE format('CREATE OPERATOR CLASS %s.%I %s FOR TYPE %s USING dmbtree FAMILY %s.%I AS STORAGE %s',
					   c.nsp, c.opcname,
					   CASE WHEN c.opcdefault THEN 'DEFAULT' ELSE '' END,
					   c.typ, c.fnsp, c.opfname, c.storage);
		n := n + 1;
	END LOOP;
	RETURN n;
END;
$$;

REVOKE ALL ON FUNCTION dmbtree_copy_btree_opclasses(boolean) FROM PUBLIC;

SELECT dmbtree_copy_btree_opclasses(builtin_only => true);

CREATE FUNCTION dmbtree_index_stats(index regclass,
    OUT leaf_pages int8,
    OUT items int8,
    OUT tids int8,
    OUT marked int8,
    OUT dead_items int8)
AS 'MODULE_PATHNAME', 'dmbtree_index_stats'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE ALL ON FUNCTION dmbtree_index_stats(regclass) FROM PUBLIC;

-- Verification: a port of amcheck's bt_index_check() and
-- bt_index_parent_check() (DESIGN.md §11.5.8, §21 D34).  checkmarks also
-- checks the entries against the rows: each points to a used line pointer,
-- and an unmarked entry matches the newest committed version of its row.
CREATE FUNCTION dmbtree_check(index regclass,
    heapallindexed boolean DEFAULT false,
    checkunique boolean DEFAULT false,
    checkmarks boolean DEFAULT false)
RETURNS void
AS 'MODULE_PATHNAME', 'dmbtree_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION dmbtree_parent_check(index regclass,
    heapallindexed boolean DEFAULT false,
    rootdescend boolean DEFAULT false,
    checkunique boolean DEFAULT false,
    checkmarks boolean DEFAULT false)
RETURNS void
AS 'MODULE_PATHNAME', 'dmbtree_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

REVOKE ALL ON FUNCTION dmbtree_check(regclass, boolean, boolean, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION dmbtree_parent_check(regclass, boolean, boolean, boolean, boolean) FROM PUBLIC;

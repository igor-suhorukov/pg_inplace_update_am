/*-------------------------------------------------------------------------
 *
 * iheap_hooks.c
 *	  Making dmbtree the index of iheap tables.
 *
 *	- A utility hook gives CREATE INDEX without USING on an iheap table a
 *	  dmbtree index (iheap.default_index_am), and optionally unique indexes
 *	  and PRIMARY KEY and UNIQUE constraints too (iheap.unique_index_am):
 *	  only dmbtree indexes take in-place updates of their key columns.  An
 *	  index stays btree where dmbtree lacks an operator class btree has.
 *	- A planner hook (build_simple_rel_hook) presents dmbtree indexes as
 *	  btree indexes with btree's operator families, which have the same
 *	  operators: the planner then uses them wherever it would use btree,
 *	  including row comparisons and LIKE prefixes, which it reserves for
 *	  btree.  Execution goes through the index's own access method.  (The
 *	  copies of btree's families keep their names, which maps partitioned
 *	  indexes, for which plancat finds no sort families.)
 *	- An object access hook drops the cumulative statistics of an iheap
 *	  relation with the relation (iheap_stats.c).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "common/keywords.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "optimizer/pathnode.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/scanner.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "iheap.h"

typedef enum IHeapIndexAm
{
	IHEAP_INDEX_AM_BTREE,
	IHEAP_INDEX_AM_DMBTREE,
} IHeapIndexAm;

static const struct config_enum_entry index_am_options[] = {
	{"btree", IHEAP_INDEX_AM_BTREE, false},
	{"dmbtree", IHEAP_INDEX_AM_DMBTREE, false},
	{NULL, 0, false}
};

static int	iheap_default_index_am = IHEAP_INDEX_AM_DMBTREE;
static int	iheap_unique_index_am = IHEAP_INDEX_AM_BTREE;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static build_simple_rel_hook_type prev_build_simple_rel = NULL;
static object_access_hook_type prev_object_access = NULL;

static bool
default_table_am_is_iheap(void)
{
	return default_table_access_method != NULL &&
		strcmp(default_table_access_method, "iheap") == 0;
}

/*
 * Is the table AM of the existing relation "rv" iheap?  A partitioned
 * table created without USING has none (its partitions get the default),
 * so that decides for it: its index and its partitions' indexes must have
 * one AM to be attached to each other.
 */
static bool
relation_is_iheap(RangeVar *rv)
{
	Oid			relid = RangeVarGetRelid(rv, NoLock, true);
	Oid			iheap_am = get_table_am_oid("iheap", true);
	HeapTuple	tp;
	bool		result = false;

	if (!OidIsValid(relid) || !OidIsValid(iheap_am))
		return false;
	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tp);

		if (form->relkind == RELKIND_PARTITIONED_TABLE && !OidIsValid(form->relam))
			result = default_table_am_is_iheap();
		else
			result = form->relam == iheap_am;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * Does the CREATE INDEX statement name its access method?  The parser
 * fills in "btree" when it does not, so look for USING ahead of the column
 * list, with the SQL lexer (USING is a reserved keyword; a quoted
 * identifier is never taken for it).
 */
static bool
index_stmt_names_am(const char *queryString, PlannedStmt *pstmt)
{
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			using_kw = ScanKeywordLookup("using", &ScanKeywords);
	int			using_tok = using_kw >= 0 ? ScanKeywordTokens[using_kw] : -1;
	const char *start;
	char	   *str;
	bool		result = false;

	if (queryString == NULL)
		return true;			/* can't tell: leave it alone */
	start = queryString + Max(pstmt->stmt_location, 0);
	str = pnstrdup(start, pstmt->stmt_len > 0 ? pstmt->stmt_len : strlen(start));

	yyscanner = scanner_init(str, &yyextra, &ScanKeywords, ScanKeywordTokens);
	for (;;)
	{
		int			tok = core_yylex(&yylval, &yylloc, yyscanner);

		if (tok == 0 || tok == '(')
			break;
		if (tok == using_tok)
		{
			result = true;
			break;
		}
	}
	scanner_finish(yyscanner);
	pfree(str);
	return result;
}

/* ------------------------------------------------------------------------
 * Operator classes
 *
 * dmbtree's operator classes are the copies of btree's that CREATE
 * EXTENSION made (dmbtree_copy_btree_opclasses()).  A class created later,
 * by CREATE OPERATOR CLASS or by another extension, has no copy until a
 * superuser makes one; an index needing it stays btree, with a notice,
 * since updates of its columns will move rows.  An index btree would refuse
 * (no class at all, or a pattern class with a nondeterministic collation)
 * stays btree too, to be refused with core's error.
 * ------------------------------------------------------------------------
 */

static void
report_btree_instead(Oid relid, const char *missing)
{
	ereport(NOTICE,
			(errmsg("index on \"%s\" uses access method btree, not dmbtree",
					get_rel_name(relid)),
			 errdetail("Access method \"dmbtree\" has no %s.", missing),
			 errhint("A superuser can copy the btree operator classes created since CREATE EXTENSION with SELECT dmbtree_copy_btree_opclasses().")));
}

/*
 * Has dmbtree a default operator class for type typid?  If not, *missing
 * says so, unless btree has none either.
 */
static bool
dmbtree_has_default_opclass(Oid typid, Oid dmbtree_am, char **missing)
{
	if (OidIsValid(GetDefaultOpClass(typid, dmbtree_am)))
		return true;
	if (OidIsValid(GetDefaultOpClass(typid, BTREE_AM_OID)))
		*missing = psprintf("default operator class for data type %s",
							format_type_be(typid));
	return false;
}

/*
 * The type and collation of an index expression, as transformIndexStmt()
 * will find them.
 */
static Oid
index_expr_type(Relation rel, Node *expr, const char *queryString,
				Oid *collation)
{
	ParseState *pstate = make_parsestate(NULL);
	ParseNamespaceItem *nsitem;
	Node	   *texpr;
	Oid			result;

	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, false, true, true);
	texpr = transformExpr(pstate, copyObject(expr), EXPR_KIND_INDEX_EXPRESSION);
	assign_expr_collations(pstate, texpr);
	result = exprType(texpr);
	*collation = exprCollation(texpr);
	free_parsestate(pstate);
	return result;
}

/* Can the index that stmt describes be a dmbtree index? */
static bool
dmbtree_takes_index(IndexStmt *stmt, const char *queryString)
{
	Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	Oid			dmbtree_am = get_index_am_oid("dmbtree", false);
	Relation	rel = NULL;
	char	   *missing = NULL;
	bool		ok = true;
	ListCell   *lc;

	if (!OidIsValid(relid))
		return true;			/* DefineIndex reports it */

	foreach(lc, stmt->indexParams)
	{
		IndexElem  *elem = lfirst(lc);
		Oid			typid;
		Oid			collid;

		if (elem->name)
		{
			HeapTuple	tp = SearchSysCacheAttName(relid, elem->name);

			if (!HeapTupleIsValid(tp))
				continue;		/* DefineIndex reports it */
			typid = ((Form_pg_attribute) GETSTRUCT(tp))->atttypid;
			collid = ((Form_pg_attribute) GETSTRUCT(tp))->attcollation;
			ReleaseSysCache(tp);
		}
		else if (stmt->transformed)
		{
			typid = exprType(elem->expr);
			collid = exprCollation(elem->expr);
		}
		else
		{
			/* the owner's statement only: this locks the table */
			if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
				break;			/* DefineIndex refuses it */
			if (rel == NULL)
				rel = relation_open(relid, AccessShareLock);
			typid = index_expr_type(rel, elem->expr, queryString, &collid);
		}
		if (elem->collation)
			collid = get_collation_oid(elem->collation, true);

		if (elem->opclass)
		{
			Oid			btclass = get_opclass_oid(BTREE_AM_OID, elem->opclass, true);

			if ((btclass == TEXT_BTREE_PATTERN_OPS_OID ||
				 btclass == VARCHAR_BTREE_PATTERN_OPS_OID ||
				 btclass == BPCHAR_BTREE_PATTERN_OPS_OID) &&
				OidIsValid(collid) && !get_collation_isdeterministic(collid))
				ok = false;
			else if (!OidIsValid(get_opclass_oid(dmbtree_am, elem->opclass, true)))
			{
				ok = false;
				if (OidIsValid(btclass))
					missing = psprintf("operator class \"%s\"",
									   NameListToString(elem->opclass));
			}
		}
		else if (!dmbtree_has_default_opclass(typid, dmbtree_am, &missing))
			ok = false;
		if (!ok)
			break;
	}
	if (rel != NULL)
		relation_close(rel, NoLock);
	if (missing != NULL)
		report_btree_instead(relid, missing);
	return ok;
}

/* the type a column definition gives, serial resolved as CREATE TABLE does */
static Oid
column_def_type(ColumnDef *col)
{
	TypeName   *tn = col->typeName;

	if (tn == NULL)
		return InvalidOid;
	if (list_length(tn->names) == 1 && !tn->pct_type && tn->arrayBounds == NIL)
	{
		const char *name = strVal(linitial(tn->names));

		if (strcmp(name, "smallserial") == 0 || strcmp(name, "serial2") == 0)
			return INT2OID;
		if (strcmp(name, "serial") == 0 || strcmp(name, "serial4") == 0)
			return INT4OID;
		if (strcmp(name, "bigserial") == 0 || strcmp(name, "serial8") == 0)
			return INT8OID;
	}
	return LookupTypeNameOid(NULL, tn, true);
}

/* the type of a column of relid, or of one the same ALTER TABLE adds */
static Oid
column_type(Oid relid, const char *name, List *cmds)
{
	HeapTuple	tp = SearchSysCacheAttName(relid, name);
	ListCell   *lc;

	if (HeapTupleIsValid(tp))
	{
		Oid			result = ((Form_pg_attribute) GETSTRUCT(tp))->atttypid;

		ReleaseSysCache(tp);
		return result;
	}
	foreach(lc, cmds)
	{
		AlterTableCmd *cmd = lfirst(lc);

		if (cmd->subtype == AT_AddColumn && IsA(cmd->def, ColumnDef) &&
			strcmp(((ColumnDef *) cmd->def)->colname, name) == 0)
			return column_def_type((ColumnDef *) cmd->def);
	}
	return InvalidOid;
}

/*
 * Can the PRIMARY KEY or UNIQUE constraint con that ALTER TABLE adds to
 * relid (on column col, for a column constraint) have a dmbtree index?
 * Constraints take default operator classes.
 */
static bool
dmbtree_takes_constraint(Oid relid, Constraint *con, ColumnDef *col,
						 List *cmds)
{
	Oid			dmbtree_am = get_index_am_oid("dmbtree", false);
	char	   *missing = NULL;
	ListCell   *lc;

	if (col != NULL)
	{
		Oid			typid = column_def_type(col);

		if (!OidIsValid(typid) ||
			!dmbtree_has_default_opclass(typid, dmbtree_am, &missing))
			goto btree;
		return true;
	}
	foreach(lc, con->keys)
	{
		Oid			typid = column_type(relid, strVal(lfirst(lc)), cmds);

		if (!OidIsValid(typid) ||
			!dmbtree_has_default_opclass(typid, dmbtree_am, &missing))
			goto btree;
	}
	return true;

btree:
	if (missing != NULL)
		report_btree_instead(relid, missing);
	return false;
}

/* ------------------------------------------------------------------------
 * The utility hook
 * ------------------------------------------------------------------------
 */

/*
 * PRIMARY KEY and UNIQUE constraints get iheap.unique_index_am.  ALTER
 * TABLE (relid valid) checks the operator classes here; CREATE TABLE
 * checks them when the index is made, the table's columns then known
 * (choose_index_am()).
 */
static void
rewrite_constraint(Constraint *con, ColumnDef *col, Oid relid, List *cmds)
{
	if ((con->contype == CONSTR_PRIMARY || con->contype == CONSTR_UNIQUE) &&
		con->access_method == NULL && con->indexname == NULL &&
		!con->without_overlaps &&
		(!OidIsValid(relid) || dmbtree_takes_constraint(relid, con, col, cmds)))
		con->access_method = pstrdup("dmbtree");
}

static void
rewrite_column_constraints(ColumnDef *col, Oid relid, List *cmds)
{
	ListCell   *lc;

	foreach(lc, col->constraints)
	{
		if (IsA(lfirst(lc), Constraint))
			rewrite_constraint((Constraint *) lfirst(lc), col, relid, cmds);
	}
}

static void
rewrite_table_elements(List *elts)
{
	ListCell   *lc;

	foreach(lc, elts)
	{
		Node	   *n = lfirst(lc);

		if (IsA(n, Constraint))
			rewrite_constraint((Constraint *) n, NULL, InvalidOid, NIL);
		else if (IsA(n, ColumnDef))
			rewrite_column_constraints((ColumnDef *) n, InvalidOid, NIL);
	}
}

/* does CREATE TABLE make an iheap table? */
static bool
create_stmt_is_iheap(CreateStmt *stmt)
{
	if (stmt->accessMethod != NULL)
		return strcmp(stmt->accessMethod, "iheap") == 0;
	return default_table_am_is_iheap();
}

/*
 * The access method a CREATE INDEX should have instead of the one it
 * names, or NULL to leave it alone.
 */
static const char *
choose_index_am(IndexStmt *stmt, const char *queryString, PlannedStmt *pstmt,
				ProcessUtilityContext context)
{
	int			am = stmt->unique ? iheap_unique_index_am :
		iheap_default_index_am;

	if (stmt->accessMethod == NULL || stmt->excludeOpNames != NIL ||
		!OidIsValid(get_index_am_oid("dmbtree", true)))
		return NULL;

	/* what users write */
	if (context != PROCESS_UTILITY_SUBCOMMAND && !stmt->isconstraint)
	{
		if (am == IHEAP_INDEX_AM_DMBTREE &&
			strcmp(stmt->accessMethod, DEFAULT_INDEX_TYPE) == 0 &&
			relation_is_iheap(stmt->relation) &&
			!index_stmt_names_am(queryString, pstmt) &&
			dmbtree_takes_index(stmt, queryString))
			return "dmbtree";
		return NULL;
	}

	/* the index of a constraint that rewrite_constraint() gave dmbtree */
	if (context == PROCESS_UTILITY_SUBCOMMAND && stmt->isconstraint &&
		strcmp(stmt->accessMethod, "dmbtree") == 0 &&
		!dmbtree_takes_index(stmt, queryString))
		return DEFAULT_INDEX_TYPE;
	return NULL;
}

/*
 * Rewrite CREATE TABLE or ALTER TABLE (a copy) as iheap.unique_index_am
 * says.  Returns true if it may change anything.
 */
static bool
rewrite_utility(Node *parsetree, bool apply)
{
	if (iheap_unique_index_am != IHEAP_INDEX_AM_DMBTREE ||
		!OidIsValid(get_index_am_oid("dmbtree", true)))
		return false;

	if (IsA(parsetree, CreateStmt))
	{
		CreateStmt *stmt = (CreateStmt *) parsetree;

		if (!create_stmt_is_iheap(stmt))
			return false;
		if (apply)
		{
			rewrite_table_elements(stmt->tableElts);
			rewrite_table_elements(stmt->constraints);
		}
		return true;
	}
	else if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
		Oid			relid;
		ListCell   *lc;

		if (stmt->objtype != OBJECT_TABLE || !relation_is_iheap(stmt->relation))
			return false;
		if (!apply)
			return true;
		relid = RangeVarGetRelid(stmt->relation, NoLock, false);
		foreach(lc, stmt->cmds)
		{
			AlterTableCmd *cmd = lfirst(lc);

			if (cmd->subtype == AT_AddConstraint && IsA(cmd->def, Constraint))
				rewrite_constraint((Constraint *) cmd->def, NULL, relid,
								   stmt->cmds);
			else if (cmd->subtype == AT_AddColumn && IsA(cmd->def, ColumnDef))
				rewrite_column_constraints((ColumnDef *) cmd->def, relid,
										   stmt->cmds);
		}
		return true;
	}
	return false;
}

static void
iheap_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					 bool readOnlyTree, ProcessUtilityContext context,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	/* the statement may be in a plan cache: change a copy */
	if (IsA(parsetree, IndexStmt))
	{
		const char *am = choose_index_am((IndexStmt *) parsetree, queryString,
										 pstmt, context);

		if (am != NULL)
		{
			pstmt = copyObject(pstmt);
			readOnlyTree = false;
			((IndexStmt *) pstmt->utilityStmt)->accessMethod = pstrdup(am);
		}
	}
	else if ((IsA(parsetree, CreateStmt) || IsA(parsetree, AlterTableStmt)) &&
			 rewrite_utility(parsetree, false))
	{
		pstmt = copyObject(pstmt);
		readOnlyTree = false;
		(void) rewrite_utility(pstmt->utilityStmt, true);
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* the btree operator family a dmbtree family was copied from */
/*
 * Does dmbtree family dmfam have the operators and support functions of
 * btree family btfam that an index column of type lefttype can use?  The
 * planner may make an index condition of any operator of the family it is
 * shown whose left input is the column's type, and the executor looks it
 * up in the index's own family: a copy lacks what was added to btree's
 * family after it was made, until dmbtree_copy_btree_opclasses() runs
 * again.  The answers are cached until pg_amop or pg_amproc changes.
 */
typedef struct FamilyPair
{
	Oid			dmfam;
	Oid			btfam;
	Oid			lefttype;
} FamilyPair;

typedef struct FamilyCover
{
	FamilyPair	key;
	bool		covers;
} FamilyCover;

static HTAB *family_covers = NULL;
static uint64 family_gen = 1;	/* bumped by changes of pg_amop, pg_amproc */
static uint64 family_covers_gen = 0;	/* family_gen family_covers is for */

static void
family_inval_callback(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	family_gen++;
}

static bool
dm_family_covers(Oid dmfam, Oid btfam, Oid lefttype)
{
	FamilyPair	key = {dmfam, btfam, lefttype};
	FamilyCover *fc;
	uint64		gen;
	CatCList   *list;
	bool		covers = true;

	if (family_covers != NULL && family_covers_gen != family_gen)
	{
		hash_destroy(family_covers);
		family_covers = NULL;
	}
	if (family_covers != NULL &&
		(fc = hash_search(family_covers, &key, HASH_FIND, NULL)) != NULL)
		return fc->covers;

	/* the lookups may process invalidations: the hash is touched after */
	gen = family_gen;
	list = SearchSysCacheList2(AMOPSTRATEGY, ObjectIdGetDatum(btfam),
							   ObjectIdGetDatum(lefttype));
	for (int i = 0; covers && i < list->n_members; i++)
	{
		Form_pg_amop op = (Form_pg_amop) GETSTRUCT(&list->members[i]->tuple);

		covers = get_opfamily_member(dmfam, op->amoplefttype,
									 op->amoprighttype,
									 op->amopstrategy) == op->amopopr;
	}
	ReleaseSysCacheList(list);
	list = SearchSysCacheList2(AMPROCNUM, ObjectIdGetDatum(btfam),
							   ObjectIdGetDatum(lefttype));
	for (int i = 0; covers && i < list->n_members; i++)
	{
		Form_pg_amproc proc = (Form_pg_amproc) GETSTRUCT(&list->members[i]->tuple);

		covers = get_opfamily_proc(dmfam, proc->amproclefttype,
								   proc->amprocrighttype,
								   proc->amprocnum) == proc->amproc;
	}
	ReleaseSysCacheList(list);

	if (gen != family_gen)
		return covers;			/* changed meanwhile: don't cache */
	if (family_covers == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(FamilyPair);
		ctl.entrysize = sizeof(FamilyCover);
		ctl.hcxt = CacheMemoryContext;
		family_covers = hash_create("iheap dmbtree family covers", 16, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		family_covers_gen = gen;
	}
	fc = hash_search(family_covers, &key, HASH_ENTER, NULL);
	fc->covers = covers;
	return covers;
}

static Oid
btree_twin(Oid opfamily)
{
	HeapTuple	tp = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfamily));
	Oid			result = InvalidOid;

	if (HeapTupleIsValid(tp))
	{
		Form_pg_opfamily form = (Form_pg_opfamily) GETSTRUCT(tp);

		result = GetSysCacheOid3(OPFAMILYAMNAMENSP, Anum_pg_opfamily_oid,
								 ObjectIdGetDatum(BTREE_AM_OID),
								 NameGetDatum(&form->opfname),
								 ObjectIdGetDatum(form->opfnamespace));
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * Present dmbtree indexes to the planner as btree indexes.  dmbtree's
 * families are copies of btree's, with the same operators, so the planner
 * may as well see btree's: for an index plancat has mapped each key
 * column's "<" operator to its btree family (sortopfamily, for ORDER BY);
 * a partitioned index has none, and gets the btree family of the same name
 * (its uniqueness proves things too, e.g. that GROUP BY columns are
 * redundant).  Index scans are built from the index's catalog entries and
 * run through dmbtree.  An index whose families lack some of btree's
 * operators for its key columns (dm_family_covers) is left as it is: the
 * planner then uses dmbtree's own operators, without what it reserves for
 * btree.
 */
static void
iheap_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	ListCell   *lc;
	Oid			dmbtree_am = InvalidOid;

	if (prev_build_simple_rel)
		prev_build_simple_rel(root, rel, rte);

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		Oid			twins[INDEX_MAX_KEYS];
		int			i;

		if (!OidIsValid(dmbtree_am))
		{
			dmbtree_am = get_index_am_oid("dmbtree", true);
			if (!OidIsValid(dmbtree_am))
				return;
		}
		if (index->relam != dmbtree_am)
			continue;
		for (i = 0; i < index->nkeycolumns; i++)
		{
			twins[i] = index->sortopfamily ? index->sortopfamily[i] :
				btree_twin(index->opfamily[i]);
			if (!OidIsValid(twins[i]) ||
				!dm_family_covers(index->opfamily[i], twins[i],
								  index->opcintype[i]))
				break;
		}
		if (i < index->nkeycolumns)
			continue;
		index->relam = BTREE_AM_OID;
		for (i = 0; i < index->nkeycolumns; i++)
			index->opfamily[i] = twins[i];
	}
}

/* ------------------------------------------------------------------------
 * The object access hook
 * ------------------------------------------------------------------------
 */
static void
iheap_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
		iheap_stat_relation_dropped(objectId);
}

void
iheap_hooks_init(void)
{
	DefineCustomEnumVariable("iheap.default_index_am",
							 "Index access method of CREATE INDEX without USING on iheap tables.",
							 "dmbtree indexes take in-place updates of their key columns; "
							 "with btree, an update of an indexed column moves the row.",
							 &iheap_default_index_am,
							 IHEAP_INDEX_AM_DMBTREE,
							 index_am_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("iheap.unique_index_am",
							 "Index access method of unique indexes and of PRIMARY KEY and UNIQUE constraints of iheap tables.",
							 "btree keeps core's fast path of foreign key checks and "
							 "ADD CONSTRAINT ... USING INDEX, which take btree indexes only.",
							 &iheap_unique_index_am,
							 IHEAP_INDEX_AM_BTREE,
							 index_am_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = iheap_ProcessUtility;
	prev_build_simple_rel = build_simple_rel_hook;
	build_simple_rel_hook = iheap_build_simple_rel;
	prev_object_access = object_access_hook;
	object_access_hook = iheap_object_access;
	CacheRegisterSyscacheCallback(AMOPSTRATEGY, family_inval_callback, (Datum) 0);
	CacheRegisterSyscacheCallback(AMPROCNUM, family_inval_callback, (Datum) 0);
}

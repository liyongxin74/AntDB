/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  transform the raw parse tree into a query tree
 *
 * For optimizable statements, we are careful to obtain a suitable lock on
 * each referenced table, and other modules of the backend preserve or
 * re-obtain these locks before depending on the results.  It is therefore
 * okay to do significant semantic analysis of these statements.  For
 * utility commands, no locks are obtained here (and if they were, we could
 * not be sure we'd still have them at execution).  Hence the general rule
 * for utility commands is to just dump them into a Query node untransformed.
 * DECLARE CURSOR, EXPLAIN, and CREATE TABLE AS are exceptions because they
 * contain optimizable statements, which we should transform.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/analyze.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#ifdef PGXC
#include "catalog/pg_inherits.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/indexing.h"
#include "utils/fmgroids.h"
#include "utils/tqual.h"
#endif
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_oper.h"
#include "parser/parse_param.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#ifdef PGXC
#include "miscadmin.h"
#include "pgxc/pgxc.h"
#include "pgxc/pgxcnode.h"
#include "utils/lsyscache.h"
#include "optimizer/pgxcplan.h"
#include "tcop/tcopprot.h"
#include "nodes/nodes.h"
#include "pgxc/poolmgr.h"
#include "catalog/pgxc_node.h"
#include "pgxc/xc_maintenance_mode.h"
#include "access/xact.h"
#endif
#include "utils/rel.h"
/* Added these header file to resolve conflicts, K.Suzuki */
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "miscadmin.h"

#ifdef ADB
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "parser/parser.h"
#include "utils/syscache.h"
#endif
/* End of addition */

/* Hook for plugins to get control at end of parse analysis */
post_parse_analyze_hook_type post_parse_analyze_hook = NULL;

static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt);
static List *transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos);
static int	count_rowexpr_columns(ParseState *pstate, Node *expr);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **targetlist);
static void determineRecursiveColTypes(ParseState *pstate,
						   Node *larg, List *nrtargetlist);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static List *transformReturningList(ParseState *pstate, List *returningList);
static Query *transformDeclareCursorStmt(ParseState *pstate,
						   DeclareCursorStmt *stmt);
static Query *transformExplainStmt(ParseState *pstate,
					 ExplainStmt *stmt);
static Query *transformCreateTableAsStmt(ParseState *pstate,
						   CreateTableAsStmt *stmt);
#ifdef PGXC
static Query *transformExecDirectStmt(ParseState *pstate, ExecDirectStmt *stmt);
static bool IsExecDirectUtilityStmt(Node *node);
static bool is_relation_child(RangeTblEntry *child_rte, List *rtable);
static bool is_rel_child_of_rel(RangeTblEntry *child_rte, RangeTblEntry *parent_rte);
#endif

static void transformLockingClause(ParseState *pstate, Query *qry,
					   LockingClause *lc, bool pushedDown);
#ifdef ADB
static Node* transformFromAndWhere(ParseState *pstate, Node *quals);
static void check_joinon_column_join(Node *node, ParseState *pstate);
static void rewrite_rownum_query(Query *query);
static bool rewrite_rownum_query_enum(Node *node, void *context);
static bool const_get_int64(const Expr *expr, int64 *val);
static Expr* make_int8_const(Datum value);
static Oid get_operator_for_function(Oid funcid);
#endif /* ADB */

/*
 * parse_analyze
 *		Analyze a raw parse tree and transform it to Query form.
 *
 * Optionally, information about $n parameter types can be supplied.
 * References to $n indexes not defined by paramTypes[] are disallowed.
 *
 * The result is a Query node.  Optimizable statements require considerable
 * transformation, while utility-type statements are simply hung off
 * a dummy CMD_UTILITY Query node.
 */
Query *
parse_analyze(Node *parseTree, const char *sourceText,
			  Oid *paramTypes, int numParams)
{
#ifdef ADB
	return parse_analyze_for_gram(parseTree, sourceText
						,paramTypes, numParams, PARSE_GRAM_POSTGRES);
}

Query *
parse_analyze_for_gram(Node *parseTree, const char *sourceText,
			  Oid *paramTypes, int numParams, ParseGrammar grammar)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	pstate->p_grammar = grammar;
	PushOverrideSearchPathForGrammar(grammar);
	PG_TRY();
	{
#else
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
#endif

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	if (numParams > 0)
		parse_fixed_parameters(pstate, paramTypes, numParams);

	query = transformTopLevelStmt(pstate, parseTree);
#ifdef ADB
	check_joinon_column_join((Node*)(query->jointree), pstate);
	rewrite_rownum_query_enum((Node*)query, NULL);
#endif /* ADB */

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query);

	free_parsestate(pstate);
#ifdef ADB
	}PG_CATCH();
	{
		PopOverrideSearchPath();
		PG_RE_THROW();
	}PG_END_TRY();
	PopOverrideSearchPath();
#endif

	return query;
}

/*
 * parse_analyze_varparams
 *
 * This variant is used when it's okay to deduce information about $n
 * symbol datatypes from context.  The passed-in paramTypes[] array can
 * be modified or enlarged (via repalloc).
 */
Query *
parse_analyze_varparams(Node *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams)
{
#ifdef ADB
	return parse_analyze_varparams_for_gram(parseTree, sourceText
		,paramTypes, numParams, PARSE_GRAM_POSTGRES);
}
Query *parse_analyze_varparams_for_gram(Node *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams, ParseGrammar grammar)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	pstate->p_grammar = grammar;
	PushOverrideSearchPathForGrammar(grammar);
	PG_TRY();
	{
#else /* ADB */
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
#endif /* ADB */

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	parse_variable_parameters(pstate, paramTypes, numParams);

	query = transformTopLevelStmt(pstate, parseTree);

	/* make sure all is well with parameter types */
	check_variable_parameters(pstate, query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query);

	free_parsestate(pstate);
#ifdef ADB
	}PG_CATCH();
	{
		PopOverrideSearchPath();
		PG_RE_THROW();
	}PG_END_TRY();
	PopOverrideSearchPath();
#endif /* ADB */
	return query;
}

/*
 * parse_sub_analyze
 *		Entry point for recursively analyzing a sub-statement.
 */
Query *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent)
{
	ParseState *pstate = make_parsestate(parentParseState);
	Query	   *query;

	pstate->p_parent_cte = parentCTE;
	pstate->p_locked_from_parent = locked_from_parent;

	query = transformStmt(pstate, parseTree);

	free_parsestate(pstate);

	return query;
}

/*
 * transformTopLevelStmt -
 *	  transform a Parse tree into a Query tree.
 *
 * The only thing we do here that we don't do in transformStmt() is to
 * convert SELECT ... INTO into CREATE TABLE AS.  Since utility statements
 * aren't allowed within larger statements, this is only allowed at the top
 * of the parse tree, and so we only try it before entering the recursive
 * transformStmt() processing.
 */
Query *
transformTopLevelStmt(ParseState *pstate, Node *parseTree)
{
	if (IsA(parseTree, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) parseTree;

		/* If it's a set-operation tree, drill down to leftmost SelectStmt */
		while (stmt && stmt->op != SETOP_NONE)
			stmt = stmt->larg;
		Assert(stmt && IsA(stmt, SelectStmt) &&stmt->larg == NULL);

		if (stmt->intoClause)
		{
			CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

			ctas->query = parseTree;
			ctas->into = stmt->intoClause;
			ctas->relkind = OBJECT_TABLE;
			ctas->is_select_into = true;

			/*
			 * Remove the intoClause from the SelectStmt.  This makes it safe
			 * for transformSelectStmt to complain if it finds intoClause set
			 * (implying that the INTO appeared in a disallowed place).
			 */
			stmt->intoClause = NULL;

			parseTree = (Node *) ctas;
		}
	}

	return transformStmt(pstate, parseTree);
}

/*
 * transformStmt -
 *	  recursively transform a Parse tree into a Query tree.
 */
Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result;

	switch (nodeTag(parseTree))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
											(DeclareCursorStmt *) parseTree);
			break;

		case T_ExplainStmt:
			result = transformExplainStmt(pstate,
										  (ExplainStmt *) parseTree);
			break;

#ifdef PGXC
		case T_ExecDirectStmt:
			result = transformExecDirectStmt(pstate,
											 (ExecDirectStmt *) parseTree);
			break;
#endif

		case T_CreateTableAsStmt:
			result = transformCreateTableAsStmt(pstate,
											(CreateTableAsStmt *) parseTree);
			break;

		default:

			/*
			 * other statements don't require any transformation; just return
			 * the original parsetree with a Query node plastered on top.
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* Mark as original query until we learn differently */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	return result;
}

/*
 * analyze_requires_snapshot
 *		Returns true if a snapshot must be set before doing parse analysis
 *		on the given raw parse tree.
 *
 * Classification here should match transformStmt().
 */
bool
analyze_requires_snapshot(Node *parseTree)
{
	bool		result;

	switch (nodeTag(parseTree))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_SelectStmt:
			result = true;
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
			/* yes, because it's analyzed just like SELECT */
			result = true;
			break;

		case T_ExplainStmt:
		case T_CreateTableAsStmt:
			/* yes, because we must analyze the contained statement */
			result = true;
			break;

#ifdef PGXC
		case T_ExecDirectStmt:

			/*
			 * We will parse/analyze/plan inner query, which probably will
			 * need a snapshot. Ensure it is set.
			 */
			result = true;
			break;
#endif

		default:
			/* other utility statements don't have any real parse analysis */
			result = false;
			break;
	}

	return result;
}

/*
 * transformDeleteStmt -
 *	  transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;
#ifdef PGXC
	ListCell   *tl;
#endif

	qry->commandType = CMD_DELETE;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
#ifdef PGXC
		/*
		 * For a WITH query that deletes from a parent table in the
		 * main query & inserts a row in the child table in the WITH query
		 * we need to use command ID communication to remote nodes in order
		 * to maintain global data visibility.
		 * For example
		 * CREATE TEMP TABLE parent ( id int, val text ) DISTRIBUTE BY REPLICATION;
		 * CREATE TEMP TABLE child ( ) INHERITS ( parent ) DISTRIBUTE BY REPLICATION;
		 * INSERT INTO parent VALUES ( 42, 'old' );
		 * INSERT INTO child VALUES ( 42, 'older' );
		 * WITH wcte AS ( INSERT INTO child VALUES ( 42, 'new' ) RETURNING id AS newid )
		 * DELETE FROM parent USING wcte WHERE id = newid;
		 * The last query gets translated into the following multi-statement
		 * transaction on the primary datanode
		 * (a) SELECT id, ctid FROM ONLY parent WHERE true
		 * (b) START TRANSACTION ISOLATION LEVEL read committed READ WRITE
		 * (c) INSERT INTO child (id, val) VALUES ($1, $2) RETURNING id -- (42, 'new')
		 * (d) DELETE FROM ONLY parent parent WHERE (parent.ctid = $1)
		 * (e) SELECT id, ctid FROM ONLY child parent WHERE true
		 * (f) DELETE FROM ONLY child parent WHERE (parent.ctid = $1)
		 * (g) COMMIT TRANSACTION
		 * The command id of the select in step (e), should be such that
		 * it does not see the insert of step (c)
		 */
		if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
		{
			foreach(tl, stmt->withClause->ctes)
			{
				CommonTableExpr *cte = (CommonTableExpr *) lfirst(tl);
				if (IsA(cte->ctequery, InsertStmt))
				{
					qry->has_to_save_cmd_id = true;
					SetSendCommandId(true);
					break;
				}
			}
		}
#endif

		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* set up range table with just the result rel */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
								  interpretInhOption(stmt->relation->inhOpt),
										 true,
										 ACL_DELETE);

	/* grab the namespace item made by setTargetTable */
	nsitem = (ParseNamespaceItem *) llast(pstate->p_namespace);

	/* there's no DISTINCT in DELETE */
	qry->distinctClause = NIL;

	/* subqueries in USING cannot access the result relation */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * The USING clause is non-standard SQL syntax, and is equivalent in
	 * functionality to the FROM list that can be specified for UPDATE. The
	 * USING keyword is used rather than FROM because FROM is already a
	 * keyword in the DELETE syntax.
	 */
	transformFromClause(pstate, stmt->usingClause);

	/* remaining clauses can reference the result relation normally */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs)
		parseCheckAggregates(pstate, qry);

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformInsertStmt -
 *	  transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_namespace;
	List	   *icolumns;
	List	   *attrnos;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;

	/* There can't be any outer WITH to worry about */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * We have three cases to deal with: DEFAULT VALUES (selectStmt == NULL),
	 * VALUES list, or general SELECT input.  We special-case VALUES, both for
	 * efficiency and so we can handle DEFAULT specifications.
	 *
	 * The grammar allows attaching ORDER BY, LIMIT, FOR UPDATE, or WITH to a
	 * VALUES clause.  If we have any of those, treat it as a general SELECT;
	 * so it will work, but you can't use DEFAULT items together with those.
	 */
	isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
									  selectStmt->sortClause != NIL ||
									  selectStmt->limitOffset != NULL ||
									  selectStmt->limitCount != NULL ||
									  selectStmt->lockingClause != NIL ||
									  selectStmt->withClause != NULL));

	/*
	 * If a non-nil rangetable/namespace was passed in, and we are doing
	 * INSERT/SELECT, arrange to pass the rangetable/namespace down to the
	 * SELECT.  This can only happen if we are inside a CREATE RULE, and in
	 * that case we want the rule's OLD and NEW rtable entries to appear as
	 * part of the SELECT's rtable, not as outer references for it.  (Kluge!)
	 * The SELECT's joinlist is not affected however.  We must do this before
	 * adding the target table to the INSERT's rtable.
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_namespace = pstate->p_namespace;
		pstate->p_namespace = NIL;
	}
	else
	{
		sub_rtable = NIL;		/* not used, but keep compiler quiet */
		sub_namespace = NIL;
	}

	/*
	 * Must get write lock on INSERT target table before scanning SELECT, else
	 * we will grab the wrong kind of initial lock if the target table is also
	 * mentioned in the SELECT part.  Note that the target table is not added
	 * to the joinlist or namespace.
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, ACL_INSERT);
#ifdef ADB
	if(pstate->p_grammar == PARSE_GRAM_ORACLE)
		addRTEtoQuery(pstate, rt_fetch(qry->resultRelation, pstate->p_rtable), false, true, true);
#endif /* ADB */

	/* Validate stmt->cols list, or build default list if no list given */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * Determine which variant of INSERT we have.
	 */
	if (selectStmt == NULL)
	{
		/*
		 * We have INSERT ... DEFAULT VALUES.  We can handle this case by
		 * emitting an empty targetlist --- all columns will be defaulted when
		 * the planner expands the targetlist.
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * We make the sub-pstate a child of the outer pstate so that it can
		 * see any Param definitions supplied from above.  Since the outer
		 * pstate's rtable and namespace are presently empty, there are no
		 * side-effects of exposing names the sub-SELECT shouldn't be able to
		 * see.
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;
#ifdef PGXC
		RangeTblEntry	*target_rte;
#endif

		/*
		 * Process the source SELECT.
		 *
		 * It is important that this be handled just like a standalone SELECT;
		 * otherwise the behavior of SELECT within INSERT might be different
		 * from a stand-alone SELECT. (Indeed, Postgres up through 6.5 had
		 * bugs of just that nature...)
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_joinexprs = NIL;	/* sub_rtable has no joins */
		sub_pstate->p_namespace = sub_namespace;

		selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

		free_parsestate(sub_pstate);

		/* The grammar should have produced a SELECT */
		if (!IsA(selectQuery, Query) ||
			selectQuery->commandType != CMD_SELECT ||
			selectQuery->utilityStmt != NULL)
			elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");

		/*
		 * Make the source be a subquery in the INSERT's rangetable, and add
		 * it to the INSERT's joinlist.
		 */
		rte = addRangeTableEntryForSubquery(pstate,
											selectQuery,
											makeAlias("*SELECT*", NIL),
											false,
											false);
#ifdef PGXC
		/*
		 * For an INSERT SELECT involving INSERT on a child after scanning
		 * the parent, set flag to send command ID communication to remote
		 * nodes in order to maintain global data visibility.
		 */
		if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
		{
			target_rte = rt_fetch(qry->resultRelation, pstate->p_rtable);
			if (is_relation_child(target_rte, selectQuery->rtable))
			{
				qry->has_to_save_cmd_id = true;
				SetSendCommandId(true);
			}
		}
#endif
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

		/*----------
		 * Generate an expression list for the INSERT that selects all the
		 * non-resjunk columns from the subquery.  (INSERT's tlist must be
		 * separate from the subquery's tlist because we may add columns,
		 * insert datatype coercions, etc.)
		 *
		 * HACK: unknown-type constants and params in the SELECT's targetlist
		 * are copied up as-is rather than being referenced as subquery
		 * outputs.  This is to ensure that when we try to coerce them to
		 * the target column's datatype, the right things happen (see
		 * special cases in coerce_type).  Otherwise, this fails:
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) ||IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
			{
				Var		   *var = makeVarFromTargetEntry(rtr->rtindex, tle);

				var->location = exprLocation((Node *) tle->expr);
				expr = (Expr *) var;
			}
			exprList = lappend(exprList, expr);
		}

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * Process INSERT ... VALUES with multiple VALUES sublists. We
		 * generate a VALUES RTE holding the transformed expression lists, and
		 * build up a targetlist containing Vars that reference the VALUES
		 * RTE.
		 */
		List	   *exprsLists = NIL;
		List	   *collations = NIL;
		int			sublist_length = -1;
		bool		lateral = false;
		int			i;

		Assert(selectStmt->intoClause == NULL);

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/* Do basic expression transformation (same as a ROW() expr) */
			sublist = transformExpressionList(pstate, sublist, EXPR_KIND_VALUES);

			/*
			 * All the sublists must be the same length, *after*
			 * transformation (which might expand '*' into multiple items).
			 * The VALUES RTE can't handle anything different.
			 */
			if (sublist_length < 0)
			{
				/* Remember post-transformation length of first sublist */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length"),
						 parser_errposition(pstate,
											exprLocation((Node *) sublist))));
			}

			/* Prepare row for assignment to target table */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos);

			/*
			 * We must assign collations now because assign_query_collations
			 * doesn't process rangetable entries.  We just assign all the
			 * collations independently in each row, and don't worry about
			 * whether they are consistent vertically.  The outer INSERT query
			 * isn't going to care about the collations of the VALUES columns,
			 * so it's not worth the effort to identify a common collation for
			 * each one here.  (But note this does have one user-visible
			 * consequence: INSERT ... VALUES won't complain about conflicting
			 * explicit COLLATEs in a column, whereas the same VALUES
			 * construct in another context would complain.)
			 */
			assign_list_collations(pstate, sublist);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * Although we don't really need collation info, let's just make sure
		 * we provide a correctly-sized list in the VALUES RTE.
		 */
		for (i = 0; i < sublist_length; i++)
			collations = lappend_oid(collations, InvalidOid);

		/*
		 * Ordinarily there can't be any current-level Vars in the expression
		 * lists, because the namespace was empty ... but if we're inside
		 * CREATE RULE, then NEW/OLD references might appear.  In that case we
		 * have to mark the VALUES RTE as LATERAL.
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			lateral = true;

		/*
		 * Generate the VALUES RTE
		 */
		rte = addRangeTableEntryForValues(pstate, exprsLists, collations,
										  NULL, lateral, true);
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

		/*
		 * Generate list of Vars referencing the RTE
		 */
		expandRTE(rte, rtr->rtindex, 0, -1, false, NULL, &exprList);
	}
	else
	{
		/*
		 * Process INSERT ... VALUES with a single VALUES sublist.  We treat
		 * this case separately for efficiency.  The sublist is just computed
		 * directly as the Query's targetlist, with no VALUES RTE.  So it
		 * works just like a SELECT without any FROM.
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);
		Assert(selectStmt->intoClause == NULL);

		/* Do basic expression transformation (same as a ROW() expr) */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists),
										   EXPR_KIND_VALUES);

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos);
	}

	/*
	 * Generate query's target list using the computed list of expressions.
	 * Also, mark all the target columns as needing insert permissions.
	 */
	rte = pstate->p_target_rangetblentry;
	qry->targetList = NIL;
	icols = list_head(icolumns);
	attnos = list_head(attrnos);
	foreach(lc, exprList)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col;
		AttrNumber	attr_num;
		TargetEntry *tle;

		col = (ResTarget *) lfirst(icols);
		Assert(IsA(col, ResTarget));
		attr_num = (AttrNumber) lfirst_int(attnos);

		tle = makeTargetEntry(expr,
							  attr_num,
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		rte->modifiedCols = bms_add_member(rte->modifiedCols,
							  attr_num - FirstLowInvalidHeapAttributeNumber);

		icols = lnext(icols);
		attnos = lnext(attnos);
	}

	/*
	 * If we have a RETURNING clause, we need to add the target relation to
	 * the query namespace before processing it, so that Var references in
	 * RETURNING will work.  Also, remove any namespace entries added in a
	 * sub-SELECT or VALUES list.
	 */
	if (stmt->returningList)
	{
		pstate->p_namespace = NIL;
		addRTEtoQuery(pstate, pstate->p_target_rangetblentry,
					  false, true, true);
		qry->returningList = transformReturningList(pstate,
													stmt->returningList);
	}

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * Prepare an INSERT row for assignment to the target table.
 *
 * The row might be either a VALUES row, or variables referencing a
 * sub-SELECT output.
 */
static List *
transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos)
{
	List	   *result;
	ListCell   *lc;
	ListCell   *icols;
	ListCell   *attnos;

	/*
	 * Check length of expr list.  It must not have more expressions than
	 * there are target columns.  We allow fewer, but only if no explicit
	 * columns list was given (the remaining columns are implicitly
	 * defaulted).  Note we must check this *after* transformation because
	 * that could expand '*' into multiple items.
	 */
	if (list_length(exprlist) > list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more expressions than target columns"),
				 parser_errposition(pstate,
									exprLocation(list_nth(exprlist,
												  list_length(icolumns))))));
	if (stmtcols != NIL &&
		list_length(exprlist) < list_length(icolumns))
	{
		/*
		 * We can get here for cases like INSERT ... SELECT (a,b,c) FROM ...
		 * where the user accidentally created a RowExpr instead of separate
		 * columns.  Add a suitable hint if that seems to be the problem,
		 * because the main error message is quite misleading for this case.
		 * (If there's no stmtcols, you'll get something about data type
		 * mismatch, which is less misleading so we don't worry about giving a
		 * hint in that case.)
		 */
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more target columns than expressions"),
				 ((list_length(exprlist) == 1 &&
				   count_rowexpr_columns(pstate, linitial(exprlist)) ==
				   list_length(icolumns)) ?
				  errhint("The insertion source is a row expression containing the same number of columns expected by the INSERT. Did you accidentally use extra parentheses?") : 0),
				 parser_errposition(pstate,
									exprLocation(list_nth(icolumns,
												  list_length(exprlist))))));
	}

	/*
	 * Prepare columns for assignment to target table.
	 */
	result = NIL;
	icols = list_head(icolumns);
	attnos = list_head(attrnos);
	foreach(lc, exprlist)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col;

		col = (ResTarget *) lfirst(icols);
		Assert(IsA(col, ResTarget));

		expr = transformAssignedExpr(pstate, expr,
									 EXPR_KIND_INSERT_TARGET,
									 col->name,
									 lfirst_int(attnos),
									 col->indirection,
									 col->location);

		result = lappend(result, expr);

		icols = lnext(icols);
		attnos = lnext(attnos);
	}

	return result;
}

/*
 * count_rowexpr_columns -
 *	  get number of columns contained in a ROW() expression;
 *	  return -1 if expression isn't a RowExpr or a Var referencing one.
 *
 * This is currently used only for hint purposes, so we aren't terribly
 * tense about recognizing all possible cases.  The Var case is interesting
 * because that's what we'll get in the INSERT ... SELECT (...) case.
 */
static int
count_rowexpr_columns(ParseState *pstate, Node *expr)
{
	if (expr == NULL)
		return -1;
	if (IsA(expr, RowExpr))
		return list_length(((RowExpr *) expr)->args);
	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;
		AttrNumber	attnum = var->varattno;

		if (attnum > 0 && var->vartype == RECORDOID)
		{
			RangeTblEntry *rte;

			rte = GetRTEByRangeTablePosn(pstate, var->varno, var->varlevelsup);
			if (rte->rtekind == RTE_SUBQUERY)
			{
				/* Subselect-in-FROM: examine sub-select's output expr */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					return -1;
				expr = (Node *) ste->expr;
				if (IsA(expr, RowExpr))
					return list_length(((RowExpr *) expr)->args);
			}
		}
	}
	return -1;
}


/*
 * transformSelectStmt -
 *	  transforms a Select Statement
 *
 * Note: this covers only cases with no set operations and no VALUES lists;
 * see below for the other cases.
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* Complain if we get called from someplace where INTO is not allowed */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
								  exprLocation((Node *) stmt->intoClause))));

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	pstate->p_locking_clause = stmt->lockingClause;

	/* make WINDOW info available for window functions, too */
	pstate->p_windowdefs = stmt->windowClause;

	/* process the FROM clause */
	transformFromClause(pstate, stmt->fromClause);

#ifdef ADB
	/* transform WHERE */
	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	qual = transformFromAndWhere(pstate, qual);
#endif /* ADB */

	/* transform targetlist */
	qry->targetList = transformTargetList(pstate, stmt->targetList,
										  EXPR_KIND_SELECT_TARGET);

	/* mark column origins */
	markTargetListOrigins(pstate, qry->targetList);

#ifndef ADB
	/* transform WHERE */
	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");
#endif /* ndef ADB */

	/* initial processing of HAVING clause is much like WHERE clause */
	qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  true /* fix unknowns */ ,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* allow SQL92 rules */ );

	if (stmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(stmt->distinctClause) == NULL)
	{
		/* We had SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														stmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* transform LIMIT */
	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET");
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT");

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs || qry->groupClause || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformValuesClause -
 *	  transforms a VALUES clause that's being used as a standalone SELECT
 *
 * We build a Query containing a VALUES RTE, rather as if one had written
 *			SELECT * FROM (VALUES ...) AS "*VALUES*"
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	List	   *exprsLists;
	List	   *collations;
	List	  **colexprs = NULL;
	int			sublist_length = -1;
	bool		lateral = false;
	RangeTblEntry *rte;
	int			rtindex;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	qry->commandType = CMD_SELECT;

	/* Most SELECT stuff doesn't apply in a VALUES clause */
	Assert(stmt->distinctClause == NIL);
	Assert(stmt->intoClause == NULL);
	Assert(stmt->targetList == NIL);
	Assert(stmt->fromClause == NIL);
	Assert(stmt->whereClause == NULL);
	Assert(stmt->groupClause == NIL);
	Assert(stmt->havingClause == NULL);
	Assert(stmt->windowClause == NIL);
	Assert(stmt->op == SETOP_NONE);

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * For each row of VALUES, transform the raw expressions.  This is also a
	 * handy place to reject DEFAULT nodes, which the grammar allows for
	 * simplicity.
	 *
	 * Note that the intermediate representation we build is column-organized
	 * not row-organized.  That simplifies the type and collation processing
	 * below.
	 */
	foreach(lc, stmt->valuesLists)
	{
		List	   *sublist = (List *) lfirst(lc);

		/* Do basic expression transformation (same as a ROW() expr) */
		sublist = transformExpressionList(pstate, sublist, EXPR_KIND_VALUES);

		/*
		 * All the sublists must be the same length, *after* transformation
		 * (which might expand '*' into multiple items).  The VALUES RTE can't
		 * handle anything different.
		 */
		if (sublist_length < 0)
		{
			/* Remember post-transformation length of first sublist */
			sublist_length = list_length(sublist);
			/* and allocate array for per-column lists */
			colexprs = (List **) palloc0(sublist_length * sizeof(List *));
		}
		else if (sublist_length != list_length(sublist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("VALUES lists must all be the same length"),
					 parser_errposition(pstate,
										exprLocation((Node *) sublist))));
		}

		/* Check for DEFAULT and build per-column expression lists */
		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("DEFAULT can only appear in a VALUES list within INSERT"),
						 parser_errposition(pstate, exprLocation(col))));
			colexprs[i] = lappend(colexprs[i], col);
			i++;
		}

		/* Release sub-list's cells to save memory */
		list_free(sublist);
	}

	/*
	 * Now resolve the common types of the columns, and coerce everything to
	 * those types.  Then identify the common collation, if any, of each
	 * column.
	 *
	 * We must do collation processing now because (1) assign_query_collations
	 * doesn't process rangetable entries, and (2) we need to label the VALUES
	 * RTE with column collations for use in the outer query.  We don't
	 * consider conflict of implicit collations to be an error here; instead
	 * the column will just show InvalidOid as its collation, and you'll get a
	 * failure later if that results in failure to resolve a collation.
	 *
	 * Note we modify the per-column expression lists in-place.
	 */
	collations = NIL;
	for (i = 0; i < sublist_length; i++)
	{
		Oid			coltype;
		Oid			colcoll;

		coltype = select_common_type(pstate, colexprs[i], "VALUES", NULL);

		foreach(lc, colexprs[i])
		{
			Node	   *col = (Node *) lfirst(lc);

			col = coerce_to_common_type(pstate, col, coltype, "VALUES");
			lfirst(lc) = (void *) col;
		}

		colcoll = select_common_collation(pstate, colexprs[i], true);

		collations = lappend_oid(collations, colcoll);
	}

#ifdef ADB
	/* fix: Array access (from variable 'colexprs') results in a null pointer
	 * dereference
	 */
	AssertArg(colexprs);
#endif
	/*
	 * Finally, rearrange the coerced expressions into row-organized lists.
	 */
	exprsLists = NIL;
	foreach(lc, colexprs[0])
	{
		Node	   *col = (Node *) lfirst(lc);
		List	   *sublist;

		sublist = list_make1(col);
		exprsLists = lappend(exprsLists, sublist);
	}
	list_free(colexprs[0]);
	for (i = 1; i < sublist_length; i++)
	{
		forboth(lc, colexprs[i], lc2, exprsLists)
		{
			Node	   *col = (Node *) lfirst(lc);
			List	   *sublist = lfirst(lc2);

			/* sublist pointer in exprsLists won't need adjustment */
			(void) lappend(sublist, col);
		}
		list_free(colexprs[i]);
	}

	/*
	 * Ordinarily there can't be any current-level Vars in the expression
	 * lists, because the namespace was empty ... but if we're inside CREATE
	 * RULE, then NEW/OLD references might appear.  In that case we have to
	 * mark the VALUES RTE as LATERAL.
	 */
	if (pstate->p_rtable != NIL &&
		contain_vars_of_level((Node *) exprsLists, 0))
		lateral = true;

	/*
	 * Generate the VALUES RTE
	 */
	rte = addRangeTableEntryForValues(pstate, exprsLists, collations,
									  NULL, lateral, true);
	addRTEtoQuery(pstate, rte, true, true, true);

	/* assume new rte is at end */
	rtindex = list_length(pstate->p_rtable);
	Assert(rte == rt_fetch(rtindex, pstate->p_rtable));

	/*
	 * Generate a targetlist as though expanding "*"
	 */
	Assert(pstate->p_next_resno == 1);
	qry->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);

	/*
	 * The grammar allows attaching ORDER BY, LIMIT, and FOR UPDATE to a
	 * VALUES, so cope.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  true /* fix unknowns */ ,
										  false /* allow SQL92 rules */ );

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET");
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT");

	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s cannot be applied to VALUES",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformSetOperationStmt -
 *	  transforms a set-operations tree
 *
 * A set-operation tree is just a SELECT, but with UNION/INTERSECT/EXCEPT
 * structure to it.  We must transform each leaf SELECT and build up a top-
 * level Query that contains the leaf SELECTs as subqueries in its rangetable.
 * The tree of set operations is converted into the setOperations field of
 * the top-level Query.
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	WithClause *withClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *lcc,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_namespace;
	int			sv_rtable_length;
	RangeTblEntry *jrte;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * Find leftmost leaf SelectStmt.  We currently only need to do this in
	 * order to deliver a suitable error message if there's an INTO clause
	 * there, implying the set-op tree is in a context that doesn't allow
	 * INTO.  (transformSetOperationTree would throw error anyway, but it
	 * seems worth the trouble to throw a different error for non-leftmost
	 * INTO, so we produce that error in transformSetOperationTree.)
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
						exprLocation((Node *) leftmostSelect->intoClause))));

	/*
	 * We need to extract ORDER BY and other top-level clauses here and not
	 * let transformSetOperationTree() see them --- else it'll just recurse
	 * right back here!
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;
	withClause = stmt->withClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;
	stmt->withClause = NULL;

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(lockingClause))->strength))));

	/* Process the WITH clause independently of all else */
	if (withClause)
	{
		qry->hasRecursive = withClause->recursive;
		qry->cteList = transformWithClause(pstate, withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * Recursively transform the components of the tree.
	 */
	sostmt = (SetOperationStmt *) transformSetOperationTree(pstate, stmt,
															true,
															NULL);
	Assert(sostmt && IsA(sostmt, SetOperationStmt));
	qry->setOperations = (Node *) sostmt;

	/*
	 * Re-find leftmost SELECT (now it's a sub-query in rangetable)
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist for outer query using column names of
	 * leftmost select and common datatypes/collations of topmost set
	 * operation.  Also make lists of the dummy vars and their names for use
	 * in parsing ORDER BY.
	 *
	 * Note: we use leftmostRTI as the varno of the dummy variables. It
	 * shouldn't matter too much which RT index they have, as long as they
	 * have one that corresponds to a real RT entry; else funny things may
	 * happen when the tree is mashed by rule rewriting.
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	left_tlist = list_head(leftmostQuery->targetList);

	forthree(lct, sostmt->colTypes,
			 lcm, sostmt->colTypmods,
			 lcc, sostmt->colCollations)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		var = makeVar(leftmostRTI,
					  lefttle->resno,
					  colType,
					  colTypmod,
					  colCollation,
					  0);
		var->location = exprLocation((Node *) lefttle->expr);
		tle = makeTargetEntry((Expr *) var,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, var);
		targetnames = lappend(targetnames, makeString(colName));
		left_tlist = lnext(left_tlist);
	}

	/*
	 * As a first step towards supporting sort clauses that are expressions
	 * using the output columns, generate a namespace entry that makes the
	 * output columns visible.  A Join RTE node is handy for this, since we
	 * can easily control the Vars generated upon matches.
	 *
	 * Note: we don't yet do anything useful with such cases, but at least
	 * "ORDER BY upper(foo)" will draw the right error message rather than
	 * "foo not found".
	 */
	sv_rtable_length = list_length(pstate->p_rtable);

	jrte = addRangeTableEntryForJoin(pstate,
									 targetnames,
									 JOIN_INNER,
									 targetvars,
									 NULL,
									 false);

	sv_namespace = pstate->p_namespace;
	pstate->p_namespace = NIL;

	/* add jrte to column namespace only */
	addRTEtoQuery(pstate, jrte, false, false, true);

	/*
	 * For now, we don't support resjunk sort clauses on the output of a
	 * setOperation tree --- you can only use the SQL92-spec options of
	 * selecting an output column by name or number.  Enforce by checking that
	 * transformSortClause doesn't add any items to tlist.
	 */
	tllen = list_length(qry->targetList);

	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* no unknowns expected */ ,
										  false /* allow SQL92 rules */ );

	/* restore namespace, remove jrte from rtable */
	pstate->p_namespace = sv_namespace;
	pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
				 errdetail("Only result column names can be used, not expressions or functions."),
				 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
				 parser_errposition(pstate,
						   exprLocation(list_nth(qry->targetList, tllen)))));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											EXPR_KIND_OFFSET, "OFFSET");
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   EXPR_KIND_LIMIT, "LIMIT");

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs || qry->groupClause || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	foreach(l, lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformSetOperationTree
 *		Recursively transform leaves and internal nodes of a set-op tree
 *
 * In addition to returning the transformed node, if targetlist isn't NULL
 * then we return a list of its non-resjunk TargetEntry nodes.  For a leaf
 * set-op node these are the actual targetlist entries; otherwise they are
 * dummy entries created to carry the type, typmod, collation, and location
 * (for error messages) of each output column of the set-op node.  This info
 * is needed only during the internal recursion of this function, so outside
 * callers pass NULL for targetlist.  Note: the reason for passing the
 * actual targetlist entries of a leaf node is so that upper levels can
 * replace UNKNOWN Consts with properly-coerced constants.
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **targetlist)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/* Guard against stack overflow due to overly complex set-expressions */
	check_stack_depth();

	/*
	 * Validity-check both leaf and internal SELECTs for disallowed ops.
	 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
				 parser_errposition(pstate,
								  exprLocation((Node *) stmt->intoClause))));

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	/*
	 * If an internal node of a set-op tree has ORDER BY, LIMIT, FOR UPDATE,
	 * or WITH clauses attached, we need to treat it like a leaf node to
	 * generate an independent sub-Query tree.  Otherwise, it can be
	 * represented by a SetOperationStmt node underneath the parent Query.
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause || stmt->withClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* Process leaf SELECT */
		Query	   *selectQuery;
		char		selectName[32];
		RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY;
		RangeTblRef *rtr;
		ListCell   *tl;

		/*
		 * Transform SelectStmt into a Query.
		 *
		 * Note: previously transformed sub-queries don't affect the parsing
		 * of this sub-query, because they are not in the toplevel pstate's
		 * namespace list.
		 */
		selectQuery = parse_sub_analyze((Node *) stmt, pstate, NULL, false);

		/*
		 * Check for bogus references to Vars on the current query level (but
		 * upper-level references are okay). Normally this can't happen
		 * because the namespace will be empty, but it could happen if we are
		 * inside a rule.
		 */
		if (pstate->p_namespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
						 parser_errposition(pstate,
							 locate_var_of_level((Node *) selectQuery, 1))));
		}

		/*
		 * Extract a list of the non-junk TLEs for upper-level processing.
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach(tl, selectQuery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}

		/*
		 * Make the leaf query be a subquery in the top-level rangetable.
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		rte = addRangeTableEntryForSubquery(pstate,
											selectQuery,
											makeAlias(selectName, NIL),
											false,
											false);

		/*
		 * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
		 */
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		return (Node *) rtr;
	}
	else
	{
		/* Process an internal node (set operation node) */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *ltargetlist;
		List	   *rtargetlist;
		ListCell   *ltl;
		ListCell   *rtl;
		const char *context;

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * Recursively transform the left child node.
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg,
											 false,
											 &ltargetlist);

		/*
		 * If we are processing a recursive union query, now is the time to
		 * examine the non-recursive term's output columns and mark the
		 * containing CTE as having those result columns.  We should do this
		 * only at the topmost setop of the CTE, of course.
		 */
		if (isTopLevel &&
			pstate->p_parent_cte &&
			pstate->p_parent_cte->cterecursive)
			determineRecursiveColTypes(pstate, op->larg, ltargetlist);

		/*
		 * Recursively transform the right child node.
		 */
		op->rarg = transformSetOperationTree(pstate, stmt->rarg,
											 false,
											 &rtargetlist);

		/*
		 * Verify that the two children have the same number of non-junk
		 * columns, and determine the types of the merged output columns.
		 */
		if (list_length(ltargetlist) != list_length(rtargetlist))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("each %s query must have the same number of columns",
						context),
					 parser_errposition(pstate,
										exprLocation((Node *) rtargetlist))));

		if (targetlist)
			*targetlist = NIL;
		op->colTypes = NIL;
		op->colTypmods = NIL;
		op->colCollations = NIL;
		op->groupClauses = NIL;
		forboth(ltl, ltargetlist, rtl, rtargetlist)
		{
			TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
			TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
			Node	   *lcolnode = (Node *) ltle->expr;
			Node	   *rcolnode = (Node *) rtle->expr;
			Oid			lcoltype = exprType(lcolnode);
			Oid			rcoltype = exprType(rcolnode);
			int32		lcoltypmod = exprTypmod(lcolnode);
			int32		rcoltypmod = exprTypmod(rcolnode);
			Node	   *bestexpr;
			int			bestlocation;
			Oid			rescoltype;
			int32		rescoltypmod;
			Oid			rescolcoll;

			/* select common type, same as CASE et al */
			rescoltype = select_common_type(pstate,
											list_make2(lcolnode, rcolnode),
											context,
											&bestexpr);
			bestlocation = exprLocation(bestexpr);
			/* if same type and same typmod, use typmod; else default */
			if (lcoltype == rcoltype && lcoltypmod == rcoltypmod)
				rescoltypmod = lcoltypmod;
			else
				rescoltypmod = -1;

			/*
			 * Verify the coercions are actually possible.  If not, we'd fail
			 * later anyway, but we want to fail now while we have sufficient
			 * context to produce an error cursor position.
			 *
			 * For all non-UNKNOWN-type cases, we verify coercibility but we
			 * don't modify the child's expression, for fear of changing the
			 * child query's semantics.
			 *
			 * If a child expression is an UNKNOWN-type Const or Param, we
			 * want to replace it with the coerced expression.  This can only
			 * happen when the child is a leaf set-op node.  It's safe to
			 * replace the expression because if the child query's semantics
			 * depended on the type of this output column, it'd have already
			 * coerced the UNKNOWN to something else.  We want to do this
			 * because (a) we want to verify that a Const is valid for the
			 * target type, or resolve the actual type of an UNKNOWN Param,
			 * and (b) we want to avoid unnecessary discrepancies between the
			 * output type of the child query and the resolved target type.
			 * Such a discrepancy would disable optimization in the planner.
			 *
			 * If it's some other UNKNOWN-type node, eg a Var, we do nothing
			 * (knowing that coerce_to_common_type would fail).  The planner
			 * is sometimes able to fold an UNKNOWN Var to a constant before
			 * it has to coerce the type, so failing now would just break
			 * cases that might work.
			 */
			if (lcoltype != UNKNOWNOID)
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
			else if (IsA(lcolnode, Const) ||
					 IsA(lcolnode, Param))
			{
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
				ltle->expr = (Expr *) lcolnode;
			}

			if (rcoltype != UNKNOWNOID)
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
			else if (IsA(rcolnode, Const) ||
					 IsA(rcolnode, Param))
			{
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
				rtle->expr = (Expr *) rcolnode;
			}

			/*
			 * Select common collation.  A common collation is required for
			 * all set operators except UNION ALL; see SQL:2008 7.13 <query
			 * expression> Syntax Rule 15c.  (If we fail to identify a common
			 * collation for a UNION ALL column, the curCollations element
			 * will be set to InvalidOid, which may result in a runtime error
			 * if something at a higher query level wants to use the column's
			 * collation.)
			 */
			rescolcoll = select_common_collation(pstate,
											  list_make2(lcolnode, rcolnode),
										 (op->op == SETOP_UNION && op->all));

			/* emit results */
			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
			op->colCollations = lappend_oid(op->colCollations, rescolcoll);

			/*
			 * For all cases except UNION ALL, identify the grouping operators
			 * (and, if available, sorting operators) that will be used to
			 * eliminate duplicates.
			 */
			if (op->op != SETOP_UNION || !op->all)
			{
				SortGroupClause *grpcl = makeNode(SortGroupClause);
				Oid			sortop;
				Oid			eqop;
				bool		hashable;
				ParseCallbackState pcbstate;

				setup_parser_errposition_callback(&pcbstate, pstate,
												  bestlocation);

				/* determine the eqop and optional sortop */
				get_sort_group_operators(rescoltype,
										 false, true, false,
										 &sortop, &eqop, NULL,
										 &hashable);

				cancel_parser_errposition_callback(&pcbstate);

				/* we don't have a tlist yet, so can't assign sortgrouprefs */
				grpcl->tleSortGroupRef = 0;
				grpcl->eqop = eqop;
				grpcl->sortop = sortop;
				grpcl->nulls_first = false;		/* OK with or without sortop */
				grpcl->hashable = hashable;

				op->groupClauses = lappend(op->groupClauses, grpcl);
			}

			/*
			 * Construct a dummy tlist entry to return.  We use a SetToDefault
			 * node for the expression, since it carries exactly the fields
			 * needed, but any other expression node type would do as well.
			 */
			if (targetlist)
			{
				SetToDefault *rescolnode = makeNode(SetToDefault);
				TargetEntry *restle;

				rescolnode->typeId = rescoltype;
				rescolnode->typeMod = rescoltypmod;
				rescolnode->collation = rescolcoll;
				rescolnode->location = bestlocation;
				restle = makeTargetEntry((Expr *) rescolnode,
										 0,		/* no need to set resno */
										 NULL,
										 false);
				*targetlist = lappend(*targetlist, restle);
			}
		}

		return (Node *) op;
	}
}

/*
 * Process the outputs of the non-recursive term of a recursive union
 * to set up the parent CTE's columns
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist)
{
	Node	   *node;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	List	   *targetList;
	ListCell   *left_tlist;
	ListCell   *nrtl;
	int			next_resno;

	/*
	 * Find leftmost leaf SELECT
	 */
	node = larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist using column names of leftmost select and
	 * dummy result expressions of the non-recursive term.
	 */
	targetList = NIL;
	left_tlist = list_head(leftmostQuery->targetList);
	next_resno = 1;

	foreach(nrtl, nrtargetlist)
	{
		TargetEntry *nrtle = (TargetEntry *) lfirst(nrtl);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		tle = makeTargetEntry(nrtle->expr,
							  next_resno++,
							  colName,
							  false);
		targetList = lappend(targetList, tle);
		left_tlist = lnext(left_tlist);
	}

	/* Now build CTE's output column info using dummy targetlist */
	analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}


/*
 * transformUpdateStmt -
 *	  transforms an update statement
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	RangeTblEntry *target_rte;
	Node	   *qual;
	ListCell   *origTargetList;
	ListCell   *tl;

	qry->commandType = CMD_UPDATE;
	pstate->p_is_update = true;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
#ifdef PGXC
		/*
		 * For a WITH query that updates a table in the main query and
		 * inserts a row in the same table in the WITH query set flag
		 * to send command ID communication to remote nodes in order to
		 * maintain global data visibility.
		 * For example
		 * CREATE TEMP TABLE tab (id int,val text) DISTRIBUTE BY REPLICATION;
		 * INSERT INTO tab VALUES (1,'p1');
		 * WITH wcte AS (INSERT INTO tab VALUES(42,'new') RETURNING id AS newid)
		 * UPDATE tab SET id = id + newid FROM wcte;
		 * The last query gets translated into the following multi-statement
		 * transaction on the primary datanode
		 * (a) START TRANSACTION ISOLATION LEVEL read committed READ WRITE
		 * (b) INSERT INTO tab (id, val) VALUES ($1, $2) RETURNING id -- (42,'new)'
		 * (c) SELECT id, val, ctid FROM ONLY tab WHERE true
		 * (d) UPDATE ONLY tab tab SET id = $1 WHERE (tab.ctid = $3) -- (43,(0,1)]
		 * (e) COMMIT TRANSACTION
		 * The command id of the select in step (c), should be such that
		 * it does not see the insert of step (b)
		 */
		if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
		{
			foreach(tl, stmt->withClause->ctes)
			{
				CommonTableExpr *cte = (CommonTableExpr *) lfirst(tl);
				if (IsA(cte->ctequery, InsertStmt))
				{
					qry->has_to_save_cmd_id = true;
					SetSendCommandId(true);
					break;
				}
			}
		}
#endif

		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->resultRelation = setTargetTable(pstate, stmt->relation,
								  interpretInhOption(stmt->relation->inhOpt),
										 true,
										 ACL_UPDATE);

	/* grab the namespace item made by setTargetTable */
	nsitem = (ParseNamespaceItem *) llast(pstate->p_namespace);

	/* subqueries in FROM cannot access the result relation */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * the FROM clause is non-standard SQL syntax. We used to be able to do
	 * this with REPLACE in POSTQUEL so we keep the feature.
	 */
	transformFromClause(pstate, stmt->fromClause);

	/* remaining clauses can reference the result relation normally */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qry->targetList = transformTargetList(pstate, stmt->targetList,
										  EXPR_KIND_UPDATE_SOURCE);

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	/*
	 * Now we are done with SELECT-like processing, and can get on with
	 * transforming the target list to match the UPDATE target columns.
	 */

	/* Prepare to assign non-conflicting resnos to resjunk attributes */
	if (pstate->p_next_resno <= pstate->p_target_relation->rd_rel->relnatts)
		pstate->p_next_resno = pstate->p_target_relation->rd_rel->relnatts + 1;

	/* Prepare non-junk columns for assignment to target table */
	target_rte = pstate->p_target_rangetblentry;
	origTargetList = list_head(stmt->targetList);

	foreach(tl, qry->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		ResTarget  *origTarget;
		int			attrno;

		if (tle->resjunk)
		{
			/*
			 * Resjunk nodes need no additional processing, but be sure they
			 * have resnos that do not match any target columns; else rewriter
			 * or planner might get confused.  They don't need a resname
			 * either.
			 */
			tle->resno = (AttrNumber) pstate->p_next_resno++;
			tle->resname = NULL;
			continue;
		}
		if (origTargetList == NULL)
			elog(ERROR, "UPDATE target count mismatch --- internal error");
		origTarget = (ResTarget *) lfirst(origTargetList);
		Assert(IsA(origTarget, ResTarget));

		attrno = attnameAttNum(pstate->p_target_relation,
							   origTarget->name, true);
		if (attrno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							origTarget->name,
						 RelationGetRelationName(pstate->p_target_relation)),
					 parser_errposition(pstate, origTarget->location)));

		updateTargetListEntry(pstate, tle, origTarget->name,
							  attrno,
							  origTarget->indirection,
							  origTarget->location);

		/* Mark the target column as requiring update permissions */
		target_rte->modifiedCols = bms_add_member(target_rte->modifiedCols,
								attrno - FirstLowInvalidHeapAttributeNumber);

		origTargetList = lnext(origTargetList);
	}
	if (origTargetList != NULL)
		elog(ERROR, "UPDATE target count mismatch --- internal error");

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformReturningList -
 *	handle a RETURNING clause in INSERT/UPDATE/DELETE
 */
static List *
transformReturningList(ParseState *pstate, List *returningList)
{
	List	   *rlist;
	int			save_next_resno;

	if (returningList == NIL)
		return NIL;				/* nothing to do */

	/*
	 * We need to assign resnos starting at one in the RETURNING list. Save
	 * and restore the main tlist's value of p_next_resno, just in case
	 * someone looks at it later (probably won't happen).
	 */
	save_next_resno = pstate->p_next_resno;
	pstate->p_next_resno = 1;

	/* transform RETURNING identically to a SELECT targetlist */
	rlist = transformTargetList(pstate, returningList, EXPR_KIND_RETURNING);

	/* mark column origins */
	markTargetListOrigins(pstate, rlist);

	/* restore state */
	pstate->p_next_resno = save_next_resno;

	return rlist;
}


/*
 * transformDeclareCursorStmt -
 *	transform a DECLARE CURSOR Statement
 *
 * DECLARE CURSOR is a hybrid case: it's an optimizable statement (in fact not
 * significantly different from a SELECT) as far as parsing/rewriting/planning
 * are concerned, but it's not passed to the executor and so in that sense is
 * a utility statement.  We transform it into a Query exactly as if it were
 * a SELECT, then stick the original DeclareCursorStmt into the utilityStmt
 * field to carry the cursor name and options.
 */
static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{
	Query	   *result;

	/*
	 * Don't allow both SCROLL and NO SCROLL to be specified
	 */
	if ((stmt->options & CURSOR_OPT_SCROLL) &&
		(stmt->options & CURSOR_OPT_NO_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("cannot specify both SCROLL and NO SCROLL")));

	result = transformStmt(pstate, stmt->query);

	/* Grammar should not have allowed anything but SELECT */
	if (!IsA(result, Query) ||
		result->commandType != CMD_SELECT ||
		result->utilityStmt != NULL)
		elog(ERROR, "unexpected non-SELECT command in DECLARE CURSOR");

	/*
	 * We also disallow data-modifying WITH in a cursor.  (This could be
	 * allowed, but the semantics of when the updates occur might be
	 * surprising.)
	 */
	if (result->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DECLARE CURSOR must not contain data-modifying statements in WITH")));

	/* FOR UPDATE and WITH HOLD are not compatible */
	if (result->rowMarks != NIL && (stmt->options & CURSOR_OPT_HOLD))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE CURSOR WITH HOLD ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(result->rowMarks))->strength)),
				 errdetail("Holdable cursors must be READ ONLY.")));

	/* FOR UPDATE and SCROLL are not compatible */
	if (result->rowMarks != NIL && (stmt->options & CURSOR_OPT_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE SCROLL CURSOR ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(result->rowMarks))->strength)),
				 errdetail("Scrollable cursors must be READ ONLY.")));

	/* FOR UPDATE and INSENSITIVE are not compatible */
	if (result->rowMarks != NIL && (stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE INSENSITIVE CURSOR ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(result->rowMarks))->strength)),
				 errdetail("Insensitive cursors must be READ ONLY.")));

	/* We won't need the raw querytree any more */
	stmt->query = NULL;

	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformExplainStmt -
 *	transform an EXPLAIN Statement
 *
 * EXPLAIN is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
static Query *
transformExplainStmt(ParseState *pstate, ExplainStmt *stmt)
{
	Query	   *result;

	/* transform contained query, allowing SELECT INTO */
	stmt->query = (Node *) transformTopLevelStmt(pstate, stmt->query);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformCreateTableAsStmt -
 *	transform a CREATE TABLE AS, SELECT ... INTO, or CREATE MATERIALIZED VIEW
 *	Statement
 *
 * As with EXPLAIN, transform the contained statement now.
 */
static Query *
transformCreateTableAsStmt(ParseState *pstate, CreateTableAsStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	/* transform contained query */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* additional work needed for CREATE MATERIALIZED VIEW */
	if (stmt->relkind == OBJECT_MATVIEW)
	{
		/*
		 * Prohibit a data-modifying CTE in the query used to create a
		 * materialized view. It's not sufficiently clear what the user would
		 * want to happen if the MV is refreshed or incrementally maintained.
		 */
		if (query->hasModifyingCTE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use data-modifying statements in WITH")));

		/*
		 * Check whether any temporary database objects are used in the
		 * creation query. It would be hard to refresh data or incrementally
		 * maintain it if a source disappeared.
		 */
		if (isQueryUsingTempRelation(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use temporary tables or views")));

		/*
		 * A materialized view would either need to save parameters for use in
		 * maintaining/loading the data or prohibit them entirely.  The latter
		 * seems safer and more sane.
		 */
		if (query_contains_extern_params(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views may not be defined using bound parameters")));

		/*
		 * For now, we disallow unlogged materialized views, because it seems
		 * like a bad idea for them to just go to empty after a crash. (If we
		 * could mark them as unpopulated, that would be better, but that
		 * requires catalog changes which crash recovery can't presently
		 * handle.)
		 */
		if (stmt->into->rel->relpersistence == RELPERSISTENCE_UNLOGGED)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views cannot be UNLOGGED")));

		/*
		 * At runtime, we'll need a copy of the parsed-but-not-rewritten Query
		 * for purposes of creating the view's ON SELECT rule.  We stash that
		 * in the IntoClause because that's where intorel_startup() can
		 * conveniently get it from.
		 */
		stmt->into->viewQuery = copyObject(query);
	}

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

#ifdef PGXC
/*
 * transformExecDirectStmt -
 *	transform an EXECUTE DIRECT Statement
 *
 * Handling is depends if we should execute on nodes or on Coordinator.
 * To execute on nodes we return CMD_UTILITY query having one T_RemoteQuery node
 * with the inner statement as a sql_command.
 * If statement is to run on Coordinator we should parse inner statement and
 * analyze resulting query tree.
 */
static Query *
transformExecDirectStmt(ParseState *pstate, ExecDirectStmt *stmt)
{
	Query		*result = makeNode(Query);
	char		*query = stmt->query;
	List		*nodelist = stmt->node_names;
	RemoteQuery	*step = makeNode(RemoteQuery);
	bool		is_local = false;
	List		*raw_parsetree_list;
	ListCell	*raw_parsetree_item;
	char		*nodename;
	Oid			nodeoid;
	int			nodeIndex;
	char		nodetype;

	/* Support not available on Datanodes */
	if (IS_PGXC_DATANODE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXECUTE DIRECT cannot be executed on a Datanode")));

	if (list_length(nodelist) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Support for EXECUTE DIRECT on multiple nodes is not available yet")));

	Assert(list_length(nodelist) == 1);
	Assert(IS_PGXC_COORDINATOR);

	/* There is a single element here */
	nodename = strVal(linitial(nodelist));
	nodeoid = get_pgxc_nodeoid(nodename);

	if (!OidIsValid(nodeoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("PGXC Node %s: object not defined",
						nodename)));

	/* Get node type and index */
	nodetype = get_pgxc_nodetype(nodeoid);
	nodeIndex = PGXCNodeGetNodeId(nodeoid, get_pgxc_nodetype(nodeoid));

	/* Check if node is requested is the self-node or not */
	if (nodetype == PGXC_NODE_COORDINATOR && nodeIndex == PGXCNodeId - 1)
		is_local = true;

	/* Transform the query into a raw parse list */
	raw_parsetree_list = pg_parse_query(query);

	/* EXECUTE DIRECT can just be executed with a single query */
	if (list_length(raw_parsetree_list) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXECUTE DIRECT cannot execute multiple queries")));

	/*
	 * Analyze the Raw parse tree
	 * EXECUTE DIRECT is restricted to one-step usage
	 */
	foreach(raw_parsetree_item, raw_parsetree_list)
	{
		Node   *parsetree = (Node *) lfirst(raw_parsetree_item);
		result = parse_analyze(parsetree, query, NULL, 0);
	}

	/* Needed by planner */
	result->sql_statement = pstrdup(query);

	/* Default list of parameters to set */
	step->sql_statement = NULL;
	step->exec_nodes = makeNode(ExecNodes);
	step->combine_type = COMBINE_TYPE_NONE;
	step->read_only = true;
	step->force_autocommit = false;
	step->cursor = NULL;

	/* This is needed by executor */
	step->sql_statement = pstrdup(query);
	if (nodetype == PGXC_NODE_COORDINATOR)
		step->exec_type = EXEC_ON_COORDS;
	else
		step->exec_type = EXEC_ON_DATANODES;

	step->base_tlist = NIL;

	/* Change the list of nodes that will be executed for the query and others */
	step->force_autocommit = false;
	step->combine_type = COMBINE_TYPE_SAME;
	step->read_only = true;
	step->exec_direct_type = EXEC_DIRECT_NONE;

	/* Set up EXECUTE DIRECT flag */
	if (is_local)
	{
		if (result->commandType == CMD_UTILITY)
			step->exec_direct_type = EXEC_DIRECT_LOCAL_UTILITY;
		else
			step->exec_direct_type = EXEC_DIRECT_LOCAL;
	}
	else
	{
		switch(result->commandType)
		{
			case CMD_UTILITY:
				step->exec_direct_type = EXEC_DIRECT_UTILITY;
				break;
			case CMD_SELECT:
				step->exec_direct_type = EXEC_DIRECT_SELECT;
				break;
			case CMD_INSERT:
				step->exec_direct_type = EXEC_DIRECT_INSERT;
				break;
			case CMD_UPDATE:
				step->exec_direct_type = EXEC_DIRECT_UPDATE;
				break;
			case CMD_DELETE:
				step->exec_direct_type = EXEC_DIRECT_DELETE;
				break;
			default:
				Assert(0);
		}
	}

	/*
	 * Features not yet supported
	 * DML can be launched without errors but this could compromise data
	 * consistency, so block it.
	 */
	if (!xc_maintenance_mode && (step->exec_direct_type == EXEC_DIRECT_DELETE
								 || step->exec_direct_type == EXEC_DIRECT_UPDATE
								 || step->exec_direct_type == EXEC_DIRECT_INSERT))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXECUTE DIRECT cannot execute DML queries")));
	else if (step->exec_direct_type == EXEC_DIRECT_UTILITY &&
			 !IsExecDirectUtilityStmt(result->utilityStmt) && !xc_maintenance_mode)
	{
		/* In case this statement is an utility, check if it is authorized */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXECUTE DIRECT cannot execute this utility query")));
	}
	else if (step->exec_direct_type == EXEC_DIRECT_LOCAL_UTILITY && !xc_maintenance_mode)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXECUTE DIRECT cannot execute locally this utility query")));
	}

	/* Build Execute Node list, there is a unique node for the time being */
	step->exec_nodes->nodeList = lappend_int(step->exec_nodes->nodeList, nodeIndex);

	/* Associate newly-created RemoteQuery node to the returned Query result */
	result->is_local = is_local;
	if (!is_local)
		result->utilityStmt = (Node *) step;

	return result;
}

/*
 * Check if given node is authorized to go through EXECUTE DURECT
 */
static bool
IsExecDirectUtilityStmt(Node *node)
{
	bool res = true;

	if (!node)
		return res;

	switch(nodeTag(node))
	{
		/*
		 * CREATE/DROP TABLESPACE are authorized to control
		 * tablespace at single node level.
		 */
		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
			res = true;
			break;
		default:
			res = false;
			break;
	}

	return res;
}

/*
 * Returns whether or not the rtable (and its subqueries)
 * contain any relation who is the parent of
 * the passed relation
 */
static bool
is_relation_child(RangeTblEntry *child_rte, List *rtable)
{
	ListCell *item;

	if (child_rte == NULL || rtable == NULL)
		return false;

	if (child_rte->rtekind != RTE_RELATION)
		return false;

	foreach(item, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(item);

		if (rte->rtekind == RTE_RELATION)
		{
			if (is_rel_child_of_rel(child_rte, rte))
				return true;
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			return is_relation_child(child_rte, rte->subquery->rtable);
		}
	}
	return false;
}

/*
 * Returns whether the passed RTEs have a parent child relationship
 */
static bool
is_rel_child_of_rel(RangeTblEntry *child_rte, RangeTblEntry *parent_rte)
{
	Oid		parentOID;
	bool		res;
	Relation	relation;
	SysScanDesc	scan;
	ScanKeyData	key[1];
	HeapTuple	inheritsTuple;
	Oid		inhrelid;

	/* Does parent RT entry allow inheritance? */
	if (!parent_rte->inh)
		return false;

	/* Ignore any already-expanded UNION ALL nodes */
	if (parent_rte->rtekind != RTE_RELATION)
		return false;

	/* Fast path for common case of childless table */
	parentOID = parent_rte->relid;
	if (!has_subclass(parentOID))
		return false;

	/* Assume we did not find any match */
	res = false;

	/* Scan pg_inherits and get all the subclass OIDs one by one. */
	relation = heap_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_inherits_inhparent, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(parentOID));
	scan = systable_beginscan(relation, InheritsParentIndexId, true, SnapshotNow, 1, key);

	while ((inheritsTuple = systable_getnext(scan)) != NULL)
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;

		/* Did we find the Oid of the passed RTE in one of the children? */
		if (child_rte->relid == inhrelid)
		{
			res = true;
			break;
		}
	}

	systable_endscan(scan);
	heap_close(relation, AccessShareLock);
	return res;
}

#endif

char *
LCS_asString(LockClauseStrength strength)
{
	switch (strength)
	{
		case LCS_FORKEYSHARE:
			return "FOR KEY SHARE";
		case LCS_FORSHARE:
			return "FOR SHARE";
		case LCS_FORNOKEYUPDATE:
			return "FOR NO KEY UPDATE";
		case LCS_FORUPDATE:
			return "FOR UPDATE";
	}
	return "FOR some";	/* shouldn't happen */
}

/*
 * Check for features that are not supported with FOR [KEY] UPDATE/SHARE.
 *
 * exported so planner can check again after rewriting, query pullup, etc
 */
void
CheckSelectLocking(Query *qry, LockClauseStrength strength)
{
	if (qry->setOperations)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(strength))));
	if (qry->distinctClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with DISTINCT clause",
						LCS_asString(strength))));
	if (qry->groupClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with GROUP BY clause",
						LCS_asString(strength))));
	if (qry->havingQual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with HAVING clause",
						LCS_asString(strength))));
	if (qry->hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with aggregate functions",
						LCS_asString(strength))));
	if (qry->hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with window functions",
						LCS_asString(strength))));
	if (expression_returns_set((Node *) qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /*------
				   translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with set-returning functions in the target list",
						LCS_asString(strength))));
}

/*
 * Transform a FOR [KEY] UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c, and isLockedRefname() in parse_relation.c.
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
					   bool pushedDown)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry, lc->strength);

	/* make a clause we can pass down to subqueries to select all rels */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* indicates all rels */
	allrels->strength = lc->strength;
	allrels->noWait = lc->noWait;

	if (lockedRels == NIL)
	{
		/* all regular tables used in query */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					applyLockingClause(qry, i,
									   lc->strength, lc->noWait, pushedDown);
					rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					break;
				case RTE_SUBQUERY:
					applyLockingClause(qry, i,
									   lc->strength, lc->noWait, pushedDown);

					/*
					 * FOR UPDATE/SHARE of subquery is propagated to all of
					 * subquery's rels, too.  We could do this later (based on
					 * the marking of the subquery RTE) but it is convenient
					 * to have local knowledge in each query level about which
					 * rels need to be opened with RowShareLock.
					 */
					transformLockingClause(pstate, rte->subquery,
										   allrels, true);
					break;
				default:
					/* ignore JOIN, SPECIAL, FUNCTION, VALUES, CTE RTEs */
					break;
			}
		}
	}
	else
	{
		/* just the named tables */
		foreach(l, lockedRels)
		{
			RangeVar   *thisrel = (RangeVar *) lfirst(l);

			/* For simplicity we insist on unqualified alias names here */
			if (thisrel->catalogname || thisrel->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 /*------
						   translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("%s must specify unqualified relation names",
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

				++i;
				if (strcmp(rte->eref->aliasname, thisrel->relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							applyLockingClause(qry, i,
											   lc->strength, lc->noWait,
											   pushedDown);
							rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							break;
						case RTE_SUBQUERY:
							applyLockingClause(qry, i,
											   lc->strength, lc->noWait,
											   pushedDown);
							/* see comment above */
							transformLockingClause(pstate, rte->subquery,
												   allrels, true);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 /*------
									   translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a join",
											LCS_asString(lc->strength)),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 /*------
									   translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a function",
											LCS_asString(lc->strength)),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 /*------
									   translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to VALUES",
											LCS_asString(lc->strength)),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_CTE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 /*------
									   translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a WITH query",
											LCS_asString(lc->strength)),
							 parser_errposition(pstate, thisrel->location)));
							break;
						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* out of foreach loop */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 /*------
						   translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("relation \"%s\" in %s clause not found in FROM clause",
								thisrel->relname,
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));
		}
	}
}

/*
 * Record locking info for a single rangetable item
 */
void
applyLockingClause(Query *qry, Index rtindex,
				   LockClauseStrength strength, bool noWait, bool pushedDown)
{
	RowMarkClause *rc;

	/* If it's an explicit clause, make sure hasForUpdate gets set */
	if (!pushedDown)
		qry->hasForUpdate = true;

	/* Check for pre-existing entry for same rtindex */
	if ((rc = get_parse_rowmark(qry, rtindex)) != NULL)
	{
		/*
		 * If the same RTE is specified for more than one locking strength,
		 * treat is as the strongest.  (Reasonable, since you can't take both
		 * a shared and exclusive lock at the same time; it'll end up being
		 * exclusive anyway.)
		 *
		 * We also consider that NOWAIT wins if it's specified both ways. This
		 * is a bit more debatable but raising an error doesn't seem helpful.
		 * (Consider for instance SELECT FOR UPDATE NOWAIT from a view that
		 * internally contains a plain FOR UPDATE spec.)
		 *
		 * And of course pushedDown becomes false if any clause is explicit.
		 */
		rc->strength = Max(rc->strength, strength);
		rc->noWait |= noWait;
		rc->pushedDown &= pushedDown;
		return;
	}

	/* Make a new RowMarkClause */
	rc = makeNode(RowMarkClause);
	rc->rti = rtindex;
	rc->strength = strength;
	rc->noWait = noWait;
	rc->pushedDown = pushedDown;
	qry->rowMarks = lappend(qry->rowMarks, rc);
}

#ifdef ADB
typedef struct JoinExprInfo
{
	Node		*expr;		/* join clause */
	JoinType	type;		/* */
	Index		lrtindex;
	Index		rrtindex;
	int			location;	/* of ColumnRefJoin location, or -1 */
}JoinExprInfo;

typedef struct GetOraColumnJoinContext
{
	ParseState *pstate;
	JoinExprInfo *info;
}GetOraColumnJoinContext;

typedef struct PullupRelForJoinContext
{
	Node *larg;
	Node *rarg;
}PullupRelForJoinContext;

static bool have_ora_column_join(Node *node, void *context)
{
	if(node == NULL)
	{
		return false;
	}else if(IsA(node, ColumnRefJoin))
	{
		return true;
	}
	return expression_tree_walker(node, have_ora_column_join, context);
}

static bool get_ora_column_join_walker(Node *node, GetOraColumnJoinContext *context)
{
	AssertArg(context && context->info && context->pstate);
	if(node == NULL)
	{
		return false;
	}else if(IsA(node, ColumnRefJoin))
	{
		ColumnRefJoin *crj = (ColumnRefJoin*)node;
		JoinExprInfo *info = context->info;
		Assert(crj->var != NULL && IsA(crj->var, Var));
		if(info->rrtindex == 0)
		{
			Assert(info->type == JOIN_INNER);
			info->type = JOIN_LEFT;
			info->rrtindex = crj->var->varno;
			info->location = crj->location;
		}else if(info->rrtindex != crj->var->varno)
		{
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
				,errmsg("a predicate may reference only one outer-joined table")
				,parser_errposition(context->pstate, crj->location)));
		}
		return false;
	}else if(IsA(node, Var))
	{
		Var *var = (Var*)node;
		JoinExprInfo *info = context->info;
		Assert(var->varno != 0);
		if(info->lrtindex == var->varno
			|| info->rrtindex == var->varno)
		{
			return false;
		}

		if(info->lrtindex == 0)
		{
			info->lrtindex = var->varno;
		}else if(info->rrtindex == 0
			&& info->type == JOIN_INNER)
		{
			info->rrtindex = var->varno;
		}else if(info->rrtindex != var->varno
			&& info->lrtindex != var->varno)
		{
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
				,errmsg("a predicate may reference only one outer-joined table")
				, parser_errposition(context->pstate, info->location)));
		}
		return false;
	}
	return expression_tree_walker(node, get_ora_column_join_walker, context);
}

static JoinExprInfo* get_ora_column_join(Node *expr, ParseState *pstate)
{
	GetOraColumnJoinContext context;
	JoinExprInfo *jinfo = palloc(sizeof(JoinExprInfo));

	jinfo->expr = expr;
	jinfo->type = JOIN_INNER;
	jinfo->rrtindex = jinfo->lrtindex = 0;
	context.info = jinfo;
	context.pstate = pstate;
	(void)get_ora_column_join_walker(expr, &context);

	return jinfo;
}

static Node* remove_column_join_expr(Node *node, void *context)
{
	if(node == NULL)
		return NULL;
	else if(IsA(node, ColumnRefJoin))
		return (Node*)((ColumnRefJoin*)node)->var;
	return expression_tree_mutator(node, remove_column_join_expr, context);
}

static bool combin_pullup_context(PullupRelForJoinContext *dest
	, PullupRelForJoinContext *src)
{
	bool res = false;
	if(src->larg != NULL)
	{
		Assert(dest->larg == NULL);
		dest->larg = src->larg;
		res = true;
	}
	if(src->rarg != NULL)
	{
		Assert(dest->rarg == NULL);
		dest->rarg = src->rarg;
		res = true;
	}
	return res;
}

static bool pullup_rel_for_join(Node *node, JoinExprInfo *jinfo
	, ParseState *pstate, PullupRelForJoinContext *context)
{
	AssertArg(node && jinfo && jinfo->expr && pstate);
	AssertArg(jinfo->lrtindex != 0 && jinfo->rrtindex != 0
		&& jinfo->lrtindex != jinfo->rrtindex
		&& (jinfo->type == JOIN_INNER || jinfo->type == JOIN_LEFT));

	if(IsA(node, FromExpr))
	{
		ListCell *lc;
		JoinExpr *join;
		Node *item;
		PullupRelForJoinContext my_context;
		FromExpr *from = (FromExpr*)node;

		for(lc=list_head(from->fromlist);lc;)
		{
			item = lfirst(lc);
			Assert(item);
			lc = lnext(lc);

			my_context.larg = my_context.rarg = NULL;
			if(pullup_rel_for_join(item, jinfo, pstate, &my_context))
			{
				Assert(context->larg == NULL && context->rarg == NULL);
				return true;
			}

			if(combin_pullup_context(context, &my_context))
			{
				from->fromlist = list_delete_ptr(from->fromlist, item);
				if(context->larg && context->rarg)
					break;
			}
		}

		/* return false when not found all */
		if(context->larg == NULL || context->rarg == NULL)
			return false;

		/* now meke JoinExpr */
		join = makeNode(JoinExpr);
		join->jointype = jinfo->type;
		join->larg = context->larg;
		join->rarg = context->rarg;
		join->quals = jinfo->expr;
		from->fromlist = lappend(from->fromlist, join);
		return true;
	}else if(IsA(node, JoinExpr))
	{
		PullupRelForJoinContext my_context;
		JoinExpr *join = (JoinExpr*)node;

		my_context.larg = my_context.rarg = NULL;
		if(pullup_rel_for_join(join->larg, jinfo, pstate, &my_context))
		{
			Assert(context->larg == NULL && context->rarg == NULL);
			return true;
		}
		(void)combin_pullup_context(context, &my_context);

		if(context->larg == NULL || context->rarg == NULL)
		{
			my_context.larg = my_context.rarg = NULL;
			if(pullup_rel_for_join(join->rarg, jinfo, pstate, &my_context))
			{
				Assert(context->larg == NULL && context->rarg == NULL);
				return true;
			}
			(void)combin_pullup_context(context, &my_context);
		}

		if(context->larg != NULL && context->rarg != NULL)
		{
			/*
			 * all table found
			 * combin clause
			 */
			BoolExpr *bexpr;
			if(jinfo->type == JOIN_LEFT
				&& join->jointype != JOIN_LEFT
				&& join->jointype != JOIN_RIGHT)
			{
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					, errmsg("a predicate may reference only one outer-joined table")
					, parser_errposition(pstate, jinfo->location)));
			}

			if(join->quals == NULL)
			{
				bexpr = (BoolExpr*)makeBoolExpr(AND_EXPR, NIL, -1);
				join->quals = (Node*)bexpr;
			}else if(and_clause(join->quals))
			{
				bexpr = (BoolExpr*)(join->quals);
			}else
			{
				bexpr = (BoolExpr*)makeBoolExpr(AND_EXPR, list_make1(join->quals), -1);
				join->quals = (Node*)bexpr;
			}
			bexpr->args = lappend(bexpr->args, jinfo->expr);
			return true;
		}

		/* release [lr]arg to this join node */
		if(context->larg != NULL)
			context->larg = node;
		else if(context->rarg != NULL)
			context->rarg = node;

		return false;
	}else if(IsA(node, RangeTblRef))
	{
		RangeTblRef *rte = (RangeTblRef*)node;
		if(rte->rtindex == jinfo->lrtindex)
		{
			Assert(context->larg == NULL);
			context->larg = node;
		}else if(rte->rtindex == jinfo->rrtindex)
		{
			Assert(context->rarg == NULL);
			context->rarg = node;
		}
		return false;
	}else
	{
		ereport(ERROR,
			(errmsg("unrecognized node type: %d", (int)nodeTag(node))));
	}
	return false;
}

static void check_joinon_column_join(Node *node, ParseState *pstate)
{
	if(node == NULL)
		return;

	switch(nodeTag(node))
	{
	case T_JoinExpr:
		{
			JoinExprInfo *jinfo;
			JoinExpr *join = (JoinExpr*)node;
			Assert(join->larg && join->rarg);
			check_joinon_column_join(join->larg, pstate);
			check_joinon_column_join(join->rarg, pstate);

			if(have_ora_column_join(join->quals, NULL) == false)
				return;

			jinfo = get_ora_column_join(join->quals, pstate);
			Assert(jinfo);

			if(jinfo->type != JOIN_INNER)
			{
				RangeTblRef *rte;
				bool failed = false;
				if(IsA(join->larg, RangeTblRef))
				{
					rte = (RangeTblRef*)(join->larg);
					if(rte->rtindex == jinfo->lrtindex && join->jointype != JOIN_LEFT)
						failed = true;
					if(rte->rtindex == jinfo->rrtindex && join->jointype != JOIN_RIGHT)
						failed = true;
				}
				if(IsA(join->rarg, RangeTblRef))
				{
					rte = (RangeTblRef*)(join->rarg);
					if(rte->rtindex == jinfo->lrtindex && join->jointype != JOIN_RIGHT)
						failed = true;
					if(rte->rtindex == jinfo->rrtindex && join->jointype != JOIN_LEFT)
						failed = true;
				}
				if(failed)
					ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
						,errmsg("a predicate may reference only on outer-joined table")
						,parser_errposition(pstate, jinfo->location)));
			}
			join->quals = remove_column_join_expr(join->quals, NULL);
		}
		break;
	case T_FromExpr:
		{
			ListCell *lc;
			FromExpr *from = (FromExpr*)node;
			foreach(lc, from->fromlist)
				check_joinon_column_join(lfirst(lc), pstate);
			if(have_ora_column_join(from->quals, NULL))
				from->quals = remove_column_join_expr(from->quals, NULL);
		}
		break;
	case T_RangeTblRef:
		break;
	default:
		elog(ERROR, "unrecognized node type: %d",(int) nodeTag(node));
	}
}

static List* find_namespace_item_for_rte(List *namespace, RangeTblEntry *rte)
{
	ListCell *lc;
	ParseNamespaceItem *pni = NULL;
	Assert(rte && namespace);
	foreach(lc, namespace)
	{
		pni = lfirst(lc);
		Assert(pni);
		if(pni->p_rte == rte)
			break;
	}
	Assert(pni->p_rte == rte);
	return list_make1(pni);
}

static void analyze_new_join(ParseState *pstate, Node *node, RangeTblEntry **top_rte
	, int *rtindex, List **namelist)
{
	Assert(pstate && node && top_rte && rtindex);
	if(IsA(node, JoinExpr))
	{
		JoinExpr *j = (JoinExpr*)node;
		if(j->rtindex == 0)
		{
			/* new join expr */
			ParseNamespaceItem *pni;
			ListCell *lc;
			List *res_colnames,
				 *colnames,
				 *res_colvars,
				 *colvars,
				 *res_namelist,
				 *arg_namelist;
			RangeTblEntry	*l_rte;
			RangeTblEntry	*r_rte;
			int				l_rtindex;
			int				r_rtindex;
			int				k;

			Assert(j->jointype == JOIN_INNER || j->jointype == JOIN_LEFT);
			analyze_new_join(pstate, j->larg, &l_rte, &l_rtindex, &res_namelist);
			expandRTE(l_rte, l_rtindex, 0, -1, false, &res_colnames, &res_colvars);

			analyze_new_join(pstate, j->rarg, &r_rte, &r_rtindex, &arg_namelist);
			expandRTE(r_rte, r_rtindex, 0, -1, false, &colnames, &colvars);

			Assert((checkNameSpaceConflicts(pstate, res_namelist, arg_namelist), true));
			res_colnames = list_concat(res_colnames, colnames);
			res_colvars = list_concat(res_colvars, colvars);
			res_namelist = list_concat(res_namelist, arg_namelist);

			*top_rte = addRangeTableEntryForJoin(pstate, res_colnames, j->jointype
						, res_colvars, j->alias, true);
			j->rtindex = list_length(pstate->p_rtable);
			Assert(*top_rte == rt_fetch(j->rtindex, pstate->p_rtable));
			*rtindex = j->rtindex;

			/* make a matching link to the JoinExpr for later use */
			for (k = list_length(pstate->p_joinexprs) + 1; k < j->rtindex; k++)
				pstate->p_joinexprs = lappend(pstate->p_joinexprs, NULL);
			pstate->p_joinexprs = lappend(pstate->p_joinexprs, j);
			Assert(list_length(pstate->p_joinexprs) == j->rtindex);

			foreach(lc, res_namelist)
			{
				pni = lfirst(lc);
				pni->p_cols_visible = false;
			}
			pni = palloc0(sizeof(*pni));
			pni->p_rte = *top_rte;
			Assert(j->alias == NULL);
			pni->p_rel_visible = false;
			pni->p_cols_visible = true;
			pni->p_lateral_only = false;
			pni->p_lateral_ok = true;
			*namelist = lappend(res_namelist, pni);
		}else
		{
			*top_rte = rt_fetch(j->rtindex, pstate->p_rtable);
			*namelist = find_namespace_item_for_rte(pstate->p_namespace, *top_rte);
			*rtindex = j->rtindex;
		}
	}else if(IsA(node, FromExpr))
	{
		RangeTblEntry *rte;
		List *my_namelist;
		ListCell *lc;
		FromExpr *from_expr = (FromExpr*)node;
		int my_rtindex;
		foreach(lc, from_expr->fromlist)
		{
			analyze_new_join(pstate, lfirst(lc), &rte, &my_rtindex, &my_namelist);
			Assert((checkNameSpaceConflicts(pstate, *namelist, my_namelist),true));
			*namelist = list_concat(*namelist, my_namelist);
		}
	}else if(IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef*)node;
		*top_rte = rt_fetch(rtr->rtindex, pstate->p_rtable);
		*namelist = find_namespace_item_for_rte(pstate->p_namespace, *top_rte);
		*rtindex = rtr->rtindex;
	}else
	{
		ereport(ERROR, (errmsg("unknown node type %d", nodeTag(node))));
	}
}

/*
 * return list of JoinExprInfo
 *
 * t1.id=t2.id(+) and t1.name=t2.name and t1.id>10
 *          |
 *          V
 * t1.id=t2.id(+)
 * t1.name=t2.name
 * t1.id>10
 *          |
 *          V
 * t1.id=t2.id(+) and t1.name=t2.name
 * t1.id>10
 */
static List* get_join_qual_exprs(Node *quals, ParseState *pstate)
{
	ListCell *lc;
	List *result;
	List *qual_list;
	Node *expr;
	JoinExprInfo *jinfo, *jinfo2;
	if(quals == NULL)
		return NIL;

	quals = (Node*)canonicalize_qual((Expr*)quals);
	if(and_clause(quals))
		qual_list = ((BoolExpr*)quals)->args;
	else
		qual_list = list_make1(quals);

	/*
	 * this loop, we get all column join expr clause
	 * and delete from qual_list
	 */
	result = NIL;
	for(lc=list_head(qual_list);lc;)
	{
		expr = lfirst(lc);
		lc = lnext(lc);
		if(have_ora_column_join(expr, NULL) == false)
			continue;

		jinfo = get_ora_column_join(expr, pstate);
		result = lappend(result, jinfo);
		qual_list = list_delete_ptr(qual_list, expr);
	}

	/*
	 * now, we combin exprs
	 */
	while(qual_list)
	{
		expr = linitial(qual_list);
		jinfo2 = get_ora_column_join(expr, pstate);

		foreach(lc, result)
		{
			jinfo = lfirst(lc);
			if(jinfo->type == jinfo2->type
				&& jinfo->lrtindex == jinfo2->lrtindex
				&& jinfo->rrtindex == jinfo2->rrtindex)
			{
				/* same table(s) clause, combin it */
				BoolExpr *bexpr;
				if(and_clause(jinfo->expr))
				{
					bexpr = (BoolExpr*)(jinfo->expr);
				}else
				{
					bexpr = (BoolExpr*)makeBoolExpr(AND_EXPR, list_make1(jinfo->expr), -1);
					jinfo->expr = (Node*)bexpr;
				}
				bexpr->args = lappend(bexpr->args, jinfo2->expr);
				pfree(jinfo2);
				goto next_qual_list;
			}
		}
		/* not match in result */
		result = lappend(result, jinfo2);
next_qual_list:
		qual_list = list_delete_first(qual_list);
	}

	return result;
}

/*
 *  from t1,t2,t3,t4
 *  where t1.id=t2.id(+)
 *    and t1.id(+)=t3.id
 *    and t1.id=t4.id(+);
 *    and other
 *         |
 *         V
 *  from (t1,t3,t4) left join t2 on t1.id=t2.id
 *  where t1.id(+)=t3.id
 *    and t1.id=t4.id(+)
 *    and other
 *         |
 *         V
 *  from ((t1 left join t3 on t1.id=t3.id),t4) left join t2 on t1.id=t2.id
 *  where t1.id=t4.id(+)
 *    and other
 *         |
 *         V
 *  from ((t1 left join t3 on t1.id=t3.id) left join t4 on t1.id=t4.id) left join t2 on t1.id=t2.id
 *  where other
 */
static Node* transformFromAndWhere(ParseState *pstate, Node *quals)
{
	List *qual_list,
		 *new_namelist;
	ListCell *lc;
	FromExpr *from;
	JoinExprInfo *jinfo;
	PullupRelForJoinContext context;

	if(pstate->p_joinlist == NIL
		|| have_ora_column_join(quals, NULL) == false)
		return quals;

	if(list_length(pstate->p_joinlist) == 1)
	{
		/* fast process */
		return remove_column_join_expr(quals, NULL);
	}

	qual_list = get_join_qual_exprs(quals, pstate);
	from = makeNode(FromExpr);
	from->fromlist = pstate->p_joinlist;

	for(lc=list_head(qual_list);lc!=NULL;)
	{
		ListCell *tmp = lc;
		jinfo = lfirst(lc);
		lc = lnext(lc);

		if(jinfo->lrtindex == 0
			|| jinfo->rrtindex == 0)
		{
			/* keep single table's clause and remove jinfo */
			lfirst(tmp) = remove_column_join_expr(jinfo->expr, NULL);
			pfree(jinfo);
			continue;
		}

		context.larg = context.rarg = NULL;
		jinfo->expr = remove_column_join_expr(jinfo->expr, NULL);
		if(pullup_rel_for_join((Node*)from, jinfo, pstate, &context) == false)
		{
			ereport(ERROR, (errmsg("move filter qual to join filter failed!")));
		}
		qual_list = list_delete_ptr(qual_list, jinfo);
		pfree(jinfo);
	}

	/* save namespace */
	Assert(pstate->p_save_namespace == NIL);
	foreach(lc, pstate->p_namespace)
	{
		ParseNamespaceItem *ni = palloc(sizeof(*ni));
		memcpy(ni, lfirst(lc), sizeof(*ni));
		pstate->p_save_namespace = lappend(pstate->p_save_namespace, ni);
	}

	{
		RangeTblEntry *rte;
		int rtindex;
		new_namelist = NIL;
		analyze_new_join(pstate, (Node*)from, &rte, &rtindex, &new_namelist);
	}
	pstate->p_namespace = new_namelist;

	pstate->p_joinlist = from->fromlist;
	pfree(from);

	if(qual_list == NIL)
	{
		return NULL;
	}else if(list_length(qual_list) == 1)
	{
		return linitial(qual_list);
	}
	return (Node*)makeBoolExpr(AND_EXPR, qual_list, -1);
}

static bool rewrite_rownum_query_enum(Node *node, void *context)
{
	if(node == NULL)
		return false;

	if(node_tree_walker(node,rewrite_rownum_query_enum, context))
		return true;
	if(IsA(node, Query))
	{
		rewrite_rownum_query((Query*)node);
	}
	return false;
}

/*
 * let "rownum <[=] CONST" or "CONST >[=] rownum"
 * to "limit N"
 * TODO: fix when "Const::consttypmod != -1"
 * TODO: fix when "rownum < 1 and rownum < 2" to "limit CASE WHEN 1<2 THEN 1 ELSE 2"
 */
static void rewrite_rownum_query(Query *query)
{
	List *qual_list,*args;
	ListCell *lc;
	Node *expr,*l,*r;
	Node *limitCount;
	Bitmapset *hints;
	char opname[4];
	Oid opno;
	Oid funcid;
	int i;
	int64 v64;

	Assert(query);
	if(query->jointree == NULL
		|| query->limitOffset != NULL
		|| query->limitCount != NULL
		|| contain_rownum(query->jointree->quals) == false)
		return;

	query->jointree->quals = expr = (Node*)canonicalize_qual((Expr*)(query->jointree->quals));
	if(and_clause((Node*)expr))
		qual_list = ((BoolExpr*)expr)->args;
	else
		qual_list = list_make1(expr);

	/* find expr */
	limitCount = NULL;
	hints = NULL;
	for(i=0,lc=list_head(qual_list);lc;lc=lnext(lc),++i)
	{
		expr = lfirst(lc);
		if(contain_rownum((Node*)expr) == false)
			continue;

		if(IsA(expr, OpExpr))
		{
			args = ((OpExpr*)expr)->args;
			opno = ((OpExpr*)expr)->opno;
			funcid = ((OpExpr*)expr)->opfuncid;
		}else if(IsA(expr, FuncExpr))
		{
			funcid = ((FuncExpr*)expr)->funcid;
			args = ((FuncExpr*)expr)->args;
			opno = InvalidOid;
		}else
		{
			return;
		}
		if(list_length(args) != 2)
			return;
		l = linitial(args);
		r = llast(args);
		Assert(l != NULL && r != NULL);
		if(!IsA(l,RownumExpr) && !IsA(r, RownumExpr))
			return;

		if(opno == InvalidOid)
		{
			/* get operator */
			Assert(OidIsValid(funcid));
			opno = get_operator_for_function(funcid);
			if(opno == InvalidOid)
				return;
		}

		if(IsA(r, RownumExpr))
		{
			/* exchange operator, like "10>rownum" to "rownum<10" */
			Node *tmp;
			opno = get_commutator(opno);
			if(opno == InvalidOid)
				return;
			tmp = l;
			l = r;
			r = tmp;
		}

		if(!IsA(l, RownumExpr))
			return;
		/* get operator name */
		{
			char *tmp = get_opname(opno);
			if(tmp == NULL)
				return;
			strncpy(opname, tmp, lengthof(opname));
			pfree(tmp);
		}

		if(opname[0] == '<')
		{
			if(contain_mutable_functions((Node*)r))
				return;

			if(const_get_int64((Expr*)r, &v64) == false)
				return;
			if(opname[1] == '=' && opname[2] == '\0')
			{
				/* rownum <= expr */
				if(v64 <= (int64)0)
				{
					/* rownum <= n, and (n<=0) */
					limitCount = (Node*)make_int8_const(Int64GetDatum(0));
					qual_list = NIL;
					break;
				}
				if(limitCount != NULL)
					return; /* has other operator */
				limitCount = r;
			}else if(opname[1] == '\0')
			{
				if(v64 <= (int64)1)
				{
					/* rownum < n, and (n<=1) */
					limitCount = (Node*)make_int8_const(Int64GetDatum(0));
					qual_list = NIL;
					break;
				}
				if(limitCount != NULL)
					return; /* has other operator */
				limitCount = (Node*)make_op2(NULL
					, SystemFuncName("-")
					, (Node*)r, (Node*)make_int8_const(Int64GetDatum(1))
					, -1, true);
				if(limitCount == NULL)
					return;
			}else if(opname[1] == '>' && opname[2] == '\0')
			{
				/* rownum <> expr */
				if(v64 <= (int64)0)
				{
					/* rownum <> n, and (n <= 0) ignore */
				}else if(limitCount != NULL)
				{
					return; /* has other operator */
				}else
				{
					/* for now, rownum <> n equal limit n-1 */
					limitCount = (Node*)make_op2(NULL
						, SystemFuncName("-")
						, (Node*)r, (Node*)make_int8_const(Int64GetDatum(1))
						, -1, true);
					if(limitCount == NULL)
						return;
				}
			}else
			{
				return; /* unknown operator */
			}
		}else if(opname[0] == '>')
		{
			if(const_get_int64((Expr*)r, &v64) == false)
				return;

			if(opname[1] == '=' && opname[2] == '\0')
			{
				/* rownum >= expr
				 *  only support rownum >= 1
				 */
				if(v64 != (int64)1)
					return;
			}else if(opname[1] == '\0')
			{
				/* rownum > expr
				 *  only support rownum > 0
				 */
				if(v64 != (int64)0)
					return;
			}else
			{
				return;
			}
		}else if(opname[0] == '=' && opname[1] == '\0')
		{
			if(!IsA(r, RownumExpr))
				return;
			/* rownum = rownum ignore */
		}else
		{
			return;
		}

		hints = bms_add_member(hints, i);
	}

	query->limitCount = limitCount;
	if(qual_list != NIL)
	{
		/* whe use args for get new quals */
		args = NIL;
		for(i=0,lc=list_head(qual_list);lc;lc=lnext(lc),++i)
		{
			if(bms_is_member(i, hints))
				continue;
			Assert(contain_rownum(lfirst(lc)) == false);
			args = lappend(args, lfirst(lc));
		}
		if(args == NIL)
		{
			query->jointree->quals = NULL;
		}else if(list_length(args) == 1)
		{
			query->jointree->quals = linitial(args);
		}else
		{
			query->jointree->quals = (Node*)makeBoolExpr(AND_EXPR, args, -1);
		}
	}
	return;
}

static Expr* make_int8_const(Datum value)
{
	Const *result;
	result = makeNode(Const);
	result->consttype = INT8OID;
	result->consttypmod = -1;
	result->constcollid = InvalidOid;
	result->constlen = sizeof(int64);
	result->constvalue = value;
	result->constisnull = false;
	result->constbyval = FLOAT8PASSBYVAL;
	result->location = -1;
	return (Expr*)result;
}

/*
 * we should be find a cast and call it
 */
static bool const_get_int64(const Expr *expr, int64 *val)
{
	Const *c;
	AssertArg(expr && val);
	if(!IsA(expr, Const))
		return false;
	c = (Const*)expr;
	if(c->constisnull)
		return false;
	switch(c->consttype)
	{
	case INT8OID:
		*val = DatumGetInt64(c->constvalue);
		return true;
	case INT4OID:
		*val = (int64)(DatumGetInt32(c->constvalue));
		return true;
	case INT2OID:
		*val = (int64)(DatumGetInt16(c->constvalue));
		return true;
	default:
		break;
	}
	return false;
}

static Oid get_operator_for_function(Oid funcid)
{
	Relation rel;
	HeapScanDesc scanDesc;
	HeapTuple	htup;
	ScanKeyData scanKeyData;
	Oid opno;

	if(funcid == InvalidOid)
		return InvalidOid;

	ScanKeyInit(&scanKeyData, Anum_pg_operator_oprcode
		, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(funcid));
	rel = heap_open(OperatorRelationId, AccessShareLock);
	scanDesc = heap_beginscan(rel, SnapshotNow, 1, &scanKeyData);
	htup = heap_getnext(scanDesc, ForwardScanDirection);
	if(HeapTupleIsValid(htup))
	{
		opno = HeapTupleGetOid(htup);
	}else
	{
		opno = InvalidOid;
	}
	heap_endscan(scanDesc);
	heap_close(rel, AccessShareLock);
	return opno;
}

#endif /* ADB */

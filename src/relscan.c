/*
 * relscan.c
 *
 * Common routines related to relation scan
 * ----
 * Copyright 2011-2019 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2019 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"

/* Data structure for collecting qual clauses that match an index */
typedef struct
{
	bool		nonempty;		/* True if lists are not all empty */
	/* Lists of RestrictInfos, one per index column */
	List	   *indexclauses[INDEX_MAX_KEYS];
} IndexClauseSet;

/*--- static variables ---*/
static bool		pgstrom_enable_brin;

#if PG_VERSION_NUM < 100000
/* several BRIN-index stuff are not implemented at PG9.6 */
#include "access/brin_page.h"

/*
 * BrinStatsData represents stats data for planner use
 */
typedef struct BrinStatsData
{
	BlockNumber	pagesPerRange;
	BlockNumber	revmapNumPages;
} BrinStatsData;

/*
 * Fetch index's statistical data into *stats
 */
static void
brinGetStats(Relation index, BrinStatsData *stats)
{
	Buffer		metabuffer;
	Page		metapage;
	BrinMetaPageData *metadata;

	metabuffer = ReadBuffer(index, BRIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = (BrinMetaPageData *) PageGetContents(metapage);

	stats->pagesPerRange = metadata->pagesPerRange;
	stats->revmapNumPages = metadata->lastRevmapPage - 1;

	UnlockReleaseBuffer(metabuffer);
}
#endif	/* <PG10.x */

/*
 * simple_match_clause_to_indexcol
 *
 * It is a simplified version of match_clause_to_indexcol.
 * Also see optimizer/path/indxpath.c
 */
static bool
simple_match_clause_to_indexcol(IndexOptInfo *index,
								int indexcol,
								RestrictInfo *rinfo)
{
	Expr	   *clause = rinfo->clause;
	Index		index_relid = index->rel->relid;
	Oid			opfamily = index->opfamily[indexcol];
	Oid			idxcollation = index->indexcollations[indexcol];
	Node	   *leftop;
	Node	   *rightop;
	Relids		left_relids;
	Relids		right_relids;
	Oid			expr_op;
	Oid			expr_coll;

	/* Clause must be a binary opclause */
	if (!is_opclause(clause))
		return false;

	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return false;
	left_relids = rinfo->left_relids;
	right_relids = rinfo->right_relids;
	expr_op = ((OpExpr *) clause)->opno;
	expr_coll = ((OpExpr *) clause)->inputcollid;

	if (OidIsValid(idxcollation) && idxcollation != expr_coll)
		return false;

	/*
	 * Check for clauses of the form:
	 *    (indexkey operator constant) OR
	 *    (constant operator indexkey)
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, right_relids) &&
		!contain_volatile_functions(rightop) &&
		op_in_opfamily(expr_op, opfamily))
		return true;

	if (match_index_to_operand(rightop, indexcol, index) &&
		!bms_is_member(index_relid, left_relids) &&
		!contain_volatile_functions(leftop) &&
		op_in_opfamily(get_commutator(expr_op), opfamily))
		return true;

	return false;
}

/*
 * simple_match_clause_to_index
 *
 * It is a simplified version of match_clause_to_index.
 * Also see optimizer/path/indxpath.c
 */
static void
simple_match_clause_to_index(IndexOptInfo *index,
							 RestrictInfo *rinfo,
							 IndexClauseSet *clauseset)
{
	int		indexcol;

    /*
     * Never match pseudoconstants to indexes.  (Normally a match could not
     * happen anyway, since a pseudoconstant clause couldn't contain a Var,
     * but what if someone builds an expression index on a constant? It's not
     * totally unreasonable to do so with a partial index, either.)
     */
    if (rinfo->pseudoconstant)
        return;

#if PG_VERSION_NUM >= 100000
    /*
     * If clause can't be used as an indexqual because it must wait till after
     * some lower-security-level restriction clause, reject it.
     */
    if (!restriction_is_securely_promotable(rinfo, index->rel))
        return;
#endif

	/* OK, check each index column for a match */
	for (indexcol = 0; indexcol < index->ncolumns; indexcol++)
	{
		if (simple_match_clause_to_indexcol(index,
											indexcol,
											rinfo))
		{
			clauseset->indexclauses[indexcol] =
				list_append_unique_ptr(clauseset->indexclauses[indexcol],
									   rinfo);
			clauseset->nonempty = true;
			break;
		}
	}
}

/*
 * estimate_brinindex_scan_nblocks
 *
 * Also see brincostestimate at utils/adt/selfuncs.c
 */
static cl_long
estimate_brinindex_scan_nblocks(PlannerInfo *root,
                                RelOptInfo *baserel,
								IndexOptInfo *index,
								IndexClauseSet *clauseset,
								List **p_indexQuals)
{
	Relation		indexRel;
	BrinStatsData	statsData;
	List		   *indexQuals = NIL;
	ListCell	   *lc		__attribute__((unused));
	int				icol	__attribute__((unused));
	Selectivity		qualSelectivity;
	Selectivity		indexSelectivity;
	double			indexCorrelation = 0.0;
	double			indexRanges;
	double			minimalRanges;
	double			estimatedRanges;

	/* Obtain some data from the index itself. */
	indexRel = index_open(index->indexoid, AccessShareLock);
	brinGetStats(indexRel, &statsData);
	index_close(indexRel, AccessShareLock);

#if PG_VERSION_NUM >= 100000
	/* Get selectivity of the index qualifiers */
	icol = 1;
	foreach (lc, index->indextlist)
	{
		TargetEntry *tle = lfirst(lc);
		ListCell   *cell;
		VariableStatData vardata;

		foreach (cell, clauseset->indexclauses[icol-1])
		{
			RestrictInfo *rinfo = lfirst(cell);

			indexQuals = lappend(indexQuals, rinfo);
		}

		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;
			RangeTblEntry *rte;

			/* in case of BRIN index on simple column */
			rte = root->simple_rte_array[var->varno];
			if (get_relation_stats_hook &&
				(*get_relation_stats_hook)(root, rte, var->varattno,
										   &vardata))
			{
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR, "no callback to release stats variable");
			}
			else
			{
				vardata.statsTuple =
					SearchSysCache3(STATRELATTINH,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(var->varattno),
									BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}
		else
		{
			if (get_index_stats_hook &&
				(*get_index_stats_hook)(root, index->indexoid, icol,
										&vardata))
			{
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR, "no callback to release stats variable");
			}
			else
			{
				vardata.statsTuple
					= SearchSysCache3(STATRELATTINH,
									  ObjectIdGetDatum(index->indexoid),
									  Int16GetDatum(icol),
									  BoolGetDatum(false));
                vardata.freefunc = ReleaseSysCache;
			}
		}

		if (HeapTupleIsValid(vardata.statsTuple))
		{
			AttStatsSlot	sslot;

			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_CORRELATION,
								 InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				double		varCorrelation = 0.0;

				if (sslot.nnumbers > 0)
					varCorrelation = Abs(sslot.numbers[0]);

				if (varCorrelation > indexCorrelation)
					indexCorrelation = varCorrelation;

				free_attstatsslot(&sslot);
			}
		}
		ReleaseVariableStats(vardata);

		icol++;
	}
#else
	indexCorrelation = 1.0;
#endif
	qualSelectivity = clauselist_selectivity(root,
											 indexQuals,
											 baserel->relid,
											 JOIN_INNER,
											 NULL);

	/* estimate number of blocks to read */
	indexRanges = ceil((double) baserel->pages / statsData.pagesPerRange);
	if (indexRanges < 1.0)
		indexRanges = 1.0;
	minimalRanges = ceil(indexRanges * qualSelectivity);

	//elog(INFO, "strom: qualSelectivity=%.6f indexRanges=%.6f minimalRanges=%.6f indexCorrelation=%.6f", qualSelectivity, indexRanges, minimalRanges, indexCorrelation);

	if (indexCorrelation < 1.0e-10)
		estimatedRanges = indexRanges;
	else
		estimatedRanges = Min(minimalRanges / indexCorrelation, indexRanges);

	indexSelectivity = estimatedRanges / indexRanges;
	if (indexSelectivity < 0.0)
		indexSelectivity = 0.0;
	if (indexSelectivity > 1.0)
		indexSelectivity = 1.0;

	/* index quals, if any */
	if (p_indexQuals)
		*p_indexQuals = indexQuals;
	/* estimated number of blocks to read */
	return (cl_long)(indexSelectivity * (double) baserel->pages);
}

/*
 * extract_index_conditions
 */
static Node *
__fixup_indexqual_operand(Node *node, IndexOptInfo *indexOpt)
{
	ListCell   *lc;

	if (!node)
		return NULL;

	if (IsA(node, RelabelType))
	{
		RelabelType *relabel = (RelabelType *) node;

		return __fixup_indexqual_operand((Node *)relabel->arg, indexOpt);
	}

	foreach (lc, indexOpt->indextlist)
	{
		TargetEntry *tle = lfirst(lc);

		if (equal(node, tle->expr))
		{
			return (Node *)makeVar(INDEX_VAR,
								   tle->resno,
								   exprType((Node *)tle->expr),
								   exprTypmod((Node *) tle->expr),
								   exprCollation((Node *) tle->expr),
								   0);
		}
	}
	if (IsA(node, Var))
		elog(ERROR, "Bug? variable is not found at index tlist");
	return expression_tree_mutator(node, __fixup_indexqual_operand, indexOpt);
}

static List *
extract_index_conditions(List *index_quals, IndexOptInfo *indexOpt)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach (lc, index_quals)
	{
		RestrictInfo *rinfo = lfirst(lc);
		OpExpr	   *op = (OpExpr *) rinfo->clause;

		if (!IsA(rinfo->clause, OpExpr))
			elog(ERROR, "Bug? unexpected index clause: %s",
				 nodeToString(rinfo->clause));
		if (list_length(((OpExpr *)rinfo->clause)->args) != 2)
			elog(ERROR, "indexqual clause must be binary opclause");
		op = (OpExpr *)copyObject(rinfo->clause);
		if (!bms_equal(rinfo->left_relids, indexOpt->rel->relids))
			CommuteOpExpr(op);
		/* replace the indexkey expression with an index Var */
		linitial(op->args) = __fixup_indexqual_operand(linitial(op->args),
													   indexOpt);
		result = lappend(result, op);
	}
	return result;
}

/*
 * pgstrom_tryfind_brinindex
 */
IndexOptInfo *
pgstrom_tryfind_brinindex(PlannerInfo *root,
						  RelOptInfo *baserel,
						  List **p_indexConds,
						  List **p_indexQuals,
						  cl_long *p_indexNBlocks)
{
	cl_long			indexNBlocks = LONG_MAX;
	IndexOptInfo   *indexOpt = NULL;
	List		   *indexQuals = NIL;
	ListCell	   *cell;

	/* skip if GUC disables BRIN-index */
	if (!pgstrom_enable_brin)
		return NULL;

	/* skip if no indexes */
	if (baserel->indexlist == NIL)
		return NULL;

	foreach (cell, baserel->indexlist)
	{
		IndexOptInfo   *index = (IndexOptInfo *) lfirst(cell);
		List		   *temp = NIL;
		ListCell	   *lc;
		cl_long			nblocks;
		IndexClauseSet	clauseset;

		/* Protect limited-size array in IndexClauseSets */
		Assert(index->ncolumns <= INDEX_MAX_KEYS);

		/* Ignore partial indexes that do not match the query. */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/* Only BRIN-indexes are now supported */
		if (index->relam != BRIN_AM_OID)
			continue;

		/* see match_clauses_to_index */
		memset(&clauseset, 0, sizeof(IndexClauseSet));
		foreach (lc, index->indrestrictinfo)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			simple_match_clause_to_index(index, rinfo, &clauseset);
		}
		if (!clauseset.nonempty)
			continue;

		/*
		 * In case when multiple BRIN-indexes are configured,
		 * the one with minimal selectivity is the best choice.
		 */
 		nblocks = estimate_brinindex_scan_nblocks(root, baserel,
												  index,
												  &clauseset,
												  &temp);
		if (indexNBlocks > nblocks)
		{
			indexOpt = index;
			indexQuals = temp;
			indexNBlocks = nblocks;
		}
	}

	if (indexOpt)
	{
		if (p_indexConds)
			*p_indexConds = extract_index_conditions(indexQuals, indexOpt);
		if (p_indexQuals)
			*p_indexQuals = indexQuals;
		if (p_indexNBlocks)
			*p_indexNBlocks = indexNBlocks;
	}
	return indexOpt;
}

/*
 * pgstrom_common_relscan_cost
 */
int
pgstrom_common_relscan_cost(PlannerInfo *root,
							RelOptInfo *scan_rel,
							List *scan_quals,
							int parallel_workers,
							IndexOptInfo *indexOpt,
							List *indexQuals,
							cl_long indexNBlocks,
							double *p_parallel_divisor,
							double *p_scan_ntuples,
							double *p_scan_nchunks,
							cl_uint *p_nrows_per_block,
							Cost *p_startup_cost,
							Cost *p_run_cost)
{
	int			scan_mode = PGSTROM_RELSCAN_NORMAL;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	Cost		index_scan_cost = 0.0;
	Cost		disk_scan_cost;
	double		gpu_ratio = pgstrom_gpu_operator_cost / cpu_operator_cost;
	double		parallel_divisor;
	double		ntuples = scan_rel->tuples;
	double		nblocks = scan_rel->pages;
	double		nchunks;
	double		selectivity;
	double		spc_seq_page_cost;
	double		spc_rand_page_cost;
	cl_uint		nrows_per_block = 0;
	Size		heap_size;
	Size		htup_size;
	QualCost	qcost;
	ListCell   *lc;

	Assert((scan_rel->reloptkind == RELOPT_BASEREL ||
			scan_rel->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
		   scan_rel->relid > 0 &&
		   scan_rel->relid < root->simple_rel_array_size);

	/* selectivity of device executable qualifiers */
	selectivity = clauselist_selectivity(root,
										 scan_quals,
										 scan_rel->relid,
										 JOIN_INNER,
										 NULL);
	/* cost of full-table scan, if no index */
	get_tablespace_page_costs(scan_rel->reltablespace,
							  &spc_rand_page_cost,
							  &spc_seq_page_cost);
	disk_scan_cost = spc_seq_page_cost * nblocks;

	/* consideration for BRIN-index, if any */
	if (indexOpt)
	{
		BrinStatsData	statsData;
		Relation		index_rel;
		Cost			x;

		index_rel = index_open(indexOpt->indexoid, AccessShareLock);
		brinGetStats(index_rel, &statsData);
		index_close(index_rel, AccessShareLock);

		get_tablespace_page_costs(indexOpt->reltablespace,
								  &spc_rand_page_cost,
								  &spc_seq_page_cost);
		index_scan_cost = spc_seq_page_cost * statsData.revmapNumPages;
		foreach (lc, indexQuals)
		{
			cost_qual_eval_node(&qcost, (Node *)lfirst(lc), root);
			index_scan_cost += qcost.startup + qcost.per_tuple;
		}

		x = index_scan_cost + spc_rand_page_cost * (double)indexNBlocks;
		if (disk_scan_cost > x)
		{
			disk_scan_cost = x;
			ntuples = scan_rel->tuples * ((double) indexNBlocks / nblocks);
			nblocks = indexNBlocks;
			scan_mode |= PGSTROM_RELSCAN_BRIN_INDEX;
		}
	}

	/* check whether NVMe-Strom is capable */
	if (ScanPathWillUseNvmeStrom(root, scan_rel))
		scan_mode |= PGSTROM_RELSCAN_SSD2GPU;

	/*
	 * Cost adjustment by CPU parallelism, if used.
	 * (overall logic is equivalent to cost_seqscan())
	 */
	if (parallel_workers > 0)
	{
		parallel_divisor = (double) parallel_workers;
#if PG_VERSION_NUM >= 110000
		if (parallel_leader_participation)
#endif
		{
			double		leader_contribution;

			leader_contribution = 1.0 - (0.3 * (double) parallel_workers);
			if (leader_contribution > 0)
				parallel_divisor += leader_contribution;
		}
		/* number of tuples to be actually processed */
		ntuples  = clamp_row_est(ntuples / parallel_divisor);

		/*
		 * After the v2.0, pg_strom.gpu_setup_cost represents the cost for
		 * run-time code build by NVRTC. Once binary is constructed, it can
		 * be shared with all the worker process, so we can discount the
		 * cost by parallel_divisor.
		 */
		startup_cost += pgstrom_gpu_setup_cost / 2
			+ (pgstrom_gpu_setup_cost / (2 * parallel_divisor));
	}
	else
	{
		parallel_divisor = 1.0;
		startup_cost += pgstrom_gpu_setup_cost;
	}
	/*
	 * Cost discount for more efficient I/O with multiplexing.
	 * PG background workers can issue read request to filesystem
	 * concurrently. It enables to work I/O subsystem during blocking-
	 * time for other workers, then, it pulls up usage ratio of the
	 * storage system.
	 */
	disk_scan_cost /= Min(2.0, sqrt(parallel_divisor));

	/* more disk i/o discount if NVMe-Strom is available */
	if ((scan_mode & PGSTROM_RELSCAN_SSD2GPU) != 0)
		disk_scan_cost /= 1.5;
	run_cost += disk_scan_cost;

	/*
	 * Rough estimation for number of chunks if KDS_FORMAT_ROW.
	 * Also note that we roughly assume KDS_HeadSz is BLCKSZ to
	 * reduce estimation cycle.
	 */
	heap_size = (double)(BLCKSZ - SizeOfPageHeaderData) * nblocks;
	htup_size = (MAXALIGN(offsetof(HeapTupleHeaderData,
								   t_bits[BITMAPLEN(scan_rel->max_attr)])) +
				 MAXALIGN(heap_size / Max(scan_rel->tuples, 1.0) -
						  sizeof(ItemIdData) - SizeofHeapTupleHeader));
	nchunks =  (((double)(offsetof(kern_tupitem, htup) + htup_size +
						  sizeof(cl_uint)) * Max(ntuples, 1.0)) /
				((double)(pgstrom_chunk_size() - BLCKSZ)));
	nchunks = Max(nchunks, 1);

	/*
	 * estimation of the tuple density per block - this logic follows
	 * the manner in estimate_rel_size()
	 */
	if (scan_rel->pages > 0)
		nrows_per_block = ceil(scan_rel->tuples / (double)scan_rel->pages);
	else
	{
		RangeTblEntry *rte = root->simple_rte_array[scan_rel->relid];
		size_t		tuple_width = get_relation_data_width(rte->relid, NULL);

		tuple_width += MAXALIGN(SizeofHeapTupleHeader);
		tuple_width += sizeof(ItemIdData);
		/* note: integer division is intentional here */
		nrows_per_block = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
	}

	/* Cost for GPU qualifiers */
	cost_qual_eval_node(&qcost, (Node *)scan_quals, root);
	startup_cost += qcost.startup;
	run_cost += qcost.per_tuple * gpu_ratio * ntuples;
	ntuples *= selectivity;

	/* Cost for DMA transfer (host/storage --> GPU) */
	run_cost += pgstrom_gpu_dma_cost * nchunks;

	*p_parallel_divisor = parallel_divisor;
	*p_scan_ntuples = ntuples / parallel_divisor;
	*p_scan_nchunks = nchunks / parallel_divisor;
	*p_nrows_per_block =
		((scan_mode & PGSTROM_RELSCAN_SSD2GPU) != 0 ? nrows_per_block : 0);
	*p_startup_cost = startup_cost;
	*p_run_cost = run_cost;

	return scan_mode;
}

/*
 * pgstromIndexState - runtime status of BRIN-index for relation scan
 */
typedef struct pgstromIndexState
{
	Oid			index_oid;
	Relation	index_rel;
	Node	   *index_quals;	/* for EXPLAIN */
	BlockNumber	nblocks;
	BlockNumber	range_sz;
	BrinRevmap *brin_revmap;
	BrinDesc   *brin_desc;
	ScanKey		scan_keys;
	int			num_scan_keys;
	IndexRuntimeKeyInfo *runtime_keys_info;
	int			num_runtime_keys;
	bool		runtime_key_ready;
	ExprContext *runtime_econtext;
} pgstromIndexState;

/*
 * pgstromExecInitBrinIndexMap
 */
void
pgstromExecInitBrinIndexMap(GpuTaskState *gts,
							Oid index_oid,
							List *index_conds,
							List *index_quals)
{
	pgstromIndexState *pi_state = NULL;
	Relation	relation = gts->css.ss.ss_currentRelation;
	EState	   *estate = gts->css.ss.ps.state;
	Index		scanrelid;
	LOCKMODE	lockmode = NoLock;

	if (!OidIsValid(index_oid))
	{
		Assert(index_conds == NIL);
		gts->outer_index_state = NULL;
		return;
	}
	Assert(relation != NULL);
	scanrelid = ((Scan *) gts->css.ss.ps.plan)->scanrelid;
	if (!ExecRelationIsTargetRelation(estate, scanrelid))
		lockmode = AccessShareLock;

	pi_state = palloc0(sizeof(pgstromIndexState));
	pi_state->index_oid = index_oid;
	pi_state->index_rel = index_open(index_oid, lockmode);
	pi_state->index_quals = (Node *)make_ands_explicit(index_quals);
	ExecIndexBuildScanKeys(&gts->css.ss.ps,
						   pi_state->index_rel,
						   index_conds,
						   false,
						   &pi_state->scan_keys,
						   &pi_state->num_scan_keys,
						   &pi_state->runtime_keys_info,
						   &pi_state->num_runtime_keys,
						   NULL,
						   NULL);

	/* ExprContext to evaluate runtime keys, if any */
	if (pi_state->num_runtime_keys != 0)
		pi_state->runtime_econtext = CreateExprContext(estate);
	else
		pi_state->runtime_econtext = NULL;

	/* BRIN index specific initialization */
	pi_state->nblocks = RelationGetNumberOfBlocks(relation);
	pi_state->brin_revmap = brinRevmapInitialize(pi_state->index_rel,
												 &pi_state->range_sz,
												 estate->es_snapshot);
	pi_state->brin_desc = brin_build_desc(pi_state->index_rel);

	/* save the state */
	gts->outer_index_state = pi_state;
}

/*
 * pgstromSizeOfBrinIndexMap
 */
Size
pgstromSizeOfBrinIndexMap(GpuTaskState *gts)
{
	pgstromIndexState *pi_state = gts->outer_index_state;
	int		nwords;

	if (!pi_state)
		return 0;

	nwords = (pi_state->nblocks +
			  pi_state->range_sz - 1) / pi_state->range_sz;
	return STROMALIGN(offsetof(Bitmapset, words) +
					  sizeof(bitmapword) * nwords);

}

/*
 * pgstromExecGetBrinIndexMap
 *
 * Also see bringetbitmap
 */
static void
__pgstromExecGetBrinIndexMap(pgstromIndexState *pi_state,
							 Bitmapset *brin_map,
							 Snapshot snapshot)
{
	BrinDesc	   *bdesc = pi_state->brin_desc;
	TupleDesc		bd_tupdesc = bdesc->bd_tupdesc;
	BlockNumber		nblocks = pi_state->nblocks;
	BlockNumber		range_sz = pi_state->range_sz;
	BlockNumber		heapBlk;
	BlockNumber		index;
	Buffer			buf = InvalidBuffer;
	FmgrInfo	   *consistentFn;
	BrinMemTuple   *dtup;
	BrinTuple	   *btup	__attribute__((unused)) = NULL;
	Size			btupsz	__attribute__((unused)) = 0;
	int				nranges;
	int				nwords;
	MemoryContext	oldcxt;
	MemoryContext	perRangeCxt;

	/* rooms for the consistent support procedures of indexed columns */
	consistentFn = palloc0(sizeof(FmgrInfo) * bd_tupdesc->natts);
	/* allocate an initial in-memory tuple */
	dtup = brin_new_memtuple(bdesc);

	/* moves to the working memory context per range */
	perRangeCxt = AllocSetContextCreate(CurrentMemoryContext,
										"PG-Strom BRIN-index temporary",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(perRangeCxt);

	nranges = (pi_state->nblocks +
			   pi_state->range_sz - 1) / pi_state->range_sz;
	nwords = (nranges + BITS_PER_BITMAPWORD - 1) / BITS_PER_BITMAPWORD;
	Assert(brin_map->nwords < 0);
	memset(brin_map->words, 0, sizeof(bitmapword) * nwords);
	/*
	 * Now scan the revmap.  We start by querying for heap page 0,
	 * incrementing by the number of pages per range; this gives us a full
	 * view of the table.
	 */
	for (heapBlk = 0, index = 0;
		 heapBlk < nblocks;
		 heapBlk += range_sz, index++)
	{
		BrinTuple  *tup;
		OffsetNumber off;
		Size		size;
		int			keyno;

		CHECK_FOR_INTERRUPTS();

		MemoryContextResetAndDeleteChildren(perRangeCxt);

		tup = brinGetTupleForHeapBlock(pi_state->brin_revmap, heapBlk,
									   &buf, &off, &size,
									   BUFFER_LOCK_SHARE,
									   snapshot);
		if (tup)
		{
#if PG_VERSION_NUM >= 100000
			btup = brin_copy_tuple(tup, size, btup, &btupsz);
#else
			btup = brin_copy_tuple(tup, size);
#endif
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
#if PG_VERSION_NUM >= 100000
			dtup = brin_deform_tuple(bdesc, btup, dtup);
#else
			dtup = brin_deform_tuple(bdesc, btup);
#endif
			if (!dtup->bt_placeholder)
			{
				for (keyno = 0; keyno < pi_state->num_scan_keys; keyno++)
				{
					ScanKey		key = &pi_state->scan_keys[keyno];
					AttrNumber	keyattno = key->sk_attno;
					BrinValues *bval = &dtup->bt_columns[keyattno - 1];
					Datum		rv;
					Form_pg_attribute keyattr __attribute__((unused));

#if PG_VERSION_NUM < 110000
					keyattr = bd_tupdesc->attrs[keyattno - 1];
#else
					keyattr = &bd_tupdesc->attrs[keyattno - 1];
#endif
					Assert((key->sk_flags & SK_ISNULL) ||
						   (key->sk_collation == keyattr->attcollation));
					/* First time this column? look up consistent function */
					if (consistentFn[keyattno - 1].fn_oid == InvalidOid)
					{
						FmgrInfo   *tmp;

						tmp = index_getprocinfo(pi_state->index_rel, keyattno,
												BRIN_PROCNUM_CONSISTENT);
						fmgr_info_copy(&consistentFn[keyattno - 1], tmp,
									   CurrentMemoryContext);
					}

					/*
					 * Check whether the scan key is consistent with the page
					 * range values; if so, pages in the range shall be
					 * skipped on the scan.
					 */
					rv = FunctionCall3Coll(&consistentFn[keyattno - 1],
										   key->sk_collation,
										   PointerGetDatum(bdesc),
										   PointerGetDatum(bval),
										   PointerGetDatum(key));
					if (!DatumGetBool(rv))
					{
						if (index / BITS_PER_BITMAPWORD < nwords)
							brin_map->words[index / BITS_PER_BITMAPWORD]
								|= (1U << (index % BITS_PER_BITMAPWORD));
						break;
					}
				}
			}
		}
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perRangeCxt);

	if (buf != InvalidBuffer)
		ReleaseBuffer(buf);
	/* mark this bitmapset is ready */
	pg_memory_barrier();
	brin_map->nwords = nwords;
}

void
pgstromExecGetBrinIndexMap(GpuTaskState *gts)
{
	pgstromIndexState *pi_state = gts->outer_index_state;

	if (!gts->outer_index_map || gts->outer_index_map->nwords < 0)
	{
		EState	   *estate = gts->css.ss.ps.state;

		if (!gts->outer_index_map)
		{
			Assert(!IsParallelWorker());
			gts->outer_index_map
				= MemoryContextAlloc(estate->es_query_cxt,
									 pgstromSizeOfBrinIndexMap(gts));
			gts->outer_index_map->nwords = -1;
		}

		ResetLatch(MyLatch);
		while (gts->outer_index_map->nwords < 0)
		{
			if (!IsParallelWorker())
			{
				__pgstromExecGetBrinIndexMap(pi_state,
											 gts->outer_index_map,
											 estate->es_snapshot);
				/* wake up parallel workers if any */
				if (gts->pcxt)
				{
					ParallelContext *pcxt = gts->pcxt;
					pid_t		pid;
					int			i;

					for (i=0; i < pcxt->nworkers_launched; i++)
					{
						if (GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle,
												   &pid) == BGWH_STARTED)
							ProcSendSignal(pid);
					}
				}
#if 0
				{
					Bitmapset *map = gts->outer_index_map;
					int		i;

					elog(INFO, "BRIN-index (%s) range_sz = %d",
						 RelationGetRelationName(pi_state->index_rel),
						 pi_state->range_sz);
					for (i=0; i < map->nwords; i += 4)
					{
						elog(INFO, "% 6d: %08x %08x %08x %08x",
							 i * BITS_PER_BITMAPWORD,
							 i+3 < map->nwords ? map->words[i+3] : 0,
							 i+2 < map->nwords ? map->words[i+2] : 0,
							 i+1 < map->nwords ? map->words[i+1] : 0,
							 i   < map->nwords ? map->words[i]   : 0);
					}
				}
#endif
			}
			else
			{
				/* wait for completion of BRIN-index preload */
				CHECK_FOR_INTERRUPTS();

				WaitLatch(MyLatch,
						  WL_LATCH_SET,
						  -1,
						  PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);
			}
		}
	}
}

void
pgstromExecEndBrinIndexMap(GpuTaskState *gts)
{
	pgstromIndexState *pi_state = gts->outer_index_state;

	if (!pi_state)
		return;
	brinRevmapTerminate(pi_state->brin_revmap);
	index_close(pi_state->index_rel, NoLock);
}

void
pgstromExecRewindBrinIndexMap(GpuTaskState *gts)
{}

/*
 * pgstromExplainBrinIndexMap
 */
void
pgstromExplainBrinIndexMap(GpuTaskState *gts,
						   ExplainState *es,
						   List *dcontext)
{
	pgstromIndexState *pi_state = gts->outer_index_state;
	char	   *conds_str;
	char		temp[128];

	if (!pi_state)
		return;

	conds_str = deparse_expression(pi_state->index_quals,
								   dcontext, es->verbose, false);
	ExplainPropertyText("BRIN cond", conds_str, es);
	if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			snprintf(temp, sizeof(temp), "%ld of %ld (%.2f%%)",
					 gts->outer_brin_count,
					 (long)pi_state->nblocks,
					 100.0 * ((double) gts->outer_brin_count /
							  (double) pi_state->nblocks));
			ExplainPropertyText("BRIN skipped", temp, es);
		}
		else
		{
			ExplainPropertyInteger("BRIN fetched", NULL,
								   pi_state->nblocks -
								   gts->outer_brin_count, es);
			ExplainPropertyInteger("BRIN skipped", NULL,
								   gts->outer_brin_count, es);
		}
	}
}

/*
 * pgstromExecScanChunkParallel - read the relation with parallel scan
 */
static pgstrom_data_store *
pgstromExecScanChunkParallel(GpuTaskState *gts,
							 pgstrom_data_store *pds,
							 Bitmapset *brin_map,
							 cl_long brin_range_sz)
{
	GpuTaskSharedState *gtss = gts->gtss;
	Relation	relation = gts->css.ss.ss_currentRelation;
	HeapScanDesc scan = gts->css.ss.ss_currentScanDesc;

	for (;;)
	{
		if (!scan->rs_inited)
		{
			Assert(scan->rs_parallel);
			if (scan->rs_nblocks == 0)
			{
				/* no blocks to read */
				break;
			}
			scan->rs_cblock = InvalidBlockNumber;
			scan->rs_numblocks = 0;		/* force to get next blocks */
			scan->rs_inited = true;
		}
		else if (scan->rs_cblock == InvalidBlockNumber)
		{
			/* end of the scan */
			break;
		}

		if (scan->rs_numblocks == 0)
		{
			NVMEScanState *nvme_sstate = gts->nvme_sstate;
			BlockNumber	sync_startpage = InvalidBlockNumber;
			cl_long		nr_allocated;
			cl_long		startblock;
			cl_long		nr_blocks;
			cl_long		page;

			/*
			 * MEMO: A key of i/o performance is consolidation of continuous
			 * block reads with a small number of system-call invocation.
			 * The default one-by-one block read logic tend to generate i/o
			 * request fragmentation under CPU parallel execution, thus it
			 * leads larger number of read commands submit and performance
			 * slow-down.
			 * So, in case of NVMe-Strom under CPU parallel, we make the
			 * @scan->rs_cblock pointer advanced by multiple blocks at once.
			 * It ensures the block numbers to read are continuous, thus,
			 * i/o stack will be able to load storage blocks with minimum
			 * number of DMA requests.
			 */
			if (!nvme_sstate)
				nr_blocks = 8;
			else if (pds)
			{
				if (pds->kds.nitems >= pds->kds.nrooms)
					break;	/* no more rooms in this PDS */
				nr_blocks = pds->kds.nrooms - pds->kds.nitems;
			}
			else
				nr_blocks = nvme_sstate->nblocks_per_chunk;

		retry_lock:
			SpinLockAcquire(&gtss->phscan.phs_mutex);
			/*
			 * If the scan's startblock has not yet been initialized, we must
			 * do it now. If this is not a synchronized scan, we just start
			 * at block 0, but if it is a synchronized scan, we must get
			 * the starting position from the synchronized scan facility.
			 * We can't hold the spinlock while doing that, though, so release
			 * the spinlock once, get the information we need, and retry.
			 * If nobody else has initialized the scan in the meantime,
			 * we'll fill in the value we fetched on the second time through.
			 */
			if (gtss->phscan.phs_startblock == InvalidBlockNumber)
			{
				if (!gtss->phscan.phs_syncscan)
					gtss->phscan.phs_startblock = 0;
				else if (sync_startpage != InvalidBlockNumber)
					gtss->phscan.phs_startblock = sync_startpage;
				else
				{
					SpinLockRelease(&gtss->phscan.phs_mutex);
					sync_startpage = ss_get_location(relation,
													 scan->rs_nblocks);
					goto retry_lock;
				}
			}
			scan->rs_startblock = startblock = gtss->phscan.phs_startblock;
			nr_allocated = gtss->nr_allocated;

			if (nr_allocated >= (cl_long)scan->rs_nblocks)
			{
				SpinLockRelease(&gtss->phscan.phs_mutex);
				scan->rs_cblock = InvalidBlockNumber;	/* end of the scan */
				break;
			}
			if (nr_allocated + nr_blocks >= (cl_long)scan->rs_nblocks)
				nr_blocks = (cl_long)scan->rs_nblocks - nr_allocated;
			page = (startblock + nr_allocated) % (cl_long)scan->rs_nblocks;
			if (page + nr_blocks >= (cl_long)scan->rs_nblocks)
				nr_blocks = (cl_long)scan->rs_nblocks - page;

			/* should never read the blocks across segment boundary */
			Assert(nr_blocks > 0 && nr_blocks <= RELSEG_SIZE);
			if ((page / RELSEG_SIZE) != (page + nr_blocks - 1) / RELSEG_SIZE)
				nr_blocks = RELSEG_SIZE - (page % RELSEG_SIZE);
			Assert(nr_blocks > 0);

			if (brin_map)
			{
				long	pos = page / brin_range_sz;
				long	end = (page + nr_blocks - 1) / brin_range_sz;
				long	s_page = -1;
				long	e_page = page + nr_blocks;

				/* find the first valid range */
				while (pos <= end)
				{
					if (!bms_is_member(pos, brin_map))
					{
						s_page = Max(page, pos * brin_range_sz);
						break;
					}
					pos++;
				}

				if (s_page < 0)
				{
					/* Oops, here is no valid range, so just skip it */
					gts->outer_brin_count += nr_blocks;
					nr_allocated += nr_blocks;
					nr_blocks = 0;
				}
				else
				{
					long	prev = page;
					/* find the continuous valid ranges */
					Assert(pos <= end);
					Assert(!bms_is_member(pos, brin_map));
					while (pos <= end)
					{
						if (bms_is_member(pos, brin_map))
						{
							e_page = Min(e_page, pos * brin_range_sz);
							break;
						}
						pos++;
					}
					nr_allocated += (e_page - page);
					nr_blocks = e_page - s_page;
					page = s_page;
					gts->outer_brin_count += page - prev;
				}
			}
			else
			{
				/* elsewhere, just walk on the following blocks */
				nr_allocated += nr_blocks;
			}
			/* update # of blocks already allocated to workers */
			gtss->nr_allocated = nr_allocated;
			SpinLockRelease(&gtss->phscan.phs_mutex);

			scan->rs_cblock = page;
			scan->rs_numblocks = nr_blocks;
			continue;
		}
		/* allocation of row-based PDS on demand */
		if (!pds)
		{
			if (gts->nvme_sstate)
				pds = PDS_create_block(gts->gcontext,
									   RelationGetDescr(relation),
									   gts->nvme_sstate);
			else
				pds = PDS_create_row(gts->gcontext,
									 RelationGetDescr(relation),
									 pgstrom_chunk_size());
			pds->kds.table_oid = RelationGetRelid(relation);
		}
		/* scan next block */
		if (!PDS_exec_heapscan(gts, pds))
			break;
		/* move to the next block */
		scan->rs_numblocks--;
		scan->rs_cblock++;
		if (scan->rs_cblock >= scan->rs_nblocks)
			scan->rs_cblock = 0;
		if (scan->rs_syncscan)
			ss_report_location(relation, scan->rs_cblock);
		/* end of the scan? */
		if (scan->rs_cblock == scan->rs_startblock)
			scan->rs_cblock = InvalidBlockNumber;
	}
	return pds;
}

/*
 * pgstromExecScanChunk - read the relation by one chunk
 */
pgstrom_data_store *
pgstromExecScanChunk(GpuTaskState *gts)
{
	Relation		rel = gts->css.ss.ss_currentRelation;
	HeapScanDesc	scan = gts->css.ss.ss_currentScanDesc;
	Bitmapset	   *brin_map;
	cl_long			brin_range_sz = 0;
	pgstrom_data_store *pds = NULL;

	/*
	 * Setup scan-descriptor, if the scan is not parallel, of if we're
	 * executing a scan that was intended to be parallel serially.
	 */
	if (!scan)
	{
		EState	   *estate = gts->css.ss.ps.state;

		if (!gts->gtss)
			scan = heap_beginscan(rel, estate->es_snapshot, 0, NULL);
		else
			scan = heap_beginscan_parallel(rel, &gts->gtss->phscan);

		gts->css.ss.ss_currentScanDesc = scan;
		/*
		 * Try to choose NVMe-Strom, if relation is deployed on the supported
		 * tablespace and expected total i/o size is enough large than cache-
		 * only scan.
		 */
		PDS_init_heapscan_state(gts);
	}
	InstrStartNode(&gts->outer_instrument);
	/* Load the BRIN-index bitmap, if any */
	if (gts->outer_index_state)
		pgstromExecGetBrinIndexMap(gts);
	brin_map = gts->outer_index_map;
	if (brin_map)
		brin_range_sz = gts->outer_index_state->range_sz;

	if (gts->gtss)
	{
		pds = pgstromExecScanChunkParallel(gts, pds, brin_map, brin_range_sz);
	}
	else
	{
		for (;;)
		{
			cl_long		page;

			if (!scan->rs_inited)
			{
				/* no blocks to read? */
				if (scan->rs_nblocks == 0)
					break;
				scan->rs_cblock = scan->rs_startblock;
				Assert(scan->rs_numblocks == InvalidBlockNumber);
				scan->rs_inited = true;
			}
			else if (scan->rs_cblock == InvalidBlockNumber)
			{
				/* no more blocks to read */
				break;
			}
			page = scan->rs_cblock;

			/*
			 * If any, check BRIN-index bitmap, then moves to the next range
			 * boundary if no tuple can match in this range.
			 */
			if (brin_map)
			{
				long	pos = page / brin_range_sz;

				if (bms_is_member(pos, brin_map))
				{
					long	prev = page;

					page = (pos + 1) * brin_range_sz;
					if (page <= (cl_long)MaxBlockNumber)
						scan->rs_cblock = (BlockNumber)page;
					else
						scan->rs_cblock = 0;
					gts->outer_brin_count += (page - prev);
					goto skip;
				}
			}

			/* allocation of row-based PDS on demand */
			if (!pds)
			{
				if (gts->nvme_sstate)
					pds =  PDS_create_block(gts->gcontext,
											RelationGetDescr(rel),
											gts->nvme_sstate);
				else
					pds = PDS_create_row(gts->gcontext,
										 RelationGetDescr(rel),
										 pgstrom_chunk_size());
				pds->kds.table_oid = RelationGetRelid(rel);
			}
			/* scan the next block */
			if (!PDS_exec_heapscan(gts, pds))
				break;		/* no more tuples we can store now! */
			/* move to the next block */
			scan->rs_cblock++;
		skip:
			if (scan->rs_cblock >= scan->rs_nblocks)
				scan->rs_cblock = 0;
			Assert(scan->rs_numblocks == InvalidBlockNumber);
			if (scan->rs_syncscan)
				ss_report_location(scan->rs_rd, scan->rs_cblock);
			/* end of the scan? */
			if (scan->rs_cblock == scan->rs_startblock)
				scan->rs_cblock = InvalidBlockNumber;
		}
	}

	if (!pds)
	{
		/* end of the scan */
		Assert(!BlockNumberIsValid(scan->rs_cblock));
	}
	else if (pds->kds.nitems == 0)
	{
		/* empty result */
		Assert(!BlockNumberIsValid(scan->rs_cblock));
		PDS_release(pds);
		pds = NULL;
	}
	else if (pds->kds.format == KDS_FORMAT_BLOCK &&
			 pds->kds.nitems < pds->kds.nrooms &&
			 pds->nblocks_uncached > 0)
	{
		/*
		 * MEMO: Special case handling if KDS_FORMAT_BLOCK was not filled
		 * up entirely. KDS_FORMAT_BLOCK has an array of block-number to
		 * support "ctid" system column, located on next to the KDS-head.
		 * Block-numbers of pre-loaded blocks (hit on shared buffer) are
		 * used from the head, and others (to be read from the file) are
		 * used from the tail. If nitems < nrooms, this array has a hole
		 * on the middle of array.
		 * So, we have to move later half of the array to close the hole
		 * and make a flat array.
		 */
		BlockNumber	   *block_nums
			= (BlockNumber *)KERN_DATA_STORE_BODY(&pds->kds);

		memmove(block_nums + (pds->kds.nitems - pds->nblocks_uncached),
				block_nums + (pds->kds.nrooms - pds->nblocks_uncached),
				sizeof(BlockNumber) * pds->nblocks_uncached);
	}
	/* update statistics */
	if (pds)
	{
		if (pds->kds.format == KDS_FORMAT_BLOCK)
			gts->nvme_count += pds->nblocks_uncached;
	}
	InstrStopNode(&gts->outer_instrument,
				  !pds ? 0.0 : (double)pds->kds.nitems);
	return pds;
}

/*
 * pgstromRewindScanChunk
 */
void
pgstromRewindScanChunk(GpuTaskState *gts)
{
	HeapScanDesc	scan = gts->css.ss.ss_currentScanDesc;

	InstrEndLoop(&gts->outer_instrument);
	heap_rescan(scan, NULL);
#if PG_VERSION_NUM < 100000
	/*
	 * In PG9.6, re-initialization of DSM segment is a role of ReScan method,
	 * then it was moved to ReInitializeDSM method on the later version.
	 * phs_cblock must be reset to zero to rewind the scan.
	 */
	if (scan->rs_parallel != NULL)
	{
		ParallelHeapScanDesc parallel_scan = scan->rs_parallel;

		SpinLockAcquire(&parallel_scan->phs_mutex);
		parallel_scan->phs_cblock = parallel_scan->phs_startblock;
		SpinLockRelease(&parallel_scan->phs_mutex);
	}
#endif
	ExecScanReScan(&gts->css.ss);
}

/*
 * pgstromExplainOuterScan
 */
void
pgstromExplainOuterScan(GpuTaskState *gts,
						List *deparse_context,
						List *ancestors,
						ExplainState *es,
						List *outer_quals,
						Cost outer_startup_cost,
						Cost outer_total_cost,
						double outer_plan_rows,
						int outer_plan_width)
{
	Plan		   *plannode = gts->css.ss.ps.plan;
	Index			scanrelid = ((Scan *) plannode)->scanrelid;
	Instrumentation *instrument = &gts->outer_instrument;
	RangeTblEntry  *rte;
	const char	   *refname;
	const char	   *relname;
	const char	   *nspname = NULL;
	StringInfoData	str;

	/* Does this GpuTaskState has outer simple scan? */
	if (scanrelid == 0)
		return;

	/*
	 * See the logic in ExplainTargetRel()
	 */
	rte = rt_fetch(scanrelid, es->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	refname = (char *) list_nth(es->rtable_names, scanrelid - 1);
	if (!refname)
		refname = rte->eref->aliasname;
	relname = get_rel_name(rte->relid);
	if (es->verbose)
		nspname = get_namespace_name(get_rel_namespace(rte->relid));

	initStringInfo(&str);
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (nspname != NULL)
			appendStringInfo(&str, "%s.%s",
							 quote_identifier(nspname),
							 quote_identifier(relname));
		else if (relname)
			appendStringInfo(&str, "%s",
							 quote_identifier(relname));
		if (!relname || strcmp(refname, relname) != 0)
		{
			if (str.len > 0)
				appendStringInfoChar(&str, ' ');
			appendStringInfo(&str, "%s", refname);
		}
	}
	else
	{
		ExplainPropertyText("Outer Scan Relation", relname, es);
		if (nspname)
			ExplainPropertyText("Outer Scan Schema", nspname, es);
		ExplainPropertyText("Outer Scan Alias", refname, es);
	}

	if (es->costs)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(&str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
							 outer_startup_cost,
							 outer_total_cost,
							 outer_plan_rows,
							 outer_plan_width);
		else
		{
			ExplainPropertyFloat("Outer Startup Cost",
								 NULL, outer_startup_cost, 2, es);
			ExplainPropertyFloat("Outer Total Cost",
								 NULL, outer_total_cost, 2, es);
			ExplainPropertyFloat("Outer Plan Rows",
								 NULL, outer_plan_rows, 0, es);
			ExplainPropertyFloat("Outer Plan Width",
								 NULL, outer_plan_width, 0, es);
		}
	}

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 * See the comment in ExplainNode()
	 */
	InstrEndLoop(instrument);

	if (es->analyze && instrument->nloops > 0)
	{
		double	nloops = instrument->nloops;
		double	startup_sec = 1000.0 * instrument->startup / nloops;
		double	total_sec = 1000.0 * instrument->total / nloops;
		double	rows = instrument->ntuples / nloops;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (es->timing)
				appendStringInfo(
					&str,
					" (actual time=%.3f..%.3f rows=%.0f loops=%.0f)",
					startup_sec, total_sec, rows, nloops);
			else
				appendStringInfo(
					&str,
					" (actual rows=%.0f loops=%.0f)",
					rows, nloops);
		}
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Outer Actual Startup Time",
									 NULL, startup_sec, 3, es);
				ExplainPropertyFloat("Outer Actual Total Time",
									 NULL, total_sec, 3, es);
			}
			ExplainPropertyFloat("Outer Actual Rows", NULL, rows, 0, es);
			ExplainPropertyFloat("Outer Actual Loops", NULL, nloops, 0, es);
		}
	}
	else if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(&str, " (never executed)");
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Outer Actual Startup Time",
									 NULL, 0.0, 3, es);
				ExplainPropertyFloat("Outer Actual Total Time",
									 NULL, 0.0, 3, es);
			}
			ExplainPropertyFloat("Outer Actual Rows",
								 NULL, 0.0, 0, es);
			ExplainPropertyFloat("Outer Actual Loops",
								 NULL, 0.0, 0, es);
		}
	}
	if (es->format == EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText("Outer Scan", str.data, es);

	if (outer_quals)
	{
		Expr   *quals_expr;
		char   *temp;

		quals_expr = make_ands_explicit(outer_quals);
		temp = deparse_expression((Node *)quals_expr,
								  deparse_context,
								  es->verbose, false);
		ExplainPropertyText("Outer Scan Filter", temp, es);

		if (gts->outer_instrument.nfiltered1 > 0.0)
			ExplainPropertyFloat("Rows Removed by Outer Scan Filter",
								 NULL,
								 gts->outer_instrument.nfiltered1 /
								 gts->outer_instrument.nloops,
								 0, es);
	}
	/* properties of BRIN-index */
	pgstromExplainBrinIndexMap(gts, es, deparse_context);
}

/*
 * pgstrom_init_relscan
 */
void
pgstrom_init_relscan(void)
{
	/* pg_strom.enable_brin */
	DefineCustomBoolVariable("pg_strom.enable_brin",
							 "Enables to use BRIN-index",
							 NULL,
							 &pgstrom_enable_brin,
							 true,
							 PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
}

/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a relation.
 *		ExecSeqNext				retrieve next tuple in sequential order.
 *		ExecInitSeqScan			creates and initializes a seqscan node.
 *		ExecEndSeqScan			releases any storage allocated.
 *		ExecReScanSeqScan		rescans the relation
 *
 *		ExecSeqScanEstimate		estimates DSM space needed for parallel scan
 *		ExecSeqScanInitializeDSM initialize DSM for parallel scan
 *		ExecSeqScanReInitializeDSM reinitialize DSM for fresh parallel scan
 *		ExecSeqScanInitializeWorker attach to DSM info in parallel worker
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "access/skey.h"
#include "executor/execdebug.h"
#include "executor/nodeSeqscan.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

static TupleTableSlot *SeqNext(SeqScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SeqNext(SeqScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
		scandesc = table_beginscan(node->ss.ss_currentRelation,
								   estate->es_snapshot,
								   node->sss_NumScanKeys, node->sss_ScanKeys);
		node->ss.ss_currentScanDesc = scandesc;
	}

	/*
	 * get the next tuple from the table
	 */
	if (table_scan_getnextslot(scandesc, direction, slot))
		return slot;
	return NULL;
}

/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SeqRecheck(SeqScanState *node, TupleTableSlot *slot)
{
	/*
	 * Note that unlike IndexScan, SeqScan never use keys in heap_beginscan
	 * (and this is very bad) - so, here we do not check are keys ok or not.
	 */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSeqScan(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SeqNext,
					(ExecScanRecheckMtd) SeqRecheck);
}


/* Detect eligible qualifiers for in-storage offloading. */
static List*
ExecTableBuildScanKeys(PlanState *planstate, Relation table,
					   List *quals,
					   ScanKey *scanKeys, int *numScanKeys) {
	ListCell   *qual_cell;
	ScanKey		scan_keys;
	int			n_scan_keys;

	n_scan_keys = 0;
	scan_keys = (ScanKey) palloc(list_length(quals) * sizeof(ScanKeyData));

	foreach(qual_cell, quals)
	{
		Expr	   *clause = (Expr *) lfirst(qual_cell);
		ScanKey		this_scan_key = &scan_keys[n_scan_keys];
		Oid			opno;		/* operator's OID */
		RegProcedure opfuncid;	/* operator proc id used in scan */
		Oid			op_righttype;
		Expr	   *leftop;		/* expr on lhs of operator */
		Expr	   *rightop;	/* expr on rhs ... */
		AttrNumber	varattno;	/* att number used in scan */
		int			relnatts;

		relnatts = RelationGetNumberOfAttributes(table);
		if (IsA(clause, OpExpr))
		{
			int			flags = 0;
			Datum		scanvalue;

			opno = ((OpExpr *) clause)->opno;
			opfuncid = ((OpExpr *) clause)->opfuncid;

			/*
			 * leftop should be the index key Var, possibly relabeled
			 */
			leftop = (Expr *) get_leftop(clause);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!IsA(leftop, Var))
					continue;

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > relnatts)
					continue;

			/*
			 * rightop is the constant or variable comparison value
			 */
			rightop = (Expr *) get_rightop(clause);

			if (rightop && IsA(rightop, RelabelType))
						rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			if (IsA(rightop, Const))
			{
				/* OK, simple constant comparison value */
				scanvalue = ((Const *) rightop)->constvalue;
				op_righttype = ((Const *) rightop)->consttype;
				if (((Const *) rightop)->constisnull)
					flags |= SK_ISNULL;
			} else {
				continue;
			}

			/* initialize the scan key's fields appropriately */
			ScanKeyEntryInitialize(this_scan_key,
										   flags,
										   varattno,	/* attribute number to scan */
										   InvalidStrategy, /* op's strategy */
										   op_righttype,	/* strategy subtype */
										   ((OpExpr *) clause)->inputcollid,	/* collation */
										   opfuncid,	/* reg proc to use */
										   scanvalue);	/* constant */

			quals = foreach_delete_current(quals, qual_cell);
			n_scan_keys++;
		}
	}

	if (n_scan_keys == 0) {
		pfree(scan_keys);
		scan_keys = NULL;
	}

	*scanKeys = scan_keys;
	*numScanKeys = n_scan_keys;

	return quals;
}

/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *
ExecInitSeqScan(SeqScan *node, EState *estate, int eflags)
{
	SeqScanState *scanstate;

	/*
	 * Once upon a time it was possible to have an outerPlan of a SeqScan, but
	 * not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SeqScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecSeqScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	scanstate->ss.ss_currentRelation =
		ExecOpenScanRelation(estate,
							 node->scanrelid,
							 eflags);

	/* and create slot with the appropriate rowtype */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation));

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	node->plan.qual = ExecTableBuildScanKeys((PlanState *) scanstate,
							   scanstate->ss.ss_currentRelation,
							   node->plan.qual,
							   &scanstate->sss_ScanKeys,
							   &scanstate->sss_NumScanKeys);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
			ExecInitQual(node->plan.qual, (PlanState *) scanstate);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSeqScan(SeqScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * get information from node
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
		table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSeqScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanSeqScan(SeqScanState *node)
{
	TableScanDesc scan;

	scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_rescan(scan,		/* scan desc */
					 NULL);		/* new scan keys */

	ExecScanReScan((ScanState *) node);
}

/* ----------------------------------------------------------------
 *						Parallel Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSeqScanEstimate
 *
 *		Compute the amount of space we'll need in the parallel
 *		query DSM, and inform pcxt->estimator about our needs.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanEstimate(SeqScanState *node,
					ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->pscan_len = table_parallelscan_estimate(node->ss.ss_currentRelation,
												  estate->es_snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeDSM
 *
 *		Set up a parallel heap scan descriptor.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeDSM(SeqScanState *node,
						 ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelTableScanDesc pscan;

	pscan = shm_toc_allocate(pcxt->toc, node->pscan_len);
	table_parallelscan_initialize(node->ss.ss_currentRelation,
								  pscan,
								  estate->es_snapshot);
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, pscan);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanReInitializeDSM(SeqScanState *node,
						   ParallelContext *pcxt)
{
	ParallelTableScanDesc pscan;

	pscan = node->ss.ss_currentScanDesc->rs_parallel;
	table_parallelscan_reinitialize(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeWorker
 *
 *		Copy relevant information from TOC into planstate.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeWorker(SeqScanState *node,
							ParallelWorkerContext *pwcxt)
{
	ParallelTableScanDesc pscan;

	pscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}

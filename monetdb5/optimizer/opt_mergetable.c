/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 * 
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * The Original Code is the MonetDB Database System.
 * 
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2012 MonetDB B.V.
 * All Rights Reserved.
*/
#include "monetdb_config.h"
#include "opt_mergetable.h"

typedef enum mat_type {
	mat_none = 0,	/* Simple mat aligned operations (ie batcalc etc) */
	mat_grp = 1,	/* result of phase one of a mat - group.new/derive */
	mat_ext = 2,	/* after mat_grp the extend gets a mat.mirror */
	mat_tpn = 3,	/* Phase one of topn on a mat */
	mat_slc = 4,	/* Phase one of topn on a mat */
	mat_rdr = 5	/* Phase one of sorting, ie sorted the parts sofar */
} mat_type;

/* TODO have a seperate mat_variable list, ie old_var, mat instruction */
/* including first or second result */

typedef struct mat {
	InstrPtr mi;	/* mat instruction */
	InstrPtr mi1;	/* second result, mat instruction */
	InstrPtr mm;	/* mat merge statement (needed for group/order by's and topn ) */
	int mv;		/* mat variable */
	int mv1;	/* second result variable */
	int pushed;
	mat_type type;	/* type of operation */
} mat_t;

static int
mat_is_topn( mat_type t)
{
	return (t == mat_tpn || t == mat_slc);
}

static int
mat_is_orderby( mat_type t)
{
	return (t == mat_rdr);
}


static mat_type
useMatType( mat_t *mat, int n) 
{
	mat_type type = mat_none;
	if (n >= 0 && mat_is_topn(mat[n].type))
		type = mat[n].type;
	if (n >= 0 && mat_is_orderby(mat[n].type))
		type = mat[n].type;
	return type;
}

static int overlap( MalBlkPtr mb, int lv, int rv)
{
	VarPtr llb = varGetProp(mb, lv, tlbProp); 
	VarPtr lub = varGetProp(mb, lv, tubProp); 
	VarPtr rlb = varGetProp(mb, rv, hlbProp); 
	VarPtr rub = varGetProp(mb, rv, hubProp); 

	if (!llb || !rlb) {
		/* unknown lower bound ie overlap */
		return 1;
	}
	/* perfect match */
	if (lub->value.val.oval == rub->value.val.oval &&
	    llb->value.val.oval == rlb->value.val.oval) 
		return 1;

	if ((rub->value.val.oval != oid_nil && 
	      rub->value.val.oval <= llb->value.val.oval) || 
 	     (lub->value.val.oval != oid_nil &&
	      lub->value.val.oval <= rlb->value.val.oval))
		return 0;

	if ((rub->value.val.oval == oid_nil && 
	     rlb->value.val.oval > lub->value.val.oval) || 
	    (lub->value.val.oval == oid_nil &&
	     llb->value.val.oval > rub->value.val.oval)) {
                return 0;
	}
	return 1;
}

static int Hoverlap( MalBlkPtr mb, int lv, int rv)
{
	VarPtr llb = varGetProp(mb, lv, hlbProp); 
	VarPtr lub = varGetProp(mb, lv, hubProp); 
	VarPtr rlb = varGetProp(mb, rv, hlbProp); 
	VarPtr rub = varGetProp(mb, rv, hubProp); 
	if (!llb || !rlb) {
		/* unknown ie overlap */
		return 1;
	}
	/* perfect match */
	if (lub->value.val.oval == rub->value.val.oval &&
	    llb->value.val.oval == rlb->value.val.oval) 
		return 1;

	if ((rub->value.val.oval != oid_nil && 
	      rub->value.val.oval <= llb->value.val.oval) || 
 	     (lub->value.val.oval != oid_nil &&
	      lub->value.val.oval <= rlb->value.val.oval))
		return 0;

	if ((rub->value.val.oval == oid_nil && 
	      rlb->value.val.oval > lub->value.val.oval) || 
 	     (lub->value.val.oval == oid_nil &&
	      llb->value.val.oval > rub->value.val.oval)) {
		return 0;
	}
	return 1;
}


static void
propagateMarkProp(MalBlkPtr mb, InstrPtr q, int ivar, oid l, oid h)
{
	int ovar = getArg(q, 0);
	VarPtr hlb = varGetProp(mb, ivar, hlbProp);
	VarPtr hub = varGetProp(mb, ivar, hubProp);
	VarPtr tlb = varGetProp(mb, ivar, tlbProp);
	VarPtr tub = varGetProp(mb, ivar, tubProp);
	ValRecord v;

	if (getFunctionId(q) == markHRef) {
		if (tlb) varSetProp(mb, ovar, tlbProp, op_gte, &tlb->value);
		if (tub) varSetProp(mb, ovar, tubProp, op_lt, &tub->value);
		varSetProp(mb, ovar, hlbProp, op_gte, VALset(&v, TYPE_oid, &l));
		varSetProp(mb, ovar, hubProp, op_lt, VALset(&v, TYPE_oid, &h));
	} else { /* markTRef */
		if (hlb) varSetProp(mb, ovar, hlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, hubProp, op_lt, &hub->value);
		varSetProp(mb, ovar, tlbProp, op_gte, VALset(&v, TYPE_oid, &l));
		varSetProp(mb, ovar, tubProp, op_lt, VALset(&v, TYPE_oid, &h));
	}
}

static void
propagateBinProp(MalBlkPtr mb, InstrPtr q, int ivarl, int ivarr)
{
	int ovar = getArg(q, 0);

	if (isMatJoinOp(q)) {
		VarPtr hlb = varGetProp(mb, ivarl, hlbProp);
		VarPtr hub = varGetProp(mb, ivarl, hubProp);
		VarPtr tlb = varGetProp(mb, ivarr, tlbProp);
		VarPtr tub = varGetProp(mb, ivarr, tubProp);

		if (hlb) varSetProp(mb, ovar, hlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, hubProp, op_lt, &hub->value);
		if (tlb) varSetProp(mb, ovar, tlbProp, op_gte, &tlb->value);
		if (tub) varSetProp(mb, ovar, tubProp, op_lt, &tub->value);
	} else if (getFunctionId(q) == kunionRef) {
		/* max ?? */
		VarPtr hlb = varGetProp(mb, ivarl, hlbProp);
		VarPtr hub = varGetProp(mb, ivarr, hubProp);
		VarPtr tlb = varGetProp(mb, ivarl, tlbProp);
		VarPtr tub = varGetProp(mb, ivarr, tubProp);

		if (hlb) varSetProp(mb, ovar, hlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, hubProp, op_lt, &hub->value);
		if (tlb) varSetProp(mb, ovar, tlbProp, op_gte, &tlb->value);
		if (tub) varSetProp(mb, ovar, tubProp, op_lt, &tub->value);
	}
}

static void
propagateProp(MalBlkPtr mb, InstrPtr q, int ivar)
{
	int ovar = getArg(q, 0);
	VarPtr hlb = varGetProp(mb, ivar, hlbProp);
	VarPtr hub = varGetProp(mb, ivar, hubProp);
	VarPtr tlb = varGetProp(mb, ivar, tlbProp);
	VarPtr tub = varGetProp(mb, ivar, tubProp);

	if (getFunctionId(q) == reverseRef) {
		if (hlb) varSetProp(mb, ovar, tlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, tubProp, op_lt, &hub->value);
		if (tlb) varSetProp(mb, ovar, hlbProp, op_gte, &tlb->value);
		if (tub) varSetProp(mb, ovar, hubProp, op_lt, &tub->value);
	} else if (getFunctionId(q) == mirrorRef || 
		   (getModuleId(q) == groupRef && (
		     getFunctionId(q) == newRef ||
		     getFunctionId(q) == deriveRef||
		     getFunctionId(q) == doneRef))) {
		if (hlb) varSetProp(mb, ovar, hlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, hubProp, op_lt, &hub->value);
		if (hlb) varSetProp(mb, ovar, tlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, tubProp, op_lt, &hub->value);
	} else if (isMapOp(q) || isDiffOp(q) || isFragmentGroup(q) || isTopn(q) || isOrderby(q)) {
		if (hlb) varSetProp(mb, ovar, hlbProp, op_gte, &hlb->value);
		if (hub) varSetProp(mb, ovar, hubProp, op_lt, &hub->value);
		if (tlb) varSetProp(mb, ovar, tlbProp, op_gte, &tlb->value);
		if (tub) varSetProp(mb, ovar, tubProp, op_lt, &tub->value);
	}
	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, propageProp\n");
		printInstruction(GDKout, mb, 0, q, LIST_MAL_ALL);
	}
}

static int
isMATalias(int idx, mat_t *mat, int top){
	int i;
	for(i =0; i<top; i++)
		if( mat[i].mv == idx || mat[i].mv1 == idx) 
			return i;
	return -1;
}

/*
 * @-
 * Packing the BATs into a single one is handled here
 * Note that MATs can be defined recursively.
 */
static InstrPtr 
mat_pack_mat(MalBlkPtr mb, InstrPtr r, mat_t *mat, int m, int *mtop)
{
	int l,k;

	assert(mat[m].type != mat_grp);
	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_pack_mat\n");
		printInstruction(GDKout, mb, 0, mat[m].mi, LIST_MAL_ALL);
	}

	if( mat[m].mi->argc-mat[m].mi->retc == 1){
		/* simple assignment is sufficient */
		r = newInstruction(mb, ASSIGNsymbol);
		getArg(r,0) = getArg(mat[m].mi,0);
		getArg(r,1) = getArg(mat[m].mi,1);
		/* dirty hack */
		r->retc = 1;
		r->argc = 2;
	} else {
		if (r)
			assert(0);
		if (r == NULL){
			r = newInstruction(mb, ASSIGNsymbol);
			setModuleId(r,matRef);
			setFunctionId(r,packRef);
			getArg(r,0) = getArg(mat[m].mi,0);
		}
		for(l=mat[m].mi->retc; l< mat[m].mi->argc; l++){
			k= isMATalias( getArg(mat[m].mi,l), mat, *mtop);
			if( k< 0)
				r= pushArgument(mb,r, getArg(mat[m].mi,l));
			else 
				r= mat_pack_mat(mb, r, mat, k, mtop);
		}
	}
	return r;
}

static InstrPtr
mat_pack_group(MalBlkPtr mb, mat_t *mat, int g, int ext);

static void
MATshift(mat_t *mat, int m, int *mtop)
{
	int j;

	for(j=m; j < *mtop-1; j++)
		mat[j] = mat[j+1];
	*mtop = *mtop-1; 
}

static InstrPtr 
MATpackAll(MalBlkPtr mb, InstrPtr mi, mat_t *mat, int m, int *mtop)
{
	InstrPtr r = NULL;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_packAll\n");
		printInstruction(GDKout, mb, 0, mat[m].mi, LIST_MAL_ALL);
	}

	if (mat[m].type == mat_none || mat_is_topn(mat[m].type) || mat_is_orderby(mat[m].type)) {
		r = mat_pack_mat(mb, mi, mat, m, mtop);
		pushInstruction(mb,r);
	} else if (mat[m].type == mat_ext) {
		assert(mat[m].mm);
		getArg(mat[m].mm, 0) = mat[m].mv;
	} else if (mat[m].type == mat_grp) {
		r = mat_pack_group(mb, mat, m, m);
		getArg(r, 0) = mat[m].mv;
		getArg(r, 1) = mat[m].mv1;
	}
	MATshift(mat,m,mtop);
	return r;
}

static int 
MATpackAll2(MalBlkPtr mb, InstrPtr mi, mat_t *mat, int m, int *mtop)
{
	InstrPtr r = NULL;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_packAll2\n");
		printInstruction(GDKout, mb, 0, mat[m].mi, LIST_MAL_ALL);
	}
	if (mat[m].type == mat_none || mat_is_topn(mat[m].type) || mat_is_orderby(mat[m].type)) {
		if ((mat_is_topn(mat[m].type) || mat_is_orderby(mat[m].type)) &&
			!mat[m].mm) 
				return -1;
		r = mat_pack_mat(mb, mi, mat, m, mtop);
		pushInstruction(mb,r);
		MATshift(mat,m,mtop);
	} else if (mat[m].type == mat_ext) {
		assert(mat[m].mm);
		getArg(mat[m].mm, 0) = mat[m].mv;
	} else if (mat[m].type == mat_grp) {
		r = mat_pack_group(mb, mat, m, m);
		getArg(r, 0) = mat[m].mv;
		getArg(r, 1) = mat[m].mv1;
	}
	return 0;
}

inline static int
mat_add(mat_t *mat, int mtop, InstrPtr q, InstrPtr r, mat_type type, int pushed) 
{
	mat[mtop].mi = q;
	mat[mtop].mi1 = r;
	mat[mtop].mv = getArg(q,0);
	mat[mtop].mv1 = -1;
	mat[mtop].pushed = pushed;
	if (r)
		mat[mtop].mv1 = getArg(r,0);
	mat[mtop].type = type;
	return mtop+1;
}

static void
mat_update(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int var)
{
	int k, tpe = getArgType(mb,p,0);
	int inout = getArg(p,1);

	for(k=1; k<mat[m].mi->argc; k++) {
		InstrPtr q = copyInstruction(p);

		getArg(q,var) = getArg(mat[m].mi,k);
		if (k+1 < mat[m].mi->argc) 
			getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = inout; 
		inout = getArg(q,0);
		pushInstruction(mb,q);
	}
}

/* mat_apply1 handles both apply and reduce cases */
static InstrPtr
mat_apply1(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int var, int reuse)
{
	int tpe, k, inout = 0, bvar = (var==2)?1:2;
	InstrPtr r = NULL, mi = mat[m].mi;

	assert(reuse == 0);
	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_apply1\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}
	(void)reuse;
	if (var == 2 && isDiffOp(p)) {
		tpe = getArgType(mb,p,0);
		/* use results as inputs */
		inout = 1;
	} else if (isaBatType(getVarType(mb,getArg(p,0)))){
		r = newInstruction(mb, ASSIGNsymbol);
		setModuleId(r,matRef);
		setFunctionId(r,newRef);
		getArg(r,0) = getArg(p,0);
		tpe = getArgType(mb,p,0);
	} else {
		assert(0);
		tpe = TYPE_any;
	}

	/* mirror on group */
	if (getModuleId(p) == batRef && getFunctionId(p) == mirrorRef &&
		getArg(p, 1) == mat[m].mv1) 
		mi = mat[m].mi1;

	for(k=1; k<mi->argc; k++) {
		int used = 0;
		if (inout || p->argc == 2 ||
		    /* join + overlap */
		    /* or calc + hoverlap */
		    (isMatJoinOp(p) &&
                    overlap(mb, getArg(mi,k), getArg(p, bvar)))  ||
		    (!isMatJoinOp(p) &&
                    Hoverlap(mb, getArg(mi,k), getArg(p, bvar)))) {
			InstrPtr q = copyInstruction(p);

			getArg(q,var) = getArg(mi,k);
			if (!inout)
				getArg(q,0) = newTmpVariable(mb, tpe);
			if (inout)
				inout = getArg(q,0);
			if (inout && k > 1) 
				getArg(q,1) = inout; 
			if (inout && k == 1)
				propagateProp(mb, q, getArg(p,1));
			else if (!inout)
				propagateProp(mb, q, getArg(mi, k));
			pushInstruction(mb,q);
			if (r) {
				r = pushArgument(mb,r,getArg(q,0));
			}
			used = 1;
		}
		if (r && !used && 
		   (getFunctionId(p) == kdifferenceRef ||
		    getFunctionId(p) == kunionRef)) 
			r = pushArgument(mb, r, getArg(mi, k));
	}
	if (r)
		assert(r->argc > 1);
	return r;
}

static int
disjoint(mat_t *mat, int *mtop, int m, MalBlkPtr mb)
{
	int i, j;
	char *used = GDKzalloc(mat[m].mi->argc);
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);
	int tpe = getVarType(mb,getArg(mat[m].mi, 0));
	int vj, vi;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_disjoint\n");
		printInstruction(GDKout, mb, 0, mat[m].mi, LIST_MAL_ALL);
	}

	if (used == NULL)
		return 0;
	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = newTmpVariable(mb, tpe);

	for(j=1; j<mat[m].mi->argc; j++) {
		if (!used[j]) {
			used[j] = 1;
			vj = getArg(mat[m].mi, j);
			for(i=1; i<mat[m].mi->argc; i++) {
				vi = getArg(mat[m].mi, i);
				if (!used[i] && Hoverlap(mb, vj, vi)) {
					InstrPtr q = newInstruction(mb, ASSIGNsymbol);

					setModuleId(q, algebraRef);
					setFunctionId(q, kunionRef);
					getArg(q,0) = newTmpVariable(mb, tpe);

					q=pushArgument(mb, q, vj);
					q=pushArgument(mb, q, vi);
					pushInstruction(mb,q);
					propagateBinProp(mb, q, vj, vi);

					vj = getArg(q,0);
					used[i] = 1;
				}
			}
			r = pushArgument(mb, r, vj);
		}
	}
	//pushInstruction(mb,r);
	*mtop = mat_add(mat, *mtop, r, NULL, mat_none, 0);
	GDKfree(used);
	return *mtop -1;
}

/*
 anti/semi join (X,Y) 
	X should be disjoint (todo add check or flag)
	Y m parts some not disjoint make disjoint
		union(parts of Y)
 */
static InstrPtr
mat_apply2(MalBlkPtr mb, InstrPtr p, mat_t *mat, int *mtop, int a1, int a2, int reuse)
{
	int tpe = getArgType(mb,p,0);
	int k,j,m,n,Am,An;
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_apply2\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}
	if (reuse) {
		if (a1 == a2) { 
			a2 = -1;
			reuse = 2;
		}
	}

	m = a1;
	n = a2;
	Am = 1;
	An = 2;

	if (isDiffOp(p)) {
		n = disjoint(mat, mtop, n, mb);
	} 

	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = getArg(p,0);

	assert(mat[m].mi1 == NULL);
	for(j=1; j<mat[m].mi->argc; j++) {
		int used = 0;
		int vm = getArg(mat[m].mi, j);
		for(k=1; k<mat[n].mi->argc; k++) {
			int vn = getArg(mat[n].mi, k);
			if (Hoverlap(mb, vm, vn)) {
				InstrPtr q = copyInstruction(p);
				if (!reuse)
					getArg(q,0) = newTmpVariable(mb, tpe);
				else
					getArg(q,0) = vm;
				getArg(q,Am) = vm;
				getArg(q,An) = vn;
				if (reuse) 
					getArg(q,reuse) = vm;
				pushInstruction(mb,q);

				if (!reuse)
					propagateProp(mb, q, vm);

				/* add result to mat */
				r = pushArgument(mb,r,getArg(q,0));
				used = 1;
			}
		}
		if (!used && getFunctionId(p) == kdifferenceRef)
			r = pushArgument(mb, r, vm);
	}
	if (r)
		assert(r->argc > 1);
	return r;
}

static InstrPtr
mat_apply(MalBlkPtr mb, InstrPtr p, mat_t *mat, int *mtop, int a1, int a2, int reuse) 
{
	if (a1>=0 && a2<0)
		return mat_apply1(mb, p, mat, a1, 1, reuse);
	if (a1<0 && a2>=0)
		return mat_apply1(mb, p, mat, a2, 2, reuse);

	return mat_apply2(mb, p, mat, mtop, a1, a2, reuse);
}

static int 
all_mats_and_aligned(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop)
{
	int a, matsize = -1;

	for(a=p->retc; a<p->argc; a++) {
		int tpe = getArgType(mb,p,a);

		if (isaBatType(tpe)) {
			int m = isMATalias(getArg(p,a), mat, mtop);

			if (m < 0 || 
			   (matsize > 0 && matsize != mat[m].mi->argc)) 
				return 0;

			matsize = mat[m].mi->argc;
		}
	}
	return 1;
}

static int
mat_map(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop)
{
	int tpe = getArgType(mb,p,0);
	int m = -1, a, j, reuse = -1, reuse_var = -1;
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_map\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	for(a=p->retc; a<p->argc && m < 0; a++) 
		if (isaBatType(getArgType(mb,p,a))) 
			m = isMATalias(getArg(p,a), mat, mtop);

	reuse = isMATalias(getArg(p,0), mat, mtop);

	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = getArg(p,0);

	for(j=1; j<mat[m].mi->argc; j++) {
		InstrPtr q = copyInstruction(p);

		getArg(q,0) = 10; /* dummy var set later */
		for(a=p->retc; a<p->argc; a++) {
			if (isaBatType(getArgType(mb,p,a))) {
				int n = isMATalias(getArg(p,a), mat, mtop);
				getArg(q, a) = getArg(mat[n].mi, j);
				if (n == reuse)
					reuse_var = getArg(mat[n].mi, j);
			}
		}
		if (reuse < 0) {
			getArg(q,0) = newTmpVariable(mb, tpe);
			propagateProp(mb, q, getArg(mat[m].mi, j));
		} else {
			getArg(q,0) = reuse_var;
		}
		pushInstruction(mb,q);

		/* add result to mat */
		r = pushArgument(mb,r,getArg(q,0));
	}
	if (reuse < 0)
		mtop = mat_add(mat, mtop, r, NULL, mat_none, 0 );
	return mtop;
}

/* join, also handles the case that mat[m].mm is set, ie that we
   have the extend available.
*/
static int
mat_join(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m, int n)
{
	int tpe = getArgType(mb,p,0);
	int j,k;
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_join\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = getArg(p,0);
	
	assert(m>=0 || n>=0);
	if (m >= 0 && mat[m].mm && !(mat_is_topn(mat[m].type) || mat_is_orderby(mat[m].type))) {
		InstrPtr q;

		if (mat[m].type != mat_ext)
			return -1;
		if (n<0) {
			q = copyInstruction(p);
			getArg(q,0) = newTmpVariable(mb, tpe);
			getArg(q,1) = getArg(mat[m].mm,0);
			assert(getArg(q,0) < mb->vtop);
			assert(getArg(q,1) < mb->vtop);
			assert(getArg(q,2) < mb->vtop);
			pushInstruction(mb,q);

			/* add result to mat */
			r = pushArgument(mb,r,getArg(q,0));
		} else {
			InstrPtr pck = newInstruction(mb, ASSIGNsymbol);
			setModuleId(pck,matRef);
			setFunctionId(pck,packRef);
			getArg(pck,0) = newTmpVariable(mb, tpe);

			for (j=1; j<mat[n].mi->argc; j++) {
				InstrPtr q = copyInstruction(p);

				getArg(q,0) = newTmpVariable(mb, tpe);
				getArg(q,1) = getArg(mat[m].mi,0);
				getArg(q,2) = getArg(mat[n].mi,j);
				assert(getArg(q,0) < mb->vtop);
				assert(getArg(q,1) < mb->vtop);
				assert(getArg(q,2) < mb->vtop);
				pushInstruction(mb,q);

				/* pack result */
				pck = pushArgument(mb,pck,getArg(q,0));
			}
			pushInstruction(mb,pck);

			q = copyInstruction(p);
			getArg(q,0) = getArg(p,0);
			getArg(q,1) = getArg(mat[m].mm,0);
			getArg(q,2) = getArg(pck,0);
			assert(getArg(q,0) < mb->vtop);
			assert(getArg(q,1) < mb->vtop);
			assert(getArg(q,2) < mb->vtop);
			pushInstruction(mb,q);
			return mtop;
		}
	} else {
		if (m >= 0 && n >= 0) {
		    for(k=1; k<mat[m].mi->argc; k++) {
			for (j=1; j<mat[n].mi->argc; j++) {
				if (overlap(mb, getArg(mat[m].mi, k), getArg(mat[n].mi, j))){
					InstrPtr q = copyInstruction(p);

					getArg(q,0) = newTmpVariable(mb, tpe);
					getArg(q,1) = getArg(mat[m].mi,k);
					getArg(q,2) = getArg(mat[n].mi,j);
					assert(getArg(q,0) < mb->vtop);
					assert(getArg(q,1) < mb->vtop);
					assert(getArg(q,2) < mb->vtop);
					pushInstruction(mb,q);
					propagateBinProp(mb, q, getArg(mat[m].mi, k), getArg(mat[n].mi, j));
	
					/* add result to mat */
					r = pushArgument(mb,r,getArg(q,0));

					/* join explosion */
					if (r->argc > 2*mat[m].mi->argc) 
						return -1;
				}
			}
		    }
		} else {
		    int mv = (m>=0)?m:n;
		    int av = (m>=0)?1:2;
		    int bv = (m>=0)?2:1;

		    if (bv == 2) {
		    	for(k=1; k<mat[mv].mi->argc; k++) {
		 	    if (overlap(mb, getArg(mat[mv].mi, k), getArg(p, bv))){
				InstrPtr q = copyInstruction(p);

				getArg(q,0) = newTmpVariable(mb, tpe);
				getArg(q,av) = getArg(mat[mv].mi, k);
				assert(getArg(q,0) < mb->vtop);
				assert(getArg(q,1) < mb->vtop);
				assert(getArg(q,2) < mb->vtop);
				pushInstruction(mb,q);
				propagateBinProp(mb, q, getArg(mat[mv].mi, k), getArg(q, bv));
				/* add result to mat */
				r = pushArgument(mb,r,getArg(q,0));
			    }
			}
		    } else {
		    	for(k=1; k<mat[mv].mi->argc; k++) {
		 	    if (overlap(mb, getArg(p, bv), getArg(mat[mv].mi, k))){
				InstrPtr q = copyInstruction(p);

				getArg(q,0) = newTmpVariable(mb, tpe);
				getArg(q,av) = getArg(mat[mv].mi, k);
				assert(getArg(q,0) < mb->vtop);
				assert(getArg(q,1) < mb->vtop);
				assert(getArg(q,2) < mb->vtop);
				pushInstruction(mb,q);
				propagateBinProp(mb, q, getArg(q, bv), getArg(mat[mv].mi, k));
				/* add result to mat */
				r = pushArgument(mb,r,getArg(q,0));
			    }
			}
		    }
		}
	}
	if (r && r->argc <= 1) 
		return -1;
	mtop = mat_add(mat, mtop, r, NULL, (m>=0)?useMatType(mat, m):mat_none, 0);
	if (m >= 0 && mat[m].type == mat_ext)
		(void)MATpackAll2(mb, NULL, mat, mtop-1, &mtop); 
	return mtop;
}

static int
resultof(MalBlkPtr mb, int var, int topstmt)
{
	int i;

	while(--topstmt > 0) {
		InstrPtr p = mb->stmt[topstmt];
		for(i=0;i<p->retc; i++)
			if (p->argv[i] == var)
				return topstmt;
	}
	return 0;
}

/* later we should set the Lifespan parts of a variable in the mat_group
   function
 */

static int
group_chain_list_length(MalBlkPtr mb, int var, int topstmt)
{
	int cnt = 0;
	while(var) {
		int s = resultof(mb, var, topstmt);
		InstrPtr p = mb->stmt[s];

		var = 0;
		if (s == 0)
			return 0;
		if (p->argc == 5 && getModuleId(p) == groupRef && 
		   (getFunctionId(p) == deriveRef || getFunctionId(p) == doneRef))
			var = getArg(p, 3);
		cnt++;
	}
	return cnt;
}

static InstrPtr
bat_mirror(MalBlkPtr mb, int hist )
{
	InstrPtr bm = newInstruction(mb, ASSIGNsymbol);
	int tpe = getHeadType(getVarType(mb, hist)); 
	
	setModuleId(bm, batRef);
	setFunctionId(bm, mirrorRef);

	getArg(bm,0) = newTmpVariable(mb, newBatType(tpe,tpe));
	bm = pushArgument(mb, bm, hist);
	pushInstruction(mb, bm);
	propagateProp(mb, bm, hist);
	return bm;
}

static void
group_attrs(int *attrs, MalBlkPtr mb, int var, int ext, int ishist )
{
	int cnt = 0;
	while(var) {
		int s = resultof(mb, var, mb->stop);
		InstrPtr q, p = mb->stmt[s];
		int attr = 0;

		assert(s!=0);
		var = 0;
		if (getModuleId(p) == groupRef && 
	 	    (getFunctionId(p) == deriveRef || 
		     getFunctionId(p) == doneRef) && p->argc == 5){
			var = getArg(p, 3);
			attr = getArg(p, 4);
		} else if (getModuleId(p) == groupRef && 
			  (getFunctionId(p) == newRef ||
		     	   getFunctionId(p) == doneRef) && p->argc == 3){
			attr = getArg(p, 2);
		} else {
			assert(0);
		}
		if (ishist) {
			q = bat_mirror(mb, ext);
			ext = getArg(q, 0);
		}
		/* ext.leftjoin(attr); */
		q = newInstruction(mb, ASSIGNsymbol);
		setModuleId(q, algebraRef);
		setFunctionId(q, leftjoinRef);
		getArg(q, 0) = newTmpVariable(mb, getVarType(mb,attr));
		q = pushArgument(mb, q, ext);
		q = pushArgument(mb, q, attr);
		pushInstruction(mb, q);
		attrs[cnt] = getDestVar(q);
		cnt++;
	}
}

static char *
aggr_phase2(char *aggr)
{
	if (aggr == countRef || aggr == count_no_nilRef)
		return sumRef;
	/* min/max/sum/prod and unique are fine */
	return aggr;
}

/* Group partial groupings */
static InstrPtr
mat_pack_group_(MalBlkPtr mb, mat_t *mat, int g, int ext)
{
	int cnt = group_chain_list_length(mb, getArg(mat[g].mi1, 1), mb->stop);
	int *attrs = (int*) GDKmalloc(cnt * sizeof(int) * mat[ext].mi->argc), k, i;
	InstrPtr cur = NULL;

	if ( attrs == NULL)
		return NULL;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_pack_group\n");
		printInstruction(GDKout, mb, 0, mat[ext].mi, LIST_MAL_ALL);
	}

	for(k=1; k<mat[ext].mi->argc; k++) 
		group_attrs(attrs+k*cnt, mb, getArg(mat[g].mi1, k), getArg(mat[ext].mi, k), mat[ext].type != mat_ext);
	for(i=cnt-1; i>=0; i--) {
		/* pack, group (or derive) */
		InstrPtr pck = newInstruction(mb, ASSIGNsymbol);
		InstrPtr grp = newInstruction(mb, ASSIGNsymbol);

		setModuleId(grp,groupRef);
		setFunctionId(grp, i?newRef:doneRef);
		
		setModuleId(pck,matRef);
		setFunctionId(pck,packRef);
		getArg(pck,0) = newTmpVariable(mb, getVarType(mb, attrs[cnt+i]));
		for(k=1; k<mat[g].mi1->argc; k++) 
			pck = pushArgument(mb, pck, attrs[k*cnt+i]);
		pushInstruction(mb, pck);

		getArg(grp,0) = newTmpVariable(mb, newBatType(TYPE_oid,TYPE_wrd));
		grp = pushReturn(mb, grp, newTmpVariable(mb, newBatType(TYPE_oid,TYPE_oid)));
		if (cur) {
			setFunctionId(grp, i?deriveRef:doneRef);
			grp = pushArgument(mb, grp, getArg(cur, 0));
			grp = pushArgument(mb, grp, getArg(cur, 1));
		}
		grp = pushArgument(mb, grp, getArg(pck, 0));
		pushInstruction(mb, grp);
		cur = grp;
	}
	getArg(cur, 0) = mat[g].mv;
	getArg(cur, 1) = mat[g].mv1;
	assert(cur);
	GDKfree(attrs);
	return cur;
}

static InstrPtr
mat_pack_group(MalBlkPtr mb, mat_t *mat, int g, int ext)
{
	InstrPtr pgrp = mat[g].mm;

	if (!pgrp)
		pgrp = mat_pack_group_(mb, mat, g, ext);
	if (!mat[ext].mm) {
		mat[ext].mm = pgrp;
		if (mat[ext].type == mat_ext) {
			mat[ext].mm = bat_mirror(mb, getArg(pgrp, 0));	
			getArg(mat[ext].mm, 0) = mat[ext].mv;
		}
	}
	mat[g].mm = pgrp;	/* (arg1) grp */
	return pgrp;
}

static int
topn_find_mirror(MalBlkPtr mb, int var, int topstmt)
{
	int s = resultof(mb, var, topstmt);
	InstrPtr p = mb->stmt[s];

	if (s && p->argc == 2 && 
	    getModuleId(p) == batRef && getFunctionId(p) == mirrorRef)
		return getArg(p, 1);
	return 0;
}

static int
topn_chain_list_length(MalBlkPtr mb, int var)
{
	int cnt = 0;

	var = topn_find_mirror(mb, var, mb->stop);
	while(var) {
		int s = resultof(mb, var, mb->stop);
		InstrPtr p = mb->stmt[s];

		var = 0;
		if (s == 0)
			return 0;
		if ((p->argc == 3 || p->argc == 4) && isTopn(p)) {
			var = getArg(p, 1);
			cnt++;
		}
	}
	return cnt;
}

static void
topn_stmt(int *stmt, MalBlkPtr mb, int var)
{
	int cnt = 0;

	var = topn_find_mirror(mb, var, mb->stop);
	while(var) {
		int s = resultof(mb, var, mb->stop);
		InstrPtr p = mb->stmt[s];

		assert(s!=0);
		var = 0;
		if ((p->argc == 3 || p->argc == 4) && isTopn(p)) {
			var = getArg(p, 1);
			stmt[cnt++] = s;
		}
	}
}

static int
mat_pack_topn_project(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int mirror_mid)
{
	int k, a = mtop - 1;
	mat_t *mirror = mat+mirror_mid;
	InstrPtr pck, q = copyInstruction(p);

	/* 2 cases, 
		1) first time we need to merge
		2) second simply use result of first an repeat projection
	*/
	assert(mirror->mm);

	pck = newInstruction(mb, ASSIGNsymbol);
	setModuleId(pck,matRef);
	setFunctionId(pck,packRef);
	getArg(pck,0) = newTmpVariable(mb, getVarType(mb, getArg(mat[a].mi,1)));
	for(k=1; k<mat[a].mi->argc; k++) 
		pck = pushArgument(mb, pck, getArg(mat[a].mi,k) );
	pushInstruction(mb, pck);
	MATshift(mat, mtop-1, &mtop);

	getArg(q,0) = getArg(p,0);	
	getArg(q,1) = getArg(mirror->mm, 0);	/* use result of 1 */
	getArg(q,2) = getArg(pck, 0);		/* result of merge */
	pushInstruction(mb, q);
	return mtop;
}

static int
pushWrdVar(MalBlkPtr mb, wrd val)
{
        ValRecord cst;

        cst.vtype= TYPE_wrd;
        cst.val.wval= val;
        return defConstant(mb, TYPE_wrd,&cst);
}

static int
mat_pack_topn(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int mirror_mid)
{
	/* find chain of topn's */
	mat_t *mirror = mat+mirror_mid;
	int cur_topn = 0, k, i;
	int cnt = topn_chain_list_length(mb, getArg(mirror->mi,1));
	int *stmt = (int*) GDKmalloc( cnt * sizeof(int) * mirror->mi->argc );

	for(k=1; k < mirror->mi->argc; k++) 
		topn_stmt(stmt+k*cnt, mb, getArg(mirror->mi, k));
	for(i=cnt-1; i>=0; i--) {
		InstrPtr q, s = mb->stmt[stmt[cnt+i]];
		InstrPtr tpn = copyInstruction(s);
		int slc = isSlice(s);
		InstrPtr pck = newInstruction(mb, ASSIGNsymbol);
		/* get attribute of topn instruction */
		int var = (s->argc == 3 || slc)?1:2; /* topn with 1 or 2 args*/
		int arg = 0, tpe = getVarType(mb, getArg(s, var));
		int zero = 0;

		if (slc)
			zero = pushWrdVar(mb, 0);
		tpe = newBatType(TYPE_oid,tpe);
		setModuleId(pck,matRef);
		setFunctionId(pck,packRef);
		getArg(pck,0) = newTmpVariable(mb, tpe);
		/* topn(m.leftjoin(attr), n); */
		for(k=1; k < mirror->mi->argc; k++) {
			s = mb->stmt[stmt[k*cnt+i]];

			/* no offset */
			if (slc) 
				getArg(s, 2) = zero; 
			q = newInstruction(mb, ASSIGNsymbol);
			setModuleId(q, algebraRef);
			setFunctionId(q, leftjoinRef);
			getArg(q, 0) = newTmpVariable(mb, tpe);
			q = pushArgument(mb, q, getArg(mirror->mi, k));
			q = pushArgument(mb, q, getArg(s, var));
			pushInstruction(mb, q);
			pck = pushArgument(mb, pck, getArg(q,0));
		}
		pushInstruction(mb, pck);

		/* first project using the current topn as the next topn
		 * operators need aligned bats 
		 */
		var = getArg(pck,0);
		if (cur_topn) {
			InstrPtr mi = bat_mirror(mb, cur_topn);
			q = newInstruction(mb, ASSIGNsymbol);
			setModuleId(q, algebraRef);
			setFunctionId(q, leftjoinRef);
			getArg(q, 0) = newTmpVariable(mb, tpe);
			q = pushArgument(mb, q, getArg(mi, 0));
			q = pushArgument(mb, q, var);
			pushInstruction(mb, q);
			var = getArg(q,0);
		}

		/* re-do the topn */
		if (!slc)
			tpe = newBatType(TYPE_oid, TYPE_oid);
		getArg(tpn, arg++) = newTmpVariable(mb, tpe);
		if (cur_topn)
			getArg(tpn, arg++) = cur_topn;
		getArg(tpn, arg) = var;
			
		pushInstruction(mb, tpn);
		cur_topn = getArg(tpn, 0);
	}
	/* all topn's done, mirror keep in mm */
	mirror->mm = bat_mirror(mb, cur_topn);

	GDKfree(stmt);

	/* now project */
	return mat_pack_topn_project(mb, p, mat, mtop, mirror_mid);
}

static int
sort_find_mirror(MalBlkPtr mb, int var, int topstmt)
{
	int s = resultof(mb, var, topstmt);
	InstrPtr p = mb->stmt[s];

	if (s && p->argc == 2 && 
	    getModuleId(p) == batRef && getFunctionId(p) == reverseRef) {
		/* return getArg(p, 1); */
		var = getArg(p, 1);
		s = resultof(mb, var, s);
		p = mb->stmt[s];
	}
	if (s && p->argc == 4 && 
	    getModuleId(p) == algebraRef && getFunctionId(p) == markTRef) 
		return getArg(p, 1);
	return 0;
}

static int
sort_chain_list_length(MalBlkPtr mb, int var, int has_mirror)
{
	int cnt = 0;

	if (has_mirror)
		var = sort_find_mirror(mb, var, mb->stop);
	while(var) {
		int s = resultof(mb, var, mb->stop);
		InstrPtr p = mb->stmt[s];

		var = 0;
		if (s == 0)
			return 0;
		if ((p->argc == 2 || p->argc == 3) && isOrderby(p)) {
			var = getArg(p, 1);
			cnt++;
		}
	}
	return cnt;
}

static void
sort_stmt(int *stmt, MalBlkPtr mb, int var, int has_mirror)
{
	int cnt = 0;

	if (has_mirror)
		var = sort_find_mirror(mb, var, mb->stop);
	while(var) {
		int s = resultof(mb, var, mb->stop);
		InstrPtr p = mb->stmt[s];

		assert(s!=0);
		var = 0;
		if ((p->argc == 2 || p->argc == 3) && isOrderby(p)) {
			var = getArg(p, 1);
			stmt[cnt++] = s;
		}
	}
}

static int
mat_pack_sort_project(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int merged_orderby_mid)
{
	int k, a = mtop - 1;
	mat_t *merged_orderby = mat+merged_orderby_mid;
	InstrPtr pck = copyInstruction(p);

	/* 2 cases, 
		1) first time we need to merge
		2) second simply use result of first an repeat projection
	*/
	assert(merged_orderby->mm);

	/* replace leftjoin by mat.project */
	setModuleId(pck,matRef);
	setFunctionId(pck,projectRef);
	pck->argc = 1;

	pck = pushArgument(mb, pck, getArg(merged_orderby->mm,1));
	for(k=1; k<mat[a].mi->argc; k++) 
		pck = pushArgument(mb, pck, getArg(mat[a].mi,k) );
	pushInstruction(mb, pck);
	MATshift(mat, mtop-1, &mtop);
	return mtop;
}

static int
mat_pack_sort(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int mirror_mid, int has_mirror)
{
	/* find chain of orderby's */
	mat_t *mirror = mat+mirror_mid;
	int k, i;
	int cnt = sort_chain_list_length(mb, getArg(mirror->mi,1), has_mirror);
	int *stmt = (int*) GDKmalloc( cnt * sizeof(int) * mirror->mi->argc );
	InstrPtr cur_sort = NULL;

	for(k=1; k < mirror->mi->argc; k++) 
		sort_stmt(stmt+k*cnt, mb, getArg(mirror->mi, k), has_mirror);
	for(i=cnt-1; i>=0; i--) {
		InstrPtr q, s = mb->stmt[stmt[cnt+i]];
		InstrPtr pck = newInstruction(mb, ASSIGNsymbol);
		/* get attribute of sort instruction */
		int var = (s->argc == 2)?1:2; /* sort with 1 or 2 args*/
		int tpe = getTailType(getVarType(mb, getArg(s, var)));
		int stpe = TYPE_oid;

		if (s->argc == 2) /* sort */
			stpe = tpe;
		/* mat sortTail/sortTailReverse and refine/refine_reverse */
		setModuleId(pck,matRef);
		setFunctionId(pck,getFunctionId(s)); 
		/* double outputs (sorted values and map) */
		if (has_mirror)
			getArg(pck,0) = newTmpVariable(mb, newBatType(TYPE_void,stpe));
		else
			getArg(pck,0) = (i)?newTmpVariable(mb, newBatType(TYPE_void,stpe)):getArg(mirror->mi, 0);
		getArg(pck,1) = newTmpVariable(mb, newBatType(TYPE_void,TYPE_bte));
		/* ugh.., second arg not pushed, durty fix */
		pck->argc = 2;
		pck->retc = 2;
		
		if (cur_sort) { 
			pck = pushArgument(mb, pck, getArg(cur_sort,0));
			pck = pushArgument(mb, pck, getArg(cur_sort,1));
		}

		/* merge sort(m.leftjoin(attr)); */
		for(k=1; k < mirror->mi->argc; k++) {
			s = mb->stmt[stmt[k*cnt+i]];

			if (cur_sort || cnt > 1 || has_mirror) {
				q = newInstruction(mb, ASSIGNsymbol);
				setModuleId(q, algebraRef);
				setFunctionId(q, leftjoinRef);
				getArg(q, 0) = newTmpVariable(mb, newBatType(TYPE_oid,tpe));
				q = pushArgument(mb, q, getArg(mirror->mi, k));
				q = pushArgument(mb, q, getArg(s, var));
				pushInstruction(mb, q);

				pck = pushArgument(mb, pck, getArg(q,0));
			} else {
				pck = pushArgument(mb, pck, getArg(mirror->mi, k));
			}
		}
		pushInstruction(mb, pck);
		cur_sort = pck;
	}
	GDKfree(stmt);
	if (has_mirror) {
		/* all sort's done, keep 'which bat order' in mm */
		mirror->mm = cur_sort;

		/* now project */
		return mat_pack_sort_project(mb, p, mat, mtop, mirror_mid);
	}
	pushInstruction(mb, copyInstruction(p));
	return mtop;
}

static void
mat_aggr(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m)
{
	int tp = getArgType(mb,p,0), k;
	int battp = (getModuleId(p)==aggrRef)?newBatType(TYPE_void,tp):tp;
	int v = newTmpVariable(mb, battp);
	InstrPtr r = NULL, s = NULL, q = NULL;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_aggr\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	/* we pack the partitial result */
	r = newInstruction(mb,ASSIGNsymbol);
	setModuleId(r, matRef);
	setFunctionId(r, packRef);
	getArg(r,0) = v;
	for(k=1; k< mat[m].mi->argc; k++) {
		q = newInstruction(mb,ASSIGNsymbol);
		setModuleId(q,getModuleId(p));
		setFunctionId(q,getFunctionId(p));
		getArg(q,0) = newTmpVariable(mb, tp);
		q = pushArgument(mb,q,getArg(mat[m].mi,k));
		pushInstruction(mb,q);
		
		r = pushArgument(mb,r,getArg(q,0));
	}
	pushInstruction(mb,r);

	if (getModuleId(p) == aggrRef) {
		s = newInstruction(mb,ASSIGNsymbol);
		setModuleId(s, algebraRef);
		setFunctionId(s, selectNotNilRef);
		getArg(s,0) = newTmpVariable(mb, newBatType(TYPE_void,tp));
		s = pushArgument(mb, s, getArg(r,0));
		pushInstruction(mb, s);
		r = s;
	}

	s = newInstruction(mb,ASSIGNsymbol);
	setModuleId(s,getModuleId(p));
	setFunctionId(s, aggr_phase2(getFunctionId(p)));
	getArg(s,0) = getArg(p,0);
	s = pushArgument(mb, s, getArg(r,0));
	pushInstruction(mb, s);
}

static int
mat_group_aggr(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m, int g, int ext)
{
	int tp = getArgType(mb,p,0), k;
	char *aggr2 = aggr_phase2(getFunctionId(p));
	InstrPtr ai1 = newInstruction(mb, ASSIGNsymbol), ai2, cur = NULL;
	InstrPtr mi = mat[m].mi;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_group_aggr\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	if (mat[m].type == mat_grp)
		mi = mat[m].mi1;

	setModuleId(ai1,matRef);
	setFunctionId(ai1,packRef);
	getArg(ai1,0) = newTmpVariable(mb, tp);

	if (mi->argc != mat[g].mi1->argc || 
	    mat[g].mi1->argc != mat[ext].mi->argc)
		return 0;
	for(k=1; k<mi->argc; k++) {
		InstrPtr q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tp);
		getArg(q,1) = getArg(mi,k);
		getArg(q,2) = getArg(mat[g].mi1,k);
		getArg(q,3) = getArg(mat[ext].mi,k);
		pushInstruction(mb,q);

		/* pack the result into a mat */
		ai1 = pushArgument(mb,ai1,getArg(q,0));
	}
	pushInstruction(mb, ai1);

 	ai2 = newInstruction(mb, ASSIGNsymbol);
	setModuleId(ai2, aggrRef);
	setFunctionId(ai2, aggr2);
	getArg(ai2,0) = getArg(p,0);

	cur = mat_pack_group(mb, mat, g, ext);
	ai2 = pushArgument(mb, ai2, getArg(ai1, 0));
	ai2 = pushArgument(mb, ai2, getArg(cur, 1));
	ai2 = pushArgument(mb, ai2, getArg(cur, 0));
	pushInstruction(mb, ai2);
	return 1;
}

static int
mat_group_new(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m)
{
	int tp0 = getArgType(mb,p,0);
	int tp1 = getArgType(mb,p,1);
	int k;
	InstrPtr r0 = newInstruction(mb, ASSIGNsymbol);
	InstrPtr r1 = newInstruction(mb, ASSIGNsymbol);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_group_new\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	setModuleId(r0,matRef);
	setFunctionId(r0,newRef);
	getArg(r0,0) = getArg(p,0);

	setModuleId(r1,matRef);
	setFunctionId(r1,newRef);
	getArg(r1,0) = getArg(p,1);
	
	for(k=1; k<mat[m].mi->argc; k++) {
		InstrPtr q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tp0);
		getArg(q,1) = newTmpVariable(mb, tp1);
		getArg(q,2) = getArg(mat[m].mi,k);
		pushInstruction(mb,q);
		propagateProp(mb, q, getArg(mat[m].mi, k));

		/* add result to mat */
		r0 = pushArgument(mb,r0,getArg(q,0));
		r1 = pushArgument(mb,r1,getArg(q,1));
	}
	mtop = mat_add(mat, mtop, r0, r1, mat_grp, 0);
	if (getFunctionId(p) == doneRef)
		mat_pack_group(mb, mat, mtop-1, mtop-1);
	return mtop;
}

static int
mat_group_derive(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int ext, int grp, int attr)
{
	int tp0 = getArgType(mb,p,0);
	int tp1 = getArgType(mb,p,1);
	int i,k;
	InstrPtr r0 = newInstruction(mb, ASSIGNsymbol);
	InstrPtr r1 = newInstruction(mb, ASSIGNsymbol);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_group_derive\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}
	setModuleId(r0,matRef);
	setFunctionId(r0,newRef);
	getArg(r0,0) = getArg(p,0);

	setModuleId(r1,matRef);
	setFunctionId(r1,newRef);
	getArg(r1,0) = getArg(p,1);
	
	/* we need overlapping ranges */
	for(i=1; i<mat[ext].mi->argc; i++) {
	    for(k=1; k<mat[attr].mi->argc; k++) {
 	   	if (Hoverlap(mb, getArg(mat[ext].mi, i), getArg(mat[attr].mi, k))){
			InstrPtr q = copyInstruction(p);
			getArg(q,0) = newTmpVariable(mb, tp0);
			getArg(q,1) = newTmpVariable(mb, tp1);
			getArg(q,2) = getArg(mat[ext].mi,i);
			getArg(q,3) = getArg(mat[grp].mi1,i);
			getArg(q,4) = getArg(mat[attr].mi,k);
			pushInstruction(mb,q);
			propagateProp(mb, q, getArg(mat[ext].mi, i));
	
			/* add result to mat */
			r0 = pushArgument(mb,r0,getArg(q,0));
			r1 = pushArgument(mb,r1,getArg(q,1));
		}
	    }
	}
	mtop = mat_add(mat, mtop, r0, r1, mat_grp, 0);
	if (getFunctionId(p) == doneRef)
		mat_pack_group(mb, mat, mtop-1, mtop-1);
	return mtop;
}

/* TODO cleanup mat_topn and mat_sort using mat_unop */
static int
mat_topn(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m)
{
	int tpe = getArgType(mb,p,0), k;
	InstrPtr pck = NULL, q = NULL;

	/* we pack the partitial result */
	pck = newInstruction(mb,ASSIGNsymbol);
	setModuleId(pck, matRef);
	setFunctionId(pck, packRef);
	getArg(pck,0) = getArg(p,0);
	for(k=1; k< mat[m].mi->argc; k++) {
		q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi,k);
		pushInstruction(mb,q);
		
		/* pack result */
		propagateProp(mb, q, getArg(q,1));
		pck = pushArgument(mb,pck,getArg(q,0));
	}
	return mat_add(mat, mtop, pck, NULL, isSlice(p)?mat_slc:mat_tpn, 0);
}

static int
mat_topn2(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m, int n)
{
	int tpe = getArgType(mb,p,0), k;
	InstrPtr pck = NULL, q = NULL;

	/* we pack the partitial result */
	pck = newInstruction(mb,ASSIGNsymbol);
	setModuleId(pck, matRef);
	setFunctionId(pck, packRef);
	getArg(pck,0) = getArg(p,0);
	assert (mat[m].mi->argc == mat[n].mi->argc);
	for(k=1; k< mat[m].mi->argc; k++) {
		q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi,k);
		getArg(q,2) = getArg(mat[n].mi,k);
		pushInstruction(mb,q);
		
		/* pack result */
		propagateProp(mb, q, getArg(q,1));
		pck = pushArgument(mb,pck,getArg(q,0));
	}
	return mat_add(mat, mtop, pck, NULL, mat_tpn, 0);
}

static int
mat_sort(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m)
{
	int tpe = getArgType(mb,p,0), k;
	InstrPtr pck = NULL, q = NULL;

	/* we pack the partitial result */
	pck = newInstruction(mb,ASSIGNsymbol);
	setModuleId(pck, matRef);
	setFunctionId(pck, packRef);
	getArg(pck,0) = getArg(p,0);
	for(k=1; k< mat[m].mi->argc; k++) {
		q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi,k);
		pushInstruction(mb,q);
		
		/* pack result */
		propagateProp(mb, q, getArg(q,1));
		pck = pushArgument(mb,pck,getArg(q,0));
	}
	return mat_add(mat, mtop, pck, NULL, mat_rdr, 0);
}

static int
mat_sort2(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m, int n)
{
	int tpe = getArgType(mb,p,0), k;
	InstrPtr pck = NULL, q = NULL;

	/* we pack the partitial result */
	pck = newInstruction(mb,ASSIGNsymbol);
	setModuleId(pck, matRef);
	setFunctionId(pck, packRef);
	getArg(pck,0) = getArg(p,0);
	assert (mat[m].mi->argc == mat[n].mi->argc);
	for(k=1; k< mat[m].mi->argc; k++) {
		q = copyInstruction(p);
		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi,k);
		getArg(q,2) = getArg(mat[n].mi,k);
		pushInstruction(mb,q);
		
		/* pack result */
		propagateProp(mb, q, getArg(q,1));
		pck = pushArgument(mb,pck,getArg(q,0));
	}
	return mat_add(mat, mtop, pck, NULL, mat_rdr, 0);
}

static int
mat_union(MalBlkPtr mb, InstrPtr p, mat_t *mat, int mtop, int m, int n)
{
	int k,j;
	InstrPtr r = newInstruction(mb, ASSIGNsymbol);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_group_union\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = getArg(p,0);
	
	if (m >=0 && n >= 0) {
		int tpe = getArgType(mb,p,0);

		/* first make the mat disjoint */
		m = disjoint(mat, &mtop, m, mb);
		for(j=1; j<mat[m].mi->argc; j++) {
			int vm = getArg(mat[m].mi, j);

			for(k=1; k<mat[n].mi->argc; k++) {
				int vn = getArg(mat[n].mi, k);
				if (Hoverlap(mb, vm, vn)) {
					InstrPtr q = copyInstruction(p);

					getArg(q,0) = newTmpVariable(mb, tpe);
					getArg(q,1) = vm;
					getArg(q,2) = vn;
					pushInstruction(mb,q);
					propagateBinProp(mb, q, vm, vn);
	
					vm = getArg(q,0);
				}
			}
			/* add result to mat */
			r = pushArgument(mb, r, vm);
		}
	} else {
		if (m >= 0) {
			for(k=1; k<mat[m].mi->argc; k++) 
				r = pushArgument(mb, r, getArg(mat[m].mi,k));
		} else {
			r = pushArgument(mb, r, getArg(p,1));
		}
		if (n >= 0) {
			for(k=1; k<mat[n].mi->argc; k++) 
				r = pushArgument(mb, r, getArg(mat[n].mi,k));
		} else {
			r = pushArgument(mb, r, getArg(p,2));
		}
	}
	return mat_add(mat, mtop, r, NULL, mat_none, 0);
}

#if 0
static InstrPtr
mat_mark(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m)
{
	int tpe = getArgType(mb,p,0);
	int k,o = newTmpVariable(mb,TYPE_oid);
	InstrPtr q, r = newInstruction(mb, ASSIGNsymbol);
	oid cur = 0;

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_group_union\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	getArg(r,0) = o;
	/* get the mark base id */
	if (p->argc == 3)
		r = pushArgument(mb, r, getArg(p, 2));
	else
		r = pushOid(mb, r, 0);
	pushInstruction(mb, r);
	
	r = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = getArg(p,0);
	
	assert(mat[m].mi1 == NULL);
	for(k=1; k<mat[m].mi->argc; k++) {
		q = copyInstruction(p);
		/* alway switch to non-default mark */
		q->argc = 3;
		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi,k);
		getArg(q,2) = o;
		pushInstruction(mb,q);

		propagateMarkProp(mb, q, getArg(mat[m].mi, k), cur, cur+1);
		cur++;

		/* add result to mat */
		r = pushArgument(mb,r,getArg(q,0));

		/* increment oid */
		if (k < mat[m].mi->argc-1) {
			InstrPtr ca, c, bc;
			
			/* bc = aggr.count */
			bc = newStmt(mb,aggrRef,countRef);
			setArgType(mb,bc,0,TYPE_wrd);
			bc = pushArgument(mb, bc, getArg(mat[m].mi,k));

			/* convert int to oid */
			c = newStmt(mb, calcRef,oidRef);
			setArgType(mb,c,0,TYPE_oid);
			c = pushArgument(mb, c, getArg(bc,0));

			/* ca = calc.add */
			ca = newStmt(mb, calcRef,plusRef);
			getArg(ca, 0) = o;
			ca = pushArgument(mb, ca, o);
			ca = pushArgument(mb, ca, getArg(c, 0));
		}
	}
	return r;
}
#endif

static InstrPtr
mat_mark(MalBlkPtr mb, InstrPtr p, mat_t *mat, int m)
{
	int k, tpe = getArgType(mb,p,0), nr = mat[m].mi->argc-1;
	InstrPtr q, r;
	
	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#MAT optimizer, mat_group_union\n");
		printInstruction(GDKout, mb, 0, p, LIST_MAL_ALL);
	}

	/* We don't use the mark base id */
	
	r = newInstruction(mb, ASSIGNsymbol);
	setModuleId(r,matRef);
	setFunctionId(r,newRef);
	getArg(r,0) = getArg(p,0);
	
	assert(mat[m].mi1 == NULL);
	for(k=1; k<mat[m].mi->argc; k++) {
		q = copyInstruction(p);
		q->argc = 2;
		getArg(q,0) = newTmpVariable(mb, tpe);
		getArg(q,1) = getArg(mat[m].mi,k);
		q = pushInt(mb, q, nr);
		q = pushInt(mb, q, k-1);
		pushInstruction(mb,q);
		propagateMarkProp(mb, q, getArg(mat[m].mi, k), k-1, k);

		r = pushArgument(mb,r,getArg(q,0));
	}
	return r;
}

static int
MATcount(InstrPtr p, mat_t *mat, int mtop)
{
	int j,cnt=0;
	for(j=p->retc; j<p->argc; j++)
		if (isMATalias(getArg(p,j), mat, mtop) >= 0) 
			cnt++;
	return cnt;
}

static int 
group_broken( InstrPtr p, int mtpe, int pack_mirror) 
{
	/* On group by results we limit the statements */
	if (((getModuleId(p) == batRef && getFunctionId(p) == reverseRef) ||
	     (getModuleId(p) == algebraRef && getFunctionId(p) == semijoinRef) ||
	      !pack_mirror) &&
	     (mtpe == mat_grp || mtpe == mat_ext)) 
		return 1;
	return 0;
}


static int
mat_setop(MalBlkPtr mb, InstrPtr p, mat_t *mat, int *Mtop )
{
	int m, n;
	InstrPtr bc;
	int mtop = *Mtop;

	m = isMATalias(getArg(p,1), mat, mtop);
	n = isMATalias(getArg(p,2), mat, mtop);
	if ((bc = mat_apply(mb, p, mat, Mtop, m, n, 0)) != NULL) 
		*Mtop = mat_add(mat, *Mtop, bc, NULL, useMatType(mat, n), 0 );
	return 0;
}

int
OPTmergetableImplementation(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p) 
{
	InstrPtr *old=0, q;
	mat_t *mat;
	int oldtop, fm, i, k, m, mtop=0, slimit;
	int size, match, actions=0, error=0;

	(void) cntxt;
	/* the number of MATs is limited to the variable stack*/
	mat = (mat_t*) GDKzalloc(mb->vtop * sizeof(mat_t));
	if ( mat == NULL)
		return 0;

	old = mb->stmt;
	oldtop= mb->stop;
	slimit = mb->ssize;
	size = (mb->stop * 1.2 < mb->ssize)? mb->ssize:(int)(mb->stop * 1.2);
	mb->stmt = (InstrPtr *) GDKzalloc(size * sizeof(InstrPtr));
	if ( mb->stmt == NULL){
		mb->stmt = old;
		return 0;
	}
	mb->ssize = size;
	mb->stop = 0;

	for( i=0; i<oldtop; i++){
		int n = -1, o = -1;

		p = old[i];
		if (getModuleId(p) == matRef && 
		   (getFunctionId(p) == newRef || getFunctionId(p) == packRef)){
			mtop = mat_add(mat, mtop, p, NULL, mat_none, 1);
			continue;
		}

		if (getModuleId(p) == batcalcRef &&
		   (getFunctionId(p) == mark_grpRef ||
		   getFunctionId(p) == dense_rank_grpRef)) { 
			/* Mergetable cannot handle 
			   order related batcalc operations */
			error++;
			goto fail;
		}
		/*
		 * @-
		 * If the instruction does not contain MAT references it can simply be added.
		 * Otherwise we have to decide on either packing them or replacement.
		 */
		if ((match = MATcount(p, mat, mtop)) == 0) {
			if (p->argc >= 2) {
				if (getFunctionId(p) == markHRef|| 
				    getFunctionId(p) == markTRef) {
					propagateMarkProp(mb, p, getArg(p,1), 0, oid_nil);
				} else if (getFunctionId(p) == leftjoinRef|| 
					   getFunctionId(p) == joinRef|| 
				           getFunctionId(p) == kunionRef) {
					propagateBinProp(mb, p, getArg(p,1), getArg(p,2));
				} else {
					propagateProp(mb, p, getArg(p,1));
				}
			}
			pushInstruction(mb, copyInstruction(p));
			continue;
		}
		/*
		 * @-
		 * Here we handle horizontal aligned mats. This information is passed using
		 * the properties hlb <= x < hub.
		 * So if this is available, we can simplify
		 * batcalc operations and for fetch joins we can use this information to do
		 * per part joins only.
		 *
		 * Also we should translate the mirror().join() (a groupby attribute) into
		 * UNION(mirror().join()).
		 */

		/* only handle simple joins, ie not range/band joins */
		/* For range/band joins (argc == 4), the propagation of oids
		   is different, ie result-head equals head-1st arg, 	
				    result-tail equals head-2nd/3rd arg */
		  
		/* TODO:
		   If a value join with mats on both sides fails (ie unknown
		   how to handle) we should bail out, ie stop any further
		   processing of any mats. This is needed because the needed 
		   mas-crossproduct handling of projections fails. 
                 */
		if (match > 0 && match <= 2 && isMatJoinOp(p) && 
		   (p->argc == 3 || (p->argc == 4 && getFunctionId(p) == thetajoinRef))) {
			int om, tpe = mat_none;
		
		   	om = m = isMATalias(getArg(p,1), mat, mtop);
			if (om < 0) { /* range join with parts on the right */
				error++;
				goto fail;
			}

		   	n = isMATalias(getArg(p,2), mat, mtop);
			if (isProjection(p) && m >= 0 && mat_is_topn(mat[m].type))
				tpe = mat_tpn;
			if (isProjection(p) && m >= 0 && mat_is_orderby(mat[m].type))
				tpe = mat_rdr;

			if ((m = mat_join(mb, p, mat, mtop, m, n)) < 0) {
				error++;
				goto fail;
			} else
				mtop = m;
			/* after topn projection we should merge */
			/* slice marks the end of a sequence of topn's */
			if (tpe == mat_tpn && (getFunctionId(old[i+1]) == sliceRef || mat[om].type == mat_slc))
				mtop = mat_pack_topn(mb, p, mat, mtop, om);
			else if (tpe == mat_tpn && mat[om].mm) 
				mtop = mat_pack_topn_project(mb, p, mat, mtop, om);

			/* after sort projection we should mat.merge */
			if (tpe == mat_rdr && !mat[om].mm)
				mtop = mat_pack_sort(mb, p, mat, mtop, om, 1);
			else if (tpe == mat_rdr && mat[om].mm) 
				mtop = mat_pack_sort_project(mb, p, mat, mtop, om);
			actions++;
			continue;
		}
		/* all map operations assume aligned bats */
		if (match >= 1 && all_mats_and_aligned(mb, p, mat, mtop) &&
		   isMapOp(p)) {
			mtop = mat_map(mb, p, mat, mtop); 
			actions++;
			continue;
		}
		if (match >= 1 &&
		    getModuleId(p) == algebraRef &&
		    getFunctionId(p)== kunionRef) {
			m = isMATalias(getArg(p,1), mat, mtop);
			n = isMATalias(getArg(p,2), mat, mtop);
			mtop = mat_union(mb, p, mat, mtop, m, n);
			actions++;
			continue;
		} 
		if (match >= 1 && 
		    (getModuleId(p) == algebraRef && 
		     ((getFunctionId(p) == semijoinRef && match == 2) ||
		      (getFunctionId(p) == kdifferenceRef)))) { 
			i += mat_setop(mb, p, mat, &mtop); 
			actions++;
			continue;
		}
		/*
		 * @-
		 * Now we handle group, derive and aggregation statements.
		 */
		if (match == 1 && p->argc == 3 && getModuleId(p) == groupRef && 
		   (getFunctionId(p) == newRef || getFunctionId(p) == doneRef) && 
	 	   ((m=isMATalias(getArg(p,2), mat, mtop)) >= 0)) {
			if (mat[m].mi1 || mat[m].mm) {
				/* group on finished group is fine */
				if (mat[m].mm) {
					pushInstruction(mb, copyInstruction(p));
					actions++;
					continue;
				}
				/* two phase group.new on group result */
				error++;
				goto fail;
			}
			mtop = mat_group_new(mb, p, mat, mtop, m);
			actions++;
			continue;
		}
		if (match == 3 && p->argc == 5 && getModuleId(p) == groupRef && 
		    (getFunctionId(p) == deriveRef || getFunctionId(p) == doneRef )
&&
		   ((m=isMATalias(getArg(p,2), mat, mtop)) >= 0) &&
		   ((n=isMATalias(getArg(p,3), mat, mtop)) >= 0) &&
		   ((o=isMATalias(getArg(p,4), mat, mtop)) >= 0)) {
		
			/* Found a derive after an aggr statement (distinct). */
			if (mat[m].mm) {
				error++;
				goto fail;
			}

			mtop = mat_group_derive(mb, p, mat, mtop, m, n, o);
			actions++;
			continue;
		}
		if (match == 3 && getModuleId(p) == aggrRef && p->argc == 4 &&
		   (getFunctionId(p) == countRef ||
		    getFunctionId(p) == count_no_nilRef ||
		    getFunctionId(p) == minRef ||
		    getFunctionId(p) == maxRef ||
		    getFunctionId(p) == sumRef ||
		    getFunctionId(p) == prodRef) &&
		   ((m=isMATalias(getArg(p,1), mat, mtop)) >= 0) &&
		   ((n=isMATalias(getArg(p,2), mat, mtop)) >= 0) &&
		   ((o=isMATalias(getArg(p,3), mat, mtop)) >= 0)) {
			if (!mat_group_aggr(mb, p, mat, m, n, o)){
				error++;
				goto fail;
			}
			actions++;
			continue;
		}
		if (match == 3 && getModuleId(p) == aggrRef && p->argc == 4)
			assert(0); 
		/*
		 * @-
		 * Aggregate handling is a prime target for optimization.
		 * The simple cases are dealt with first.
		 * Handle the rewrite v:=aggr.count(b) and sum()
		 * And the min/max is as easy
		 */
		if (match == 1 && p->argc == 2 &&
		   ((getModuleId(p)==aggrRef &&
			(getFunctionId(p)== countRef || 
			 getFunctionId(p)== count_no_nilRef || 
			 getFunctionId(p)== minRef ||
			 getFunctionId(p)== maxRef ||
			 getFunctionId(p)== sumRef ||
		    	 getFunctionId(p) == prodRef)) ||
		    (getModuleId(p) == algebraRef &&
		     getFunctionId(p) == kuniqueRef)) &&
			(m=isMATalias(getArg(p,1), mat, mtop)) >= 0) {
			mat_aggr(mb, p, mat, m);
			actions++;
			continue;
		} 
		if (match == 1 && p->argc == 3 && isTopn(p) &&
		   (m=isMATalias(getArg(p,1), mat, mtop)) >= 0 &&
		   mat[m].type == mat_none) {
			mtop = mat_topn(mb, p, mat, mtop, m);
			actions++;
			continue;
		}
		if (match == 1 && p->argc == 4 && isSlice(p) &&
		   (m=isMATalias(getArg(p,1), mat, mtop)) >= 0 &&
		   mat[m].type == mat_none) {
			mtop = mat_topn(mb, p, mat, mtop, m);
			actions++;
			continue;
		}
		if (match == 2 && p->argc == 4 && isTopn(p) &&
		   (m=isMATalias(getArg(p,1), mat, mtop)) >= 0 &&
		   (n=isMATalias(getArg(p,2), mat, mtop)) >= 0 &&
		    mat_is_topn(mat[n].type) &&
		    mat_is_topn(mat[m].type) && !mat[m].mm) {
			mtop = mat_topn2(mb, p, mat, mtop, m, n);
			actions++;
			continue;
		}
		if (match == 1 && p->argc == 2 && isOrderby(p) &&
		   (m=isMATalias(getArg(p,1), mat, mtop)) >=0 &&
		   mat[m].type == mat_none) {
			mtop = mat_sort(mb, p, mat, mtop, m);
			actions++;
			continue;
		}
		/* TODO: grp before sorting, isn't handled */
		if (match == 1 && p->argc == 2 && isOrderby(p) &&
		   (m=isMATalias(getArg(p,1), mat, mtop)) >=0 &&
		   mat[m].type == mat_grp) {
			assert(mat[m].mm); /* should be packed */
			error++;
			goto fail;
		}
		if (match == 2 && p->argc == 3 && isOrderby(p) &&
		   (m=isMATalias(getArg(p,1), mat, mtop)) >= 0 &&
		   (n=isMATalias(getArg(p,2), mat, mtop)) >= 0 &&
		    /*mat_is_orderby(mat[n].type) &&*/
		    mat_is_orderby(mat[m].type) && !mat[m].mm) {
			mtop = mat_sort2(mb, p, mat, mtop, m, n);
			actions++;
			continue;
		}
		if (match == 1 && p->argc == 4 && getModuleId(p) == sqlRef && 
		    getFunctionId(p) == resultSetRef && 
		   (m=isMATalias(getArg(p,3), mat, mtop)) >= 0 &&
		    mat_is_orderby(mat[m].type) && !mat[m].mm) {
			mtop = mat_pack_sort(mb, p, mat, mtop, m, 0);
			actions++;
			continue;
		}
		/*
		 * @-
		 * The slice operation can also be piggy backed onto the mat.pack using it
		 * as a property of the MAT. Pushing it through
		 * would be feasible as well, provided the start of the slice is a constant 0.
		 */
		if (match > 0 && getModuleId(p) == algebraRef &&
				getFunctionId(p) == sliceRef &&
				(m = isMATalias(getArg(p, 1), mat, mtop)) >= 0)
		{
			assert(0);
			/* inject new mat.pack() operation */
			q = MATpackAll(mb, NULL, mat, m, &mtop);
			/* rename mat.pack() to mat.slice() */
			setFunctionId(q, sliceRef);
			/* insert bounds from algebra.slice() into mat.slice() */
			/* (setArgument() seems to shift the remaining arguments,
			 *  i.e., insert a new argument, not overwrite an existing one) */
			q = setArgument(mb, q, 1, getArg(p, 2));
			q = setArgument(mb, q, 2, getArg(p, 3));
			/* reuse result variable of algebra.slice() for mat.slice() */
			/* (we do not explicitly keep, and thus drop, the original algebra.slice()) */
			getArg(q, 0) = getArg(p, 0);

			actions++;
			continue;
		}
		/*
		 * @-
		 * The mark operators are a special case of apply on parts as we need to
		 * correct the mark base oid's
		 */
		if (match == 1 && 
		    getModuleId(p) == algebraRef && 
		    (getFunctionId(p) == markTRef ||
		     getFunctionId(p) == markHRef)) { 
			InstrPtr mark;

			m = isMATalias(getArg(p,1), mat, mtop);
 			mark = mat_mark(mb, p, mat, m);
			mtop = mat_add(mat, mtop, mark, NULL, useMatType(mat, m), 0);
			actions++;
			continue;
		}
		/*
		 * @-
		 * Pack MAT arguments, except one, to limit plan explosion.
		 * The preferred partitioned one is the first argment as it
		 * often reflects a base table.
		 * Look at the depth of the MAT definition to limit the explosion.
		 */
		for( fm= p->argc-1; fm>p->retc ; fm--)
			if ((m=isMATalias(getArg(p,fm), mat, mtop)) >= 0)
				break;
		/*
		 * @-
		 * Not all instructions can be replaced by the sequence. We have to
		 * group them and check for them individually.
		 */
		if (match == 1 && isDiffOp(p) && fm == 1 && 
		   (m=isMATalias(getArg(p,fm), mat, mtop)) >= 0){
			InstrPtr r;

			if ((r = mat_apply1(mb, p, mat, m, fm, 0)) != NULL)
				mtop = mat_add(mat, mtop, r, NULL, mat_none, 0);
			actions++;
			continue;
		}
		if (match == 1 && 
		   isUpdateInstruction(p) && getModuleId(p) == sqlRef && 
		   (m=isMATalias(getArg(p,fm), mat, mtop)) >= 0) {
			mat_update(mb, p, mat, m, fm);
			actions++;
			continue;
		}
		if (match == 1 && 
		   isFragmentGroup(p) && 
		   (m=isMATalias(getArg(p,fm), mat, mtop)) >= 0){
			int pack_mirror = 0;
			InstrPtr r;

			OPTDEBUGmergetable mnstr_printf(GDKout, "# %s.%s\n", getModuleId(p), getFunctionId(p));

			if (getFunctionId(p) == mirrorRef && 
	    			mat[m].type == mat_grp/* && mat[m].mm */) {
				assert(mat[m].mm != NULL);
				pack_mirror = 1;
			}

			if (group_broken(p, mat[m].type, pack_mirror)) {
				error++;
				goto fail;
			}

			if ((r = mat_apply1(mb, p, mat, m, fm, 0)) != NULL)
				mtop = mat_add(mat, mtop, r, NULL, useMatType(mat, m), 0);

			/* packed group should include the mirror statement */
			if (pack_mirror) {
				if (mat[m].mv1 == p->argv[1])  {
					assert(0);
					mat[mtop-1].type = mat_grp;
				} else
					mat[mtop-1].type = mat_ext;
				mat_pack_group(mb, mat, m, mtop-1);
			}
			actions++;
			continue;
		} 
		/*
		 * @-
		 * All other instructions should be checked for remaining MAT dependencies.
		 * It requires MAT materialization.
		 */
		OPTDEBUGmergetable mnstr_printf(GDKout, "# %s.%s %d\n", getModuleId(p), getFunctionId(p), match);


		for (k = p->retc; k<p->argc; k++) {
			if((m=isMATalias(getArg(p,k), mat, mtop)) >= 0){
				if (MATpackAll2(mb, NULL, mat, m, &mtop) < 0){
					error++;
					goto fail;
				}
				actions++;
			}
		}
		pushInstruction(mb, copyInstruction(p));
		if (p->argc >= 2)
			propagateProp(mb, p, getArg(p,1));
	}
	/*
	 * @-
	 * As a final optimization, we could remove the mal.new definitions,
	 * because they are not needed for the execution.
	 * For the time being, they are no-ops.
	 */
	(void) stk; 
	chkTypes(cntxt->fdout, cntxt->nspace,mb, TRUE);

	OPTDEBUGmergetable {
		mnstr_printf(GDKout,"#Result of multi table optimizer\n");
		(void) optimizerCheck(cntxt,mb,"merge test",1,0,0);
		printFunction(GDKout, mb, 0, LIST_MAL_ALL);
	}

	if ( mb->errors == 0) {
		for(i=0; i<slimit; i++)
			if (old[i]) 
				freeInstruction(old[i]);
		GDKfree(old);
	}

fail:
	if( error || mb->errors){
		actions= 0;
		OPTDEBUGmergetable 
			mnstr_printf(GDKout, "## %s.%s\n", getModuleId(p), getFunctionId(p));

		for(i=0; i<mb->stop; i++)
			if (mb->stmt[i])
				freeInstruction(mb->stmt[i]);
		GDKfree(mb->stmt);
		mb->stmt = old;
		mb->ssize = slimit;
		mb->stop = oldtop;
		for(i=0; i<mb->stop; i++) {
			InstrPtr p = mb->stmt[i];
			if (p && getModuleId(p) == matRef && getFunctionId(p) == newRef){
				/* simply drop this function, for the base binding is available */
				p->token = NOOPsymbol;
			}
		}
		OPTDEBUGmergetable mnstr_printf(GDKout,"Result of multi table optimizer FAILED\n");
	}
	DEBUGoptimizers
		mnstr_printf(cntxt->fdout,"#opt_mergetable: %d merge actions\n",actions);
	for (i =0; i<mtop; i++) {
		if (mat[i].pushed == 1)
			continue;
		if (mat[i].mi && mat[i].mi->token != NOOPsymbol)
			freeInstruction(mat[i].mi);
		if (mat[i].mi1 && mat[i].mi1->token != NOOPsymbol)
			freeInstruction(mat[i].mi1);
	}
	GDKfree(mat);
	return actions;
}

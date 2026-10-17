/*
** Garbage collector.
** Copyright (C) 2005-2021 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "luajit.h"
#ifdef __e2k__
#include "e2kintrin.h"
#endif

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

#ifdef __e2k__
/* Mark a TValue (if needed). */
#define gc_marktv_volatile(g, tv) \
  { lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tviswhite_volatile(tv)) gc_mark(g, gcV(tv)); }
#endif

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lj_assertG(iswhite(o), "mark of non-white object");
  lj_assertG(!isdead(g, o), "mark of dead object");
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    gc_markobj(g, tabref(gco2ud(o)->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lj_assertG(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE,
	       "bad GC type %d", gct);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lj_assertG(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv,
	       "broken upvalue chain");
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Mark userdata in mmudata list. */
static void gc_mark_mmudata(global_State *g)
{
  GCobj *root = gcref(g->gc.mmudata);
  GCobj *u = root;
  if (u) {
    do {
      u = gcnext(u);
      makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Link userdata to the end of the mmudata list (it is unlinked already). */
static void gc_link_mmudata(global_State *g, GCobj *o)
{
  if (gcref(g->gc.mmudata)) {  /* Link to end of mmudata list. */
    GCobj *root = gcref(g->gc.mmudata);
    setgcrefr(o->gch.nextgc, root->gch.nextgc);
    setgcref(root->gch.nextgc, o);
    setgcref(g->gc.mmudata, o);
  } else {  /* Create circular list. */
    setgcref(o->gch.nextgc, o);
    setgcref(g->gc.mmudata, o);
  }
}

/* GlobalWar: what a walk over a userdata list saw. */
typedef struct GCgwwalk {
  size_t objs, white, gc;  /* Udata walked, found white, moved to mmudata. */
} GCgwwalk;

/* Separate userdata to be finalized to mmudata list: from *p up to end. */
static size_t gc_separate_list(global_State *g, GCRef *p, GCobj *end,
			       int all, GCgwwalk *w)
{
  size_t m = 0;
  GCobj *o;
  while ((o = gcref(*p)) != end) {
    w->objs++;
    if (iswhite(o)) w->white++;
    if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
      markfinalized(o);  /* Done, as there's no __gc metamethod. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */
      m += sizeudata(gco2ud(o));
      w->gc++;
      markfinalized(o);
      *p = o->gch.nextgc;
      gc_link_mmudata(g, o);
    }
  }
  return m;
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  GCgwwalk w = {0, 0, 0};
  /* All userdata, both lists of luaJIT_gw_finobj() included. */
  return gc_separate_list(g, &mainthread(g)->nextgc, NULL, all, &w);
}

/* -- GlobalWar: the finobj list (see lj_gc.h) ----------------------------- */

/* Grow a vector of pointers without raising an error. NULL: out of memory
** (the vector is unchanged), else the new one.
*/
static void *gc_gw_grow(global_State *g, void *v, MSize *sz)
{
  MSize nsz = *sz ? *sz * 2 : 64;
  void *p = g->allocf(g->allocd, v, *sz * sizeof(void *), nsz * sizeof(void *));
  if (p == NULL) return NULL;
  g->gc.total = (g->gc.total - *sz * sizeof(void *)) + nsz * sizeof(void *);
  *sz = nsz;
  return p;
}

/* Watch a metatable without __gc of a plain userdata. 0: out of memory. */
static int gc_gw_watchmt(global_State *g, GCgwfin *f, GCtab *mt)
{
  if ((mt->marked & LJ_GC_GW_MTWATCH)) return 1;
  if (f->nmt == f->szmt) {
    GCtab **v = (GCtab **)gc_gw_grow(g, f->mt, &f->szmt);
    if (v == NULL) return 0;
    f->mt = v;
  }
  f->mt[f->nmt++] = mt;
  mt->marked |= LJ_GC_GW_MTWATCH;
  f->watched++;
  return 1;
}

/* Flag a plain udata with __gc for the next sweep. 0: out of memory. */
static int gc_gw_addpend(global_State *g, GCgwfin *f, GCudata *ud)
{
  if (f->npend == f->szpend) {
    GCudata **v = (GCudata **)gc_gw_grow(g, f->pend, &f->szpend);
    if (v == NULL) return 0;
    f->pend = v;
  }
  f->pend[f->npend++] = ud;
  gwudflags(ud) = LJ_GW_UD_PENDING;
  f->pending++;
  return 1;
}

/* Stop watching all metatables. */
static void gc_gw_unwatchall(GCgwfin *f)
{
  MSize i;
  for (i = 0; i < f->nmt; i++)
    f->mt[i]->marked &= (uint8_t)~LJ_GC_GW_MTWATCH;
  f->nmt = 0;
}

/* Move userdata o (*p == o) to the head of the finobj list. */
static void gc_gw_tofinobj(GCudata *a, GCRef *p, GCobj *o)
{
  setgcrefr(*p, o->gch.nextgc);  /* Unlink. */
  setgcrefr(o->gch.nextgc, a->nextgc);
  setgcref(a->nextgc, o);
  gwudflags(gco2ud(o)) = LJ_GW_UD_FINOBJ;
}

/* A userdata got a metatable (lua_setmetatable() and the libraries). */
void lj_gc_gw_udmeta(lua_State *L, GCudata *ud)
{
  global_State *g = G(L);
  GCudata *a = gc_gw_anchor(g);
  GCgwfin *f;
  GCtab *mt;
  if (a == NULL || !(f = gwfin(a))->mode || isfinalized(ud) ||
      (gwudflags(ud) & (LJ_GW_UD_FINOBJ|LJ_GW_UD_PENDING)))
    return;  /* Off, never finalized again, or in the finobj list already. */
  mt = tabref(ud->metatable);
  if (mt == NULL)
    return;  /* The hook sees the metatable it may get later. */
  if (lj_meta_fastg(g, mt, MM_gc)) {
    GCobj *o = obj2gco(ud);
    GCRef *p = &f->b->nextgc;
    if (gcref(*p) == o) {  /* The head of the plain list: move it now. */
      /* The sweep may be right at it: let it go on with the next one. */
      if (mref(g->gc.sweep, GCRef) == &o->gch.nextgc)
	setmref(g->gc.sweep, p);
      gc_gw_tofinobj(a, p, o);
      /* The finobj list may be swept already: no black objects after it. */
      if (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
	makewhite(g, o);
      f->moved_head++;
    } else if (gc_gw_addpend(g, f, ud)) {
      /* The next sweep moves it (gc_sweep). */
    } else {
      f->dirty = 1;  /* Out of memory: the next atomic walks all. */
      f->oom++;
    }
  } else if (!gc_gw_watchmt(g, f, mt)) {
    f->dirty = 1;
    f->oom++;
  }
}

/* lj_udata_free() of an anchor. */
void lj_gc_gw_freeanchor(global_State *g, GCudata *ud)
{
  if (ud->len == sizeof(GCgwfin)) {  /* A: free its vectors. */
    GCgwfin *f = gwfin(ud);
    if (f->szmt) lj_mem_freevec(g, f->mt, f->szmt, GCtab *);
    if (f->szpend) lj_mem_freevec(g, f->pend, f->szpend, GCudata *);
  }
}

/* The plain list when it may hold udata with __gc: as gc_separate_list(),
** and a live one with __gc moves to the finobj list, the metatables of the
** others are watched again.
*/
static size_t gc_gw_walkplain(global_State *g, GCudata *a, GCgwwalk *w,
			      size_t *moved)
{
  GCgwfin *f = gwfin(a);
  GCRef *p = &f->b->nextgc;
  size_t m = 0;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    GCudata *ud = gco2ud(o);
    GCtab *mt = tabref(ud->metatable);
    w->objs++;
    if (isfinalized(ud)) {
      if (iswhite(o)) w->white++;
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else if (iswhite(o)) {
      w->white++;
      gwudflags(ud) = 0;
      if (!lj_meta_fastg(g, mt, MM_gc)) {
	markfinalized(o);  /* Done, as there's no __gc metamethod. */
	p = &o->gch.nextgc;
      } else {  /* Otherwise move userdata to be finalized to mmudata list. */
	m += sizeudata(ud);
	w->gc++;
	markfinalized(o);
	*p = o->gch.nextgc;
	gc_link_mmudata(g, o);
      }
    } else if (lj_meta_fastg(g, mt, MM_gc)) {
      gc_gw_tofinobj(a, p, o);  /* Live with __gc: to the finobj list. */
      (*moved)++;
    } else {
      gwudflags(ud) = 0;
      if (mt && !gc_gw_watchmt(g, f, mt)) {
	f->dirty = 1;  /* Out of memory: walk all again next time. */
	f->oom++;
      }
      p = &o->gch.nextgc;
    }
  }
  return m;
}

/* GlobalWar: what gc_separate_atomic() did. */
typedef struct GCgwsep {
  GCgwwalk fin, plain;	/* The finobj list; the plain list (mode 0: all). */
  int mode, full;	/* full: 1 forced, 2 a watched metatable got __gc. */
  size_t moved;		/* Full walk: live udata moved to the finobj list. */
  size_t pending, kept;	/* PENDING udata; of them white, kept a cycle. */
} GCgwsep;

/* lj_gc_separateudata(g, 0) of the atomic phase. */
static size_t gc_separate_atomic(global_State *g, GCgwsep *s)
{
  GCudata *a = gc_gw_anchor(g);
  GCgwfin *f;
  size_t m;
  MSize i;
  if (a == NULL || !(f = gwfin(a))->mode)  /* All userdata, as upstream. */
    return gc_separate_list(g, &mainthread(g)->nextgc, NULL, 0, &s->plain);
  s->mode = 1;
  s->pending = f->npend;
  if (f->dirty) {
    s->full = 1;
  } else {
    for (i = 0; i < f->nmt; i++)  /* Did a watched metatable get __gc? */
      if (lj_meta_fastg(g, f->mt[i], MM_gc)) {
	s->full = 2;
	break;
      }
  }
  m = gc_separate_list(g, &a->nextgc, obj2gco(f->b), 0, &s->fin);
  if (s->full) {
    gc_gw_unwatchall(f);  /* The walk watches the metatables again. */
    f->dirty = 0;
    f->full_walks++;
    if (s->full == 2) f->full_mt++;
    m += gc_gw_walkplain(g, a, &s->plain, &s->moved);
    f->moved_full += s->moved;
  } else {
    for (i = 0; i < f->npend; i++) {
      GCobj *o = obj2gco(f->pend[i]);
      /* Unreachable, but its __gc needs the finobj list: keep it for one
      ** more cycle, the sweep moves it there.
      */
      if ((gwudflags(gco2ud(o)) & LJ_GW_UD_PENDING) && iswhite(o)) {
	gc_mark(g, o);
	s->kept++;
      }
    }
    f->kept += s->kept;
  }
  f->npend = 0;  /* The sweep after this atomic phase moves them all. */
  return m;
}

/* After all marking: stop watching the metatables about to be freed. */
static void gc_gw_prunemt(global_State *g)
{
  GCudata *a = gc_gw_anchor(g);
  GCgwfin *f;
  MSize i, j;
  if (a == NULL || !(f = gwfin(a))->mode) return;
  for (i = j = 0; i < f->nmt; i++) {
    GCtab *mt = f->mt[i];
    if (iswhite(obj2gco(mt)) && !(mt->marked & LJ_GC_FIXED))
      mt->marked &= (uint8_t)~LJ_GC_GW_MTWATCH;
    else
      f->mt[j++] = mt;
  }
  f->nmt = j;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      CTState *cts = ctype_ctsG(g);
      if (cts && cts->finalizer == t) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
#ifdef __e2k__
    for (i = 0; i < asize; i++) {
      const TValue *tv = arrayslot(t, i);

      // Prefetch next cache line in array.
      __builtin_e2k_prefetch(((const uint8_t *) tv) + 64, E2K_HINT_T0);

      // Prefetch next iteration pointer.
      if ((i + 1) < asize && tvisgcv(&tv[1])) {
        __builtin_e2k_prefetch(gcV(&tv[1]), E2K_HINT_T0);
      }

      gc_marktv_volatile(g, tv);
    }
#else
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
#endif
  }
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lj_assertG(!tvisnil(&n->key), "mark of nil key in non-empty slot");
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lj_assertG(traceno != G2J(g)->cur.traceno, "active trace escaped");
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  lj_assertG(isgray(o), "propagation of non-gray object");
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lj_assertG(0, "bad GC type %d", gct);
    return 0;
#endif
  }
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
#ifdef __e2k__
    __builtin_e2k_prefetch(gcref(o->gch.nextgc), E2K_HINT_T0);
#endif
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      if (LJ_UNLIKELY(o->gch.gct == ~LJ_TUDATA) &&
	  LJ_UNLIKELY(gwudflags(gco2ud(o)) & LJ_GW_UD_PENDING)) {
	/* GlobalWar: a plain udata with __gc. p is in the plain list, so the
	** finobj list before it is swept already: move it there.
	*/
	GCudata *a = gc_gw_anchor(g);
	if (a) {
	  gc_gw_tofinobj(a, p, o);
	  gwfin(a)->moved_sweep++;
	  continue;
	}
      }
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive object");
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(global_State *g, GCobj *o)
{
  UNUSED(g);
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((t->marked & LJ_GC_WEAK), "clear of non-weak table");
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1))
	  setnilV(tv);
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
  }
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  GCSize oldt = g->gc.threshold;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g);
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  top = L->top;
  copyTV(L, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(L, top, o, ~o->gch.gct);
  L->top = top+1;
  errcode = lj_vm_pcall(L, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  lj_assertG(tvref(g->jit_base) == NULL, "finalizer called on trace");
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Add userdata back to the main userdata list and make it white. */
  {
    /* GlobalWar: finalized, so to the plain list of luaJIT_gw_finobj(). */
    GCudata *a = gc_gw_anchor(g);
    GCRef *head = a ? &gwfin(a)->b->nextgc : &mainthread(g)->nextgc;
    setgcrefr(o->gch.nextgc, *head);
    setgcref(*head, o);
    gwudflags(gco2ud(o)) = 0;
  }
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *node = noderef(t->node);
    ptrdiff_t i;
    setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
    for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
      if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
	GCobj *o = gcV(&node[i].key);
	TValue tmp;
	makewhite(g, o);
	o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
	copyTV(L, &tmp, &node[i].val);
	setnilV(&node[i].val);
	gc_call_finalizer(g, L, &tmp, o);
      }
  }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i, strmask;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
  strmask = g->strmask;
  for (i = 0; i <= strmask; i++)  /* Free all string hash chains. */
    gc_fullsweep(g, &g->strhash[i]);
}

/* -- GlobalWar: GC diagnostics (a local patch, see luajit.h) -------------- */

/* NULL: off (the default), atomic() runs as in upstream. */
static luaJIT_gw_atomic *gc_gw_stats;
static luaJIT_gw_clock gc_gw_clock;
static global_State *gc_gw_g;  /* The state the stats are recorded for. */

/* Slots of a table: array + hash. */
#define gc_gw_tabslots(t) \
  ((size_t)(t)->asize + ((t)->hmask ? (size_t)(t)->hmask + 1 : 0))

/* gc_propagate_gray() that counts what it propagates. */
static size_t gc_gw_propagate(global_State *g, luaJIT_gw_gcpart *p)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL) {
    GCobj *o = gcref(g->gc.gray);
    if (o->gch.gct == ~LJ_TTAB) {
      p->tabs++;
      p->tabslots += gc_gw_tabslots(gco2tab(o));
    } else if (o->gch.gct == ~LJ_TTHREAD) {
      p->threads++;
      p->stackslots += gco2th(o)->stacksize;
    }
    m += propagatemark(g);
    p->objs++;
  }
  p->bytes += m;
  return m;
}

/* gc_separate_atomic() that reports what it walked. */
static size_t gc_gw_separateudata(global_State *g, luaJIT_gw_atomic *s)
{
  GCgwsep sep;
  size_t m;
  memset(&sep, 0, sizeof(sep));
  m = gc_separate_atomic(g, &sep);
  s->separate.objs = sep.fin.objs + sep.plain.objs;
  s->ud_white = sep.fin.white + sep.plain.white;
  s->ud_gc = sep.fin.gc + sep.plain.gc;
  s->separate.bytes = m;
  s->fin_mode = sep.mode;
  s->fin_full = sep.full;
  s->fin_walked = sep.fin.objs;
  s->plain_walked = sep.plain.objs;
  s->fin_moved = sep.moved;
  s->fin_pending = sep.pending;
  s->fin_kept = sep.kept;
  return m;
}

/* gc_clearweak() that counts the tables, their slots and the cleared ones. */
static void gc_gw_clearweak(global_State *g, GCobj *o, luaJIT_gw_atomic *s)
{
  UNUSED(g);
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((t->marked & LJ_GC_WEAK), "clear of non-weak table");
    s->clearweak.objs++;
    s->clearweak.tabs++;
    s->clearweak.tabslots += gc_gw_tabslots(t);
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1)) {
	  setnilV(tv);
	  s->weak_cleared++;
	}
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1))) {
	  setnilV(&n->val);
	  s->weak_cleared++;
	}
      }
    }
    o = gcref(t->gclist);
  }
}

/* atomic() with the time and the counts of each part. Keep in sync with it. */
static void atomic_gw(global_State *g, lua_State *L, luaJIT_gw_atomic *st)
{
  luaJIT_gw_clock clk = gc_gw_clock;
  luaJIT_gw_atomic s;
  unsigned long long t0, t1, t2;
  size_t udsize;
  GCupval *uv;
  GCobj *o;

  memset(&s, 0, sizeof(s));
  s.total = (size_t)g->gc.total;
  s.strnum = g->strnum;
  s.strslots = (size_t)g->strmask + 1;
  s.mainstack = mainthread(g)->stacksize;
  s.curstack = L->stacksize;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv))
    s.uv.objs++;

  t0 = clk();
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  t1 = clk(); s.uv.t = t1 - t0;
  gc_gw_propagate(g, &s.left);  /* Propagate any left-overs. */
  t2 = clk(); s.left.t = t2 - t1;

  for (o = gcref(g->gc.weak); o; o = gcref(gco2tab(o)->gclist))
    s.remark_weak++;
  t1 = clk();
  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lj_assertG(!iswhite(obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_gw_propagate(g, &s.remark);  /* Propagate all of the above. */
  t2 = clk(); s.remark.t = t2 - t1;

  for (o = gcref(g->gc.grayagain); o; o = gcref(o->gch.gclist)) {
    if (o->gch.gct == ~LJ_TTAB) {
      s.ga_tabs++;
      s.ga_tabslots += gc_gw_tabslots(gco2tab(o));
    } else if (o->gch.gct == ~LJ_TTHREAD) {
      s.ga_threads++;
      s.ga_stackslots += gco2th(o)->stacksize;
    }
  }
  t1 = clk();
  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_gw_propagate(g, &s.grayagain);  /* Propagate it. */
  t2 = clk(); s.grayagain.t = t2 - t1;

  /* Separate userdata to be finalized. */
  udsize = gc_gw_separateudata(g, &s);
  t1 = clk(); s.separate.t = t1 - t2;
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_gw_propagate(g, &s.mmudata);  /* And propagate the marks. */
  t2 = clk(); s.mmudata.t = t2 - t1;
  gc_gw_prunemt(g);  /* GlobalWar: unwatch the dead metatables. */
  {
    GCudata *a = gc_gw_anchor(g);
    s.fin_mts = (a && gwfin(a)->mode) ? gwfin(a)->nmt : 0;
  }

  /* All marking done, clear weak tables. */
  gc_gw_clearweak(g, gcref(g->gc.weak), &s);
  t1 = clk(); s.clearweak.t = t1 - t2;

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
  t2 = clk(); s.t_rest = t2 - t1;
  s.t_total = t2 - t0;

  if (gcref(g->gc.mmudata)) {  /* Count the circular mmudata list. */
    GCobj *root = gcref(g->gc.mmudata);
    o = root;
    do {
      o = gcnext(o);
      s.mm_count++;
    } while (o != root);
  }
  s.estimate = (size_t)g->gc.estimate;
  s.seq = st->seq + 1;
  *st = s;
}

LUA_API void luaJIT_gw_gcstats(lua_State *L, luaJIT_gw_atomic *st,
			       luaJIT_gw_clock clock)
{
  if (st && clock) {
    gc_gw_clock = clock;
    gc_gw_g = G(L);
    gc_gw_stats = st;
  } else {
    gc_gw_stats = NULL;
    gc_gw_g = NULL;
  }
}

/* Size of a GC object, as the GC counts it (cdata: the header only). */
static size_t gc_gw_objsize(GCobj *o)
{
  switch (o->gch.gct) {
  case ~LJ_TSTR: return sizestring(gco2str(o));
  case ~LJ_TUPVAL: return sizeof(GCupval);
  case ~LJ_TTHREAD:
    return sizeof(lua_State) + sizeof(TValue) * gco2th(o)->stacksize;
  case ~LJ_TPROTO: return gco2pt(o)->sizept;
  case ~LJ_TFUNC: {
    GCfunc *fn = gco2func(o);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
    }
#if LJ_HASJIT
  case ~LJ_TTRACE: {
    GCtrace *T = gco2trace(o);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
    }
#endif
#if LJ_HASFFI
  case ~LJ_TCDATA: return sizeof(GCcdata);
#endif
  case ~LJ_TTAB: {
    GCtab *t = gco2tab(o);
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
	   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
    }
  case ~LJ_TUDATA: return sizeudata(gco2ud(o));
  default: return 0;
  }
}

/* One object of the census. */
static void gc_gw_census_obj(global_State *g, GCobj *o, luaJIT_gw_heap *out,
			     luaJIT_gw_census_ud cbu, luaJIT_gw_census_tab cbt,
			     void *ctx)
{
  size_t sz = gc_gw_objsize(o);
  uint32_t k = (uint32_t)o->gch.gct - ~LJ_TSTR;
  if (isdead(g, o) && !(o->gch.marked & LJ_GC_FIXED)) {
    out->dead++;
    out->dead_bytes += sz;
    return;
  }
  if (k < LUAJIT_GW__MAX) {
    out->count[k]++;
    out->bytes[k] += sz;
  }
  if (o->gch.gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    if (cbt)
      cbt(ctx, (const void *)t, sz, t->asize,
	  t->hmask ? t->hmask + 1 : 0, (const void *)tabref(t->metatable));
  } else if (o->gch.gct == ~LJ_TUDATA) {
    GCudata *ud = gco2ud(o);
    if (cbu)
      cbu(ctx, uddata(ud), sz, (const void *)tabref(ud->metatable),
	  (const void *)tabref(ud->env), isfinalized(ud) ? 1 : 0);
  } else if (o->gch.gct == ~LJ_TTHREAD) {  /* Its open upvalues. */
    GCobj *u;
    for (u = gcref(gco2th(o)->openupval); u; u = gcref(u->gch.nextgc)) {
      out->count[LUAJIT_GW_UPVAL]++;
      out->bytes[LUAJIT_GW_UPVAL] += sizeof(GCupval);
    }
  }
}

LUA_API void luaJIT_gw_census(lua_State *L, luaJIT_gw_heap *out,
			      luaJIT_gw_census_ud ud, luaJIT_gw_census_tab tab,
			      void *ctx)
{
  global_State *g = G(L);
  GCudata *a = gc_gw_anchor(g);
  int seg = 0;  /* 1: in the finobj list, 0/2: in the plain one. */
  GCobj *o;
  MSize i;
  memset(out, 0, sizeof(*out));
  out->total = (size_t)g->gc.total;
  out->gcstate = g->gc.state;
  /* All objects but strings and open upvalues; userdata after the main thread. */
  for (o = gcref(g->gc.root); o; o = gcref(o->gch.nextgc)) {
    if (a && o == obj2gco(a)) {  /* The anchors of luaJIT_gw_finobj(). */
      seg = 1;
      continue;
    } else if (a && o == obj2gco(gwfin(a)->b)) {
      seg = 2;
      continue;
    }
    if (o->gch.gct == ~LJ_TUDATA && !isdead(g, o)) {
      if (seg == 1) out->ud_finobj++; else out->ud_plain++;
      if ((gwudflags(gco2ud(o)) & LJ_GW_UD_PENDING)) out->ud_pending++;
    }
    gc_gw_census_obj(g, o, out, ud, tab, ctx);
  }
  if (a) {
    out->finobj_mode = gwfin(a)->mode;
    out->fin_mts = gwfin(a)->nmt;
  }
  for (i = 0; i <= g->strmask; i++)  /* Strings: the hash chains. */
    for (o = gcref(g->strhash[i]); o; o = gcref(o->gch.nextgc))
      gc_gw_census_obj(g, o, out, ud, tab, ctx);
  if (gcref(g->gc.mmudata)) {  /* Waiting for __gc: unlinked from the root. */
    GCobj *root = gcref(g->gc.mmudata);
    o = root;
    do {
      o = gcnext(o);
      out->finalize++;
      out->finalize_bytes += gc_gw_objsize(o);
    } while (o != root);
  }
}

LUA_API size_t luaJIT_gw_objsize(lua_State *L, int idx)
{
  cTValue *o;
  if (idx > 0) {
    o = L->base + (idx - 1);
    if (o >= L->top) return 0;
  } else if (idx < 0 && idx > LUA_REGISTRYINDEX) {
    o = L->top + idx;
    if (o < L->base) return 0;
  } else {
    return 0;
  }
  return tvisgcv(o) ? gc_gw_objsize(gcV(o)) : 0;
}

/* -- GlobalWar: the finobj list switch (see lj_gc.h, luajit.h) ----------- */

/* Make anchors A and B right after the main thread. NULL: out of memory. */
static GCudata *gc_gw_newanchors(global_State *g)
{
  GCSize sa = sizeof(GCudata) + sizeof(GCgwfin), sb = sizeof(GCudata);
  GCudata *a = (GCudata *)g->allocf(g->allocd, NULL, 0, sa);
  GCudata *b;
  GCudata *u[2];
  int i;
  if (a == NULL) return NULL;
  b = (GCudata *)g->allocf(g->allocd, NULL, 0, sb);
  if (b == NULL) {
    g->allocf(g->allocd, a, sa, 0);
    return NULL;
  }
  g->gc.total += sa + sb;
  u[0] = a; u[1] = b;
  for (i = 0; i < 2; i++) {
    /* Fixed: the sweep keeps them, lj_gc_freeall() frees them. Never marked
    ** (unreachable from Lua): no metatable, no environment.
    */
    newwhite(g, u[i]);
    u[i]->marked |= LJ_GC_FIXED;
    u[i]->gct = ~LJ_TUDATA;
    u[i]->udtype = LJ_GW_UDTYPE_ANCHOR;
    gwudflags(u[i]) = 0;
    u[i]->len = i ? 0 : sizeof(GCgwfin);
    setgcrefnull(u[i]->metatable);
    setgcrefnull(u[i]->env);
  }
  memset(gwfin(a), 0, sizeof(GCgwfin));
  gwfin(a)->b = b;
  /* mainthread -> A -> B -> (the userdata so far: the plain list). The sweep
  ** may stand at &mainthread->nextgc: then A and B are next, both alive.
  */
  setgcrefr(b->nextgc, mainthread(g)->nextgc);
  setgcref(a->nextgc, obj2gco(b));
  setgcref(mainthread(g)->nextgc, obj2gco(a));
  return a;
}

LUA_API int luaJIT_gw_finobj(lua_State *L, int mode)
{
  global_State *g = G(L);
  GCudata *a = gc_gw_anchor(g);
  GCgwfin *f;
  if (mode < 0)  /* Query. */
    return a ? gwfin(a)->mode : 0;
  if (mode) {
    if (a == NULL && (a = gc_gw_newanchors(g)) == NULL)
      return -1;  /* Out of memory: stays off. */
    f = gwfin(a);
    if (!f->mode) {
      f->mode = 1;
      f->dirty = 1;  /* The plain list may hold udata with __gc. */
    }
    return 1;
  }
  if (a) {  /* Off: atomic walks all userdata again, as upstream. */
    f = gwfin(a);
    f->mode = 0;
    gc_gw_unwatchall(f);
    f->npend = 0;  /* PENDING udata stay: the sweep still moves them. */
    f->dirty = 1;
  }
  return 0;
}

LUA_API void luaJIT_gw_finobj_stats(lua_State *L, luaJIT_gw_finobj_info *out)
{
  global_State *g = G(L);
  GCudata *a = gc_gw_anchor(g);
  memset(out, 0, sizeof(*out));
  if (a) {
    GCgwfin *f = gwfin(a);
    out->anchors = 1;
    out->mode = f->mode;
    out->dirty = f->dirty;
    out->watched_now = f->nmt;
    out->pending_now = f->npend;
    out->moved_head = f->moved_head;
    out->pending = f->pending;
    out->moved_sweep = f->moved_sweep;
    out->moved_full = f->moved_full;
    out->full_walks = f->full_walks;
    out->full_mt = f->full_mt;
    out->watched = f->watched;
    out->kept = f->kept;
    out->oom = f->oom;
  }
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  if (LJ_UNLIKELY(gc_gw_stats != NULL) && gc_gw_g == g) {
    atomic_gw(g, L, gc_gw_stats);  /* GlobalWar diagnostics, off by default. */
    return;
  }
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lj_assertG(!iswhite(obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  {  /* Separate userdata to be finalized. */
    GCgwsep sep;  /* GlobalWar: the finobj list, see lj_gc.h. */
    memset(&sep, 0, sizeof(sep));
    udsize = gc_separate_atomic(g, &sep);
  }
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */
  gc_gw_prunemt(g);  /* GlobalWar: unwatch the dead metatables. */

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    gc_fullsweep(g, &g->strhash[g->gc.sweepstr++]);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->strmask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = g->gc.total;
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
#if LJ_HASFFI
	g->gc.nocdatafin = 1;
#endif
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
#if LJ_HASFFI
    if (!g->gc.nocdatafin) lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
#endif
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lj_assertG(0, "bad GC state");
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lj_assertG(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o),
	     "bad object states for forward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
		 "bad GC state");
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lj_assertG((osz == 0) == (p == NULL), "realloc API violation");
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lj_assertG((nsz == 0) == (p == NULL), "allocf API violation");
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  g->gc.total = (g->gc.total - osz) + nsz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}


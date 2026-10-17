/*
** Garbage collector.
** Copyright (C) 2005-2021 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

/* Garbage collector states. Order matters. */
enum {
  GCSpause, GCSpropagate, GCSatomic, GCSsweepstring, GCSsweep, GCSfinalize
};

/* Bitmasks for marked field of GCobj. */
#define LJ_GC_WHITE0	0x01
#define LJ_GC_WHITE1	0x02
#define LJ_GC_BLACK	0x04
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_WEAKKEY	0x08
#define LJ_GC_WEAKVAL	0x10
#define LJ_GC_CDATA_FIN	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_SFIXED	0x40

#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

/* Macros to test and set GCobj colors. */
#define iswhite(x)	((x)->gch.marked & LJ_GC_WHITES)
#define isblack(x)	((x)->gch.marked & LJ_GC_BLACK)
#define isgray(x)	(!((x)->gch.marked & (LJ_GC_BLACK|LJ_GC_WHITES)))
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#ifdef __e2k__
/* Workaround for speculative loads from invalid addresses */
#define tviswhite_volatile(x)  (tvisgcv(x) && iswhite((volatile GCobj *) gcV(x)))
#endif
#define otherwhite(g)	(g->gc.currentwhite ^ LJ_GC_WHITES)
#define isdead(g, v)	((v)->gch.marked & otherwhite(g) & LJ_GC_WHITES)

#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)curwhite(g))
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g))
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#define fixstring(s)	((s)->marked |= LJ_GC_FIXED)
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

/* -- GlobalWar: a separate list of userdata with __gc (a local patch) ----
**
** luaJIT_gw_finobj(L, 1) (luajit.h) splits the userdata list after the main
** thread in two by two anchors (fixed userdata, never visible to Lua):
**
**   mainthread -> A -> [finobj: udata with __gc] -> B -> [plain udata] -> NULL
**
** A stays right after the main thread, so it is found in O(1); its payload
** is GCgwfin. New userdata go to the head of the plain list (after B). A
** userdata that gets a metatable with __gc moves to the finobj list: in O(1)
** when it is the head of the plain list (lua_newuserdata + lua_setmetatable),
** otherwise it is flagged PENDING and the next sweep moves it. The atomic
** phase walks only the finobj list, not all userdata. It is still one chain:
** the sweep, lj_gc_freeall() and lj_gc_separateudata(g, 1) see everything.
** A metatable without __gc of a plain userdata is watched (GCgwfin.mt): if
** __gc is added to it later, the next atomic phase walks the plain list too.
*/
#define LJ_GW_UDTYPE_ANCHOR	UDTYPE__MAX	/* udtype of the anchors. */
#define LJ_GW_UD_FINOBJ		0x01	/* GCudata.unused2: in the finobj list. */
#define LJ_GW_UD_PENDING	0x02	/* Has __gc, waits in the plain list. */
#define LJ_GC_GW_MTWATCH	0x80	/* GCtab.marked: in GCgwfin.mt. */
#define gwudflags(u)		((u)->unused2)

typedef struct GCgwfin {
  GCudata *b;		/* Anchor B: the end of the finobj list. */
  uint8_t mode;		/* 0: off (atomic walks all udata), 1: on. */
  uint8_t dirty;	/* 1: the plain list may hold udata with __gc. */
  uint8_t unused[2];
  MSize nmt, szmt;	/* Watched metatables (without __gc). */
  GCtab **mt;
  MSize npend, szpend;	/* Udata flagged PENDING since the last atomic. */
  GCudata **pend;
  /* Counters since the anchors were made (luaJIT_gw_finobj_stats). */
  uint64_t moved_head;	/* Moved by lua_setmetatable(): the plain head. */
  uint64_t pending;	/* Flagged PENDING (not the head). */
  uint64_t moved_sweep;	/* Moved by the sweep (PENDING ones). */
  uint64_t moved_full;	/* Moved by a full walk of the plain list. */
  uint64_t full_walks;	/* Atomic phases that walked the plain list too. */
  uint64_t full_mt;	/* Of them: a watched metatable got __gc. */
  uint64_t watched;	/* Metatables put on watch. */
  uint64_t kept;	/* PENDING udata unreachable at atomic, kept a cycle. */
  uint64_t oom;		/* Out of memory for the vectors: walk all instead. */
} GCgwfin;

#define gwfin(a)	((GCgwfin *)uddata(a))
/* Anchor A of the state or NULL. */
#define gc_gw_anchor(g) \
  (gcref(mainthread(g)->nextgc) && \
   gcref(mainthread(g)->nextgc)->gch.gct == ~LJ_TUDATA && \
   gco2ud(gcref(mainthread(g)->nextgc))->udtype == LJ_GW_UDTYPE_ANCHOR ? \
   gco2ud(gcref(mainthread(g)->nextgc)) : (GCudata *)NULL)

LJ_FUNC void lj_gc_gw_udmeta(lua_State *L, GCudata *ud);
LJ_FUNC void lj_gc_gw_freeanchor(global_State *g, GCudata *ud);

/* Collector. */
LJ_FUNC size_t lj_gc_separateudata(global_State *g, int all);
LJ_FUNC void lj_gc_finalize_udata(lua_State *L);
#if LJ_HASFFI
LJ_FUNC void lj_gc_finalize_cdata(lua_State *L);
#else
#define lj_gc_finalize_cdata(L)		UNUSED(L)
#endif
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);

/* GC check: drive collector forward if the GC threshold has been reached. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNCA void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_barriertrace(global_State *g, uint32_t traceno);
#endif

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  lj_assertG(isblack(o) && !isdead(g, o),
	     "bad object states for backward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  black2gray(o);
  setgcrefr(t->gclist, g->gc.grayagain);
  setgcref(g->gc.grayagain, o);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *p,
			  MSize *szp, MSize lim, MSize esz);

#define lj_mem_new(L, s)	lj_mem_realloc(L, NULL, 0, (s))

static LJ_AINLINE void lj_mem_free(global_State *g, void *p, size_t osize)
{
  g->gc.total -= (GCSize)osize;
  g->allocf(g->allocd, p, osize, 0);
}

#define lj_mem_newvec(L, n, t)	((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) \
  ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)	lj_mem_free(g, (p), (n)*sizeof(t))

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgco(L, sizeof(t)))
#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))

#endif

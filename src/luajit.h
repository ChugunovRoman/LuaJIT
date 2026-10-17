/*
** LuaJIT -- a Just-In-Time Compiler for Lua. https://luajit.org/
**
** Copyright (C) 2005-2021 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: https://www.opensource.org/licenses/mit-license.php ]
*/

#ifndef _LUAJIT_H
#define _LUAJIT_H

#include "lua.h"

#define LUAJIT_VERSION		"LuaJIT 2.1.0-beta3"
#define LUAJIT_VERSION_NUM	20100  /* Version 2.1.0 = 02.01.00. */
#define LUAJIT_VERSION_SYM	luaJIT_version_2_1_0_beta3
#define LUAJIT_COPYRIGHT	"Copyright (C) 2005-2021 Mike Pall"
#define LUAJIT_URL		"https://luajit.org/"

/* Modes for luaJIT_setmode. */
#define LUAJIT_MODE_MASK	0x00ff

enum {
  LUAJIT_MODE_ENGINE,		/* Set mode for whole JIT engine. */
  LUAJIT_MODE_DEBUG,		/* Set debug mode (idx = level). */

  LUAJIT_MODE_FUNC,		/* Change mode for a function. */
  LUAJIT_MODE_ALLFUNC,		/* Recurse into subroutine protos. */
  LUAJIT_MODE_ALLSUBFUNC,	/* Change only the subroutines. */

  LUAJIT_MODE_TRACE,		/* Flush a compiled trace. */

  LUAJIT_MODE_WRAPCFUNC = 0x10,	/* Set wrapper mode for C function calls. */

  LUAJIT_MODE_MAX
};

/* Flags or'ed in to the mode. */
#define LUAJIT_MODE_OFF		0x0000	/* Turn feature off. */
#define LUAJIT_MODE_ON		0x0100	/* Turn feature on. */
#define LUAJIT_MODE_FLUSH	0x0200	/* Flush JIT-compiled code. */

/* LuaJIT public C API. */

/* Control the JIT engine. */
LUA_API int luaJIT_setmode(lua_State *L, int idx, int mode);

/* Low-overhead profiling API. */
typedef void (*luaJIT_profile_callback)(void *data, lua_State *L,
					int samples, int vmstate);
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data);
LUA_API void luaJIT_profile_stop(lua_State *L);
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len);

/* Enforce (dynamic) linker error for version mismatches. Call from main. */
LUA_API void LUAJIT_VERSION_SYM(void);

/* -- GlobalWar: GC diagnostics (a local patch, not in upstream LuaJIT) ---- */

/* luaJIT_gw_gcstats() times every atomic phase of the GC (lj_gc.c atomic())
** part by part and counts what each part walked. Off by default: until it is
** given a struct, atomic() pays one load and a branch per GC cycle.
** luaJIT_gw_census() walks all GC objects once (a diagnostic on request),
** luaJIT_gw_objsize() returns the size of one object on the stack.
** Only from the thread that runs Lua now. The engine checks LUAJIT_GW_GCSTATS:
** without this patch the diagnostics are compiled out.
*/
#define LUAJIT_GW_GCSTATS	1

/* A monotonic clock of the caller (QueryPerformanceCounter etc.). */
typedef unsigned long long (*luaJIT_gw_clock)(void);

/* One part of the atomic phase. */
typedef struct luaJIT_gw_gcpart {
  unsigned long long t;		/* Time, in ticks of the clock. */
  size_t objs;			/* Objects propagated (turned black). */
  size_t bytes;			/* Their size, as the GC estimates it. */
  size_t tabs, tabslots;	/* Tables among them, array + hash slots. */
  size_t threads, stackslots;	/* Threads among them, stack slots. */
} luaJIT_gw_gcpart;

/* The last atomic phase. */
typedef struct luaJIT_gw_atomic {
  unsigned int seq;		/* Atomic phases recorded so far. */
  unsigned long long t_total;	/* The whole atomic(). */
  luaJIT_gw_gcpart uv;		/* Remark of open upvalues (objs = upvalues). */
  luaJIT_gw_gcpart left;	/* Left-overs of the gray list. */
  luaJIT_gw_gcpart remark;	/* Weak tables again, running thread, roots. */
  size_t remark_weak;		/* Weak tables traversed again there. */
  luaJIT_gw_gcpart grayagain;	/* The 2nd chance list and all it reaches. */
  size_t ga_tabs, ga_tabslots;	/* The list: tables written behind a barrier. */
  size_t ga_threads, ga_stackslots;  /* The list: threads (always there). */
  luaJIT_gw_gcpart separate;	/* lj_gc_separateudata(): objs = udata walked. */
  size_t ud_white;		/* Udata found white (unreachable). */
  size_t ud_gc;			/* Of them moved to finalization (__gc). */
  luaJIT_gw_gcpart mmudata;	/* Mark the udata to finalize, propagate. */
  size_t mm_count;		/* Udata/cdata waiting for finalization. */
  luaJIT_gw_gcpart clearweak;	/* objs = weak tables, tabslots = their slots. */
  size_t weak_cleared;		/* Slots cleared. */
  unsigned long long t_rest;	/* lj_buf_shrink() and the sweep setup. */
  size_t strnum, strslots;	/* Interned strings, string hash slots. */
  size_t mainstack, curstack;	/* Stack slots of the main/running thread. */
  size_t total, estimate;	/* Heap bytes before, estimate after. */
} luaJIT_gw_atomic;

/* st != NULL: record every atomic phase of the state of L into *st (time by
** clock). st == NULL: off. One state at a time.
*/
LUA_API void luaJIT_gw_gcstats(lua_State *L, luaJIT_gw_atomic *st,
			       luaJIT_gw_clock clock);

/* Object kinds of the census (ORDER LJ_T: LJ_TSTR .. LJ_TUDATA). */
enum {
  LUAJIT_GW_STR, LUAJIT_GW_UPVAL, LUAJIT_GW_THREAD, LUAJIT_GW_PROTO,
  LUAJIT_GW_FUNC, LUAJIT_GW_TRACE, LUAJIT_GW_CDATA, LUAJIT_GW_TAB,
  LUAJIT_GW_UDATA, LUAJIT_GW__MAX
};

typedef struct luaJIT_gw_heap {
  size_t count[LUAJIT_GW__MAX];	/* Live objects by kind (cdata: header only). */
  size_t bytes[LUAJIT_GW__MAX];
  size_t dead, dead_bytes;	/* Skipped: found dead, not swept yet. */
  size_t finalize, finalize_bytes;  /* Waiting for __gc. */
  size_t total;			/* Heap bytes (lua_gc(L, LUA_GCCOUNT)). */
  int gcstate;			/* 0 pause, 1 propagate, 2 atomic, 3-4 sweep, 5 finalize. */
} luaJIT_gw_heap;

/* Every live userdata: p = its payload (as lua_topointer()), bytes = its
** size in bytes (header included), metatable/environment table (as
** lua_topointer(), NULL = none), finalized = its __gc has run (or it had
** none when it was found unreachable).
*/
typedef void (*luaJIT_gw_census_ud)(void *ctx, const void *p, size_t bytes,
				    const void *mt, const void *env,
				    int finalized);
/* Every live table (as lua_topointer()), its size, array and hash slots. */
typedef void (*luaJIT_gw_census_tab)(void *ctx, const void *t, size_t bytes,
				     unsigned int asize, unsigned int hslots,
				     const void *mt);

/* Walk all GC objects of the state: totals into *out, the callbacks (may be
** NULL) for userdata and tables. The callbacks must not touch the Lua state.
*/
LUA_API void luaJIT_gw_census(lua_State *L, luaJIT_gw_heap *out,
			      luaJIT_gw_census_ud ud, luaJIT_gw_census_tab tab,
			      void *ctx);

/* Size of the GC object at the stack index idx (no pseudo-indices), 0 for a
** non-GC value: tables with their array and hash, strings, userdata, threads
** with their stack, functions (without upvalues and prototypes).
*/
LUA_API size_t luaJIT_gw_objsize(lua_State *L, int idx);

/* luaJIT_gw_udata_hook(): a callback for every new userdata (lj_udata_new:
** lua_newuserdata, so newproxy and io files too, and ffi.load). It is called
** right after the userdata is allocated: it is not on the stack yet and has no
** metatable. p = its payload (as lua_touserdata() will return it), size = the
** payload size. The callback may read the debug info of L (lua_getstack,
** lua_getinfo without "f" and ">", luaJIT_gw_cfunc), must not push anything,
** allocate in the Lua heap, call Lua or raise an error. One callback for the
** process (all states). NULL (the default) = off: one load and a branch per
** userdata. The engine checks LUAJIT_GW_UDATA_HOOK.
*/
#define LUAJIT_GW_UDATA_HOOK	1

typedef void (*luaJIT_gw_udata_cb)(lua_State *L, void *p, size_t size);

LUA_API void luaJIT_gw_udata_hook(luaJIT_gw_udata_cb cb);

/* The function of the frame ar (filled by lua_getstack()): a C function or a
** builtin -> its C entry, a Lua function -> NULL. Pushes nothing.
*/
LUA_API lua_CFunction luaJIT_gw_cfunc(lua_State *L, const lua_Debug *ar);

#endif

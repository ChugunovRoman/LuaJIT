/*
** Userdata handling.
** Copyright (C) 2005-2021 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_udata.h"
#include "lj_frame.h"
#include "luajit.h"

/* GlobalWar diagnostics (a local patch, see luajit.h): NULL = off. */
static luaJIT_gw_udata_cb udata_gw_hook;

LUA_API void luaJIT_gw_udata_hook(luaJIT_gw_udata_cb cb)
{
  udata_gw_hook = cb;
}

LUA_API lua_CFunction luaJIT_gw_cfunc(lua_State *L, const lua_Debug *ar)
{
  uint32_t offset = (uint32_t)ar->i_ci & 0xffff;  /* As lj_debug_getinfo(). */
  GCfunc *fn;
  if (offset == 0) return NULL;
  fn = frame_func(tvref(L->stack) + offset);
  return isluafunc(fn) ? NULL : fn->c.f;
}

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  GCudata *ud = lj_mem_newt(L, sizeof(GCudata) + sz, GCudata);
  global_State *g = G(L);
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  gwudflags(ud) = 0;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  setgcref(ud->env, obj2gco(env));
  {
    /* Chain to userdata list (after main thread). GlobalWar: with the anchors
    ** of luaJIT_gw_finobj() to the head of the plain list (lj_gc.h).
    */
    GCudata *a = gc_gw_anchor(g);
    GCRef *head = a ? &gwfin(a)->b->nextgc : &mainthread(g)->nextgc;
    setgcrefr(ud->nextgc, *head);
    setgcref(*head, obj2gco(ud));
  }
  if (LJ_UNLIKELY(udata_gw_hook != NULL))
    udata_gw_hook(L, uddata(ud), (size_t)sz);  /* GlobalWar, off by default. */
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
  if (LJ_UNLIKELY(ud->udtype == LJ_GW_UDTYPE_ANCHOR))
    lj_gc_gw_freeanchor(g, ud);  /* GlobalWar: lj_gc_freeall() only. */
  lj_mem_free(g, ud, sizeudata(ud));
}


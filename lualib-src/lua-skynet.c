#define LUA_LIB

#include "skynet.h"
#include "lua-seri.h"

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <time.h>

#if defined(__APPLE__)
#include <sys/time.h>
#endif

#include "skynet.h"

// return nsec
static int64_t
get_time() {
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	return (int64_t)1000000000 * ti.tv_sec + ti.tv_nsec;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)1000000000 * tv.tv_sec + tv.tv_usec * 1000;
#endif
}

static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

struct callback_context {
	lua_State *L;
};

static int
_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct callback_context *cb_ctx = (struct callback_context *)ud;
	lua_State *L = cb_ctx->L;
	int trace = 1;
	int r;
	lua_pushvalue(L,2);

	lua_pushinteger(L, type);
	lua_pushlightuserdata(L, (void *)msg);
	lua_pushinteger(L,sz);
	lua_pushinteger(L, session);
	lua_pushinteger(L, source);

	r = lua_pcall(L, 5, 0 , trace);

	if (r == LUA_OK) {
		return 0;
	}
	const char * self = skynet_command(context, "REG", NULL);
	switch (r) {
	case LUA_ERRRUN:
		skynet_error(context, "lua call [%x to %s : %d msgsz = %d, type = %d] error : " KRED "%s" KNRM, source , self, session, sz, type, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		skynet_error(context, "lua memory error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRERR:
		skynet_error(context, "lua error in error : [%x to %s : %d]", source , self, session);
		break;
	};

	lua_pop(L,1);

	return 0;
}

static int
forward_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	_cb(context, ud, type, session, source, msg, sz);
	// don't delete msg in forward mode.
	return 1;
}

static void
clear_last_context(lua_State *L) {
	if (lua_getfield(L, LUA_REGISTRYINDEX, "callback_context") == LUA_TUSERDATA) {
		lua_pushnil(L);
		lua_setiuservalue(L, -2, 2);
	}
	lua_pop(L, 1);
}

static int
_cb_pre(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct callback_context *cb_ctx = (struct callback_context *)ud;
	clear_last_context(cb_ctx->L);
	skynet_callback(context, ud, _cb);
	return _cb(context, cb_ctx, type, session, source, msg, sz);
}

static int
_forward_pre(struct skynet_context *context, void *ud, int type, int session, uint32_t source, const void *msg, size_t sz) {
	struct callback_context *cb_ctx = (struct callback_context *)ud;
	clear_last_context(cb_ctx->L);
	skynet_callback(context, ud, forward_cb);
	return forward_cb(context, cb_ctx, type, session, source, msg, sz);
}

static int
lcallback(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int forward = lua_toboolean(L, 2);
	luaL_checktype(L,1,LUA_TFUNCTION);
	lua_settop(L,1);
	struct callback_context * cb_ctx = (struct callback_context *)lua_newuserdatauv(L, sizeof(*cb_ctx), 2);
	cb_ctx->L = lua_newthread(L);
	lua_pushcfunction(cb_ctx->L, traceback);
	lua_setiuservalue(L, -2, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "callback_context");
	lua_setiuservalue(L, -2, 2);
	lua_setfield(L, LUA_REGISTRYINDEX, "callback_context");
	lua_xmove(L, cb_ctx->L, 1);

	skynet_callback(context, cb_ctx, (forward)?(_forward_pre):(_cb_pre));
	return 0;
}

static int
lcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {
		parm = luaL_checkstring(L,2);
	}

	result = skynet_command(context, cmd, parm);
	if (result) {
		lua_pushstring(L, result);
		return 1;
	}
	return 0;
}

static int
laddresscommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {
		parm = luaL_checkstring(L,2);
	}
	result = skynet_command(context, cmd, parm);
	if (result && result[0] == ':') {
		int i;
		uint32_t addr = 0;
		for (i=1;result[i];i++) {
			int c = result[i];
			if (c>='0' && c<='9') {
				c = c - '0';
			} else if (c>='a' && c<='f') {
				c = c - 'a' + 10;
			} else if (c>='A' && c<='F') {
				c = c - 'A' + 10;
			} else {
				return 0;
			}
			addr = addr * 16 + c;
		}
		lua_pushinteger(L, addr);
		return 1;
	}
	return 0;
}

static int
lintcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	const char * parm = NULL;
	char tmp[64];	// for integer parm
	if (lua_gettop(L) == 2) {
		if (lua_isnumber(L, 2)) {
			int32_t n = (int32_t)luaL_checkinteger(L,2);
			sprintf(tmp, "%d", n);
			parm = tmp;
		} else {
			parm = luaL_checkstring(L,2);
		}
	}

	result = skynet_command(context, cmd, parm);
	if (result) {
		char *endptr = NULL;
		lua_Integer r = strtoll(result, &endptr, 0);
		if (endptr == NULL || *endptr != '\0') {
			// may be real number
			double n = strtod(result, &endptr);
			if (endptr == NULL || *endptr != '\0') {
				return luaL_error(L, "Invalid result %s", result);
			} else {
				lua_pushnumber(L, n);
			}
		} else {
			lua_pushinteger(L, r);
		}
		return 1;
	}
	return 0;
}

static int
lrawintcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * parm = NULL;
	char tmp[64];	// for integer parm
	if (lua_gettop(L) == 2) {
		if (lua_isnumber(L, 2)) {
			int32_t n = (int32_t)luaL_checkinteger(L,2);
			sprintf(tmp, "%d", n);
			parm = tmp;
		} else {
			parm = luaL_checkstring(L,2);
		}
	}

	int64_t result = (int64_t)skynet_command(context, cmd, parm);
	lua_pushinteger(L, result);
	return 1;
}

static int
lgenid(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int session = skynet_send(context, 0, 0, PTYPE_TAG_ALLOCSESSION , 0 , NULL, 0);
	lua_pushinteger(L, session);
	return 1;
}

static const char *
get_dest_string(lua_State *L, int index) {
	const char * dest_string = lua_tostring(L, index);
	if (dest_string == NULL) {
		luaL_error(L, "dest address type (%s) must be a string or number.", lua_typename(L, lua_type(L,index)));
	}
	return dest_string;
}

static int
send_message(lua_State *L, int source, int idx_type) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t dest = (uint32_t)lua_tointeger(L, 1);
	const char * dest_string = NULL;
	if (dest == 0) {
		if (lua_type(L,1) == LUA_TNUMBER) {
			return luaL_error(L, "Invalid service address 0");
		}
		dest_string = get_dest_string(L, 1);
	}

	int type = luaL_checkinteger(L, idx_type+0);
	int session = 0;
	if (lua_isnil(L,idx_type+1)) {
		type |= PTYPE_TAG_ALLOCSESSION;
	} else {
		session = luaL_checkinteger(L,idx_type+1);
	}

	int mtype = lua_type(L,idx_type+2);
	switch (mtype) {
	case LUA_TSTRING: {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,idx_type+2,&len);
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			session = skynet_sendname(context, source, dest_string, type, session , msg, len);
		} else {
			session = skynet_send(context, source, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,idx_type+2);
		int size = luaL_checkinteger(L,idx_type+3);
		if (dest_string) {
			session = skynet_sendname(context, source, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
			session = skynet_send(context, source, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "invalid param %s", lua_typename(L, lua_type(L,idx_type+2)));
	}
	if (session < 0) {
		if (session == -2) {
			// package is too large
			lua_pushboolean(L, 0);
			return 1;
		}
		// send to invalid address
		// todo: maybe throw an error would be better
		return 0;
	}
	lua_pushinteger(L,session);
	return 1;
}

/*
	uint32 address
	 string address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len
 */
static int
lsend(lua_State *L) {
	return send_message(L, 0, 2);
}

/*
	uint32 address
	 string address
	integer source_address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len
 */
static int
lredirect(lua_State *L) {
	uint32_t source = (uint32_t)luaL_checkinteger(L,2);
	return send_message(L, source, 3);
}

static int
lerror(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int n = lua_gettop(L);
	if (n <= 1) {
		lua_settop(L, 1);
		size_t len;
		const char *s = luaL_tolstring(L, 1, &len);
		skynet_error(context, "%*s", (int)len, s);
		return 0;
	}
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	int i;
	for (i=1; i<=n; i++) {
		luaL_tolstring(L, i, NULL);
		luaL_addvalue(&b);
		if (i<n) {
			luaL_addchar(&b, ' ');
		}
	}
	luaL_pushresult(&b);
	size_t len;
	const char *s = luaL_tolstring(L, -1, &len);
	skynet_error(context, "%*s", (int)len, s);
	return 0;
}

static int
ltostring(lua_State *L) {
	if (lua_isnoneornil(L,1)) {
		return 0;
	}
	char * msg = lua_touserdata(L,1);
	int sz = luaL_checkinteger(L,2);
	lua_pushlstring(L,msg,sz);
	return 1;
}

static int
lharbor(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t handle = (uint32_t)luaL_checkinteger(L,1);
	int harbor = 0;
	int remote = skynet_isremote(context, handle, &harbor);
	lua_pushinteger(L,harbor);
	lua_pushboolean(L, remote);

	return 2;
}

static int
lpackstring(lua_State *L) {
	luaseri_pack(L);
	char * str = (char *)lua_touserdata(L, -2);
	int sz = lua_tointeger(L, -1);
	lua_pushlstring(L, str, sz);
	skynet_free(str);
	return 1;
}

static int
ltrash(lua_State *L) {
	int t = lua_type(L,1);
	switch (t) {
	case LUA_TSTRING: {
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,1);
		luaL_checkinteger(L,2);
		skynet_free(msg);
		break;
	}
	default:
		luaL_error(L, "skynet.trash invalid param %s", lua_typename(L,t));
	}

	return 0;
}

static int
lnow(lua_State *L) {
	uint64_t ti = skynet_now();
	lua_pushinteger(L, ti);
	return 1;
}

static int
lhpc(lua_State *L) {
	lua_pushinteger(L, get_time());
	return 1;
}

#define MAX_LEVEL 3

struct source_info {
	const char * source;
	int line;
};

/*
	string tag
	string userstring
	thread co (default nil/current L)
	integer level (default nil)
 */
static int
ltrace(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * tag = luaL_checkstring(L, 1);
	const char * user = luaL_checkstring(L, 2);
	if (!lua_isnoneornil(L, 3)) {
		lua_State * co = L;
		int level;
		if (lua_isthread(L, 3)) {
			co = lua_tothread (L, 3);
			level = luaL_optinteger(L, 4, 1);
		} else {
			level = luaL_optinteger(L, 3, 1);
		}
		struct source_info si[MAX_LEVEL];
		lua_Debug d;
		int index = 0;
		do {
			if (!lua_getstack(co, level, &d))
				break;
			lua_getinfo(co, "Sl", &d);
			level++;
			si[index].source = d.source;
			si[index].line = d.currentline;
			if (d.currentline >= 0)
				++index;
		} while (index < MAX_LEVEL);
		switch (index) {
		case 1:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s : %s:%d", tag, get_time(), user, si[0].source, si[0].line);
			break;
		case 2:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s : %s:%d %s:%d", tag, get_time(), user,
				si[0].source, si[0].line,
				si[1].source, si[1].line
				);
			break;
		case 3:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s : %s:%d %s:%d %s:%d", tag, get_time(), user,
				si[0].source, si[0].line,
				si[1].source, si[1].line,
				si[2].source, si[2].line
				);
			break;
		default:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s", tag, get_time(), user);
			break;
		}
		return 0;
	}
	skynet_error(context, "<TRACE %s> %" PRId64 " %s", tag, get_time(), user);
	return 0;
}

LUAMOD_API int
luaopen_skynet_core(lua_State *L) {
	luaL_checkversion(L);

	luaL_Reg l[] = {
		{ "send" , lsend },
		{ "genid", lgenid },
		{ "redirect", lredirect },
		{ "command" , lcommand },
		{ "intcommand", lintcommand },
		{ "rawintcommand", lrawintcommand },
		{ "addresscommand", laddresscommand },
		{ "error", lerror },
		{ "harbor", lharbor },
		{ "callback", lcallback },
		{ "trace", ltrace },
		{ NULL, NULL },
	};

	// functions without skynet_context
	luaL_Reg l2[] = {
		{ "tostring", ltostring },
		{ "pack", luaseri_pack },
		{ "unpack", luaseri_unpack },
		{ "packstring", lpackstring },
		{ "trash" , ltrash },
		{ "now", lnow },
		{ "hpc", lhpc },	// getHPCounter
		{ NULL, NULL },
	};

	lua_createtable(L, 0, sizeof(l)/sizeof(l[0]) + sizeof(l2)/sizeof(l2[0]) -2);

	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");
	struct skynet_context *ctx = lua_touserdata(L,-1);
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}


	luaL_setfuncs(L,l,1);

	luaL_setfuncs(L,l2,0);

	return 1;
}

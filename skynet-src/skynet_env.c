#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

#include <immintrin.h>
#include <time.h>

struct skynet_env {
	struct spinlock lock;
	lua_State *L;
    int64_t snowflake_starttime;
	int64_t snowflake_lasttime;
	int32_t snowflake_sequence;
};

static struct skynet_env *E = NULL;

const char * 
skynet_getenv(const char *key) {
	SPIN_LOCK(E)

	lua_State *L = E->L;
	
	lua_getglobal(L, key);
	const char * result = lua_tostring(L, -1);
	lua_pop(L, 1);

	SPIN_UNLOCK(E)

	return result;
}

void 
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)
	
	lua_State *L = E->L;
	lua_getglobal(L, key);
	assert(lua_isnil(L, -1));
	lua_pop(L,1);
	lua_pushstring(L,value);
	lua_setglobal(L,key);

	SPIN_UNLOCK(E)
}

void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	SPIN_INIT(E)
	E->L = luaL_newstate();
}

// snowflake
static uint64_t
current_timestamp() {
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	return ti.tv_sec * 1000 + ti.tv_nsec / 1000000;
}

static uint64_t
next_timestamp(uint64_t lastTimestamp) {
    uint64_t timestamp = current_timestamp();
    while (timestamp == lastTimestamp) {
        _mm_pause();
        timestamp = current_timestamp();
    }
    return timestamp;
}

/*
1bit |  38bit  | 15bit   | 10 bit
0    | 时间戳  | 节点号  | 序列表
时间戳 从 2024-1-1 00:00:00 开始，支持8年
节点号 0-32767
序列表 0-1023
*/
int64_t
skynet_snowflake(int machine) {
    SPIN_LOCK(E)
	int64_t uuid = 0;
	int64_t curtime = current_timestamp();
	if (curtime < E->snowflake_lasttime) {
        printf("Clock moved backwards. Refusing to generate id.\n");
        uuid = -1;
    } else {
		if (curtime == E->snowflake_lasttime) {
			// same millisecond
			E->snowflake_sequence = (E->snowflake_sequence + 1) & 0x3ff;
			if (E->snowflake_sequence == 0){
				E->snowflake_lasttime = next_timestamp(E->snowflake_lasttime);
			}
		} else{
			E->snowflake_sequence = 0;
			E->snowflake_lasttime = curtime;
		}

		int64_t deltams = curtime - E->snowflake_starttime;
		if (deltams >= 0x4000000000){
			printf("Out of range time difference. Refusing to generate id.\n");
			uuid = -1;
		}else{
			uuid = deltams << 25 | machine << 10 | E->snowflake_sequence;
		}
	}
    SPIN_UNLOCK(E)
    return uuid;
}

void
skynet_snowflake_init(){
	lua_State *L = E->L;
	lua_getglobal(L, "snowflake_starttime");
	int isnum = 0;
	int64_t result = lua_tointegerx(L, -1, &isnum);
	lua_pop(L, 1);
	if (isnum == 0){
		// 2024-1-1 00:00:00
		result = 1704038400;
	}
	E->snowflake_starttime = result * 1000;
	E->snowflake_sequence = 0;
	E->snowflake_lasttime = 0;
}


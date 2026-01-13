#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

#include <stdint.h>

const char * skynet_getenv(const char *key);
void skynet_setenv(const char *key, const char *value);

void skynet_env_init();

int64_t skynet_snowflake(int machine);
void skynet_snowflake_init();

#endif

#ifndef _HOST_H_
#define _HOST_H_

#include <stdio.h>
#include <stdint.h>

typedef struct {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t dayofweek;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint16_t milliseconds;
} HOST_TIME_t;

typedef void (*HOST_THREAD_FUNC_t)(void*);

#define HOST_THREAD_PRIORITY_LOWEST			0
#define HOST_THREAD_PRIORITY_LOWER			1
#define HOST_THREAD_PRIORITY_LOW			2
#define HOST_THREAD_PRIORITY_NORMAL			3
#define HOST_THREAD_PRIORITY_HIGH			4
#define HOST_THREAD_PRIORITY_HIGHER			5
#define HOST_THREAD_PRIORITY_HIGHEST		6

void host_get_local_time(HOST_TIME_t* timedata);
int host_get_terminal_columns(FILE* stream);
int host_fseek64(FILE* file, uint64_t offset, int origin);
uint64_t host_ftell64(FILE* file);
int host_create_thread(HOST_THREAD_FUNC_t thread_func, void* param);
void host_end_thread(void);
void host_set_thread_priority(int priority);
int host_do_startup_tasks(void);
void host_sleep(uint32_t ms);

#endif

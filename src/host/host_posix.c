#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include "host.h"

void host_get_local_time(HOST_TIME_t* timedata) {
	struct timeval tv;
	time_t now;
	struct tm tm_now;
	struct tm* tm_local;

	memset(timedata, 0, sizeof(*timedata));
	if (gettimeofday(&tv, NULL) != 0) {
		now = time(NULL);
		timedata->milliseconds = 0;
	}
	else {
		now = tv.tv_sec;
		timedata->milliseconds = (uint16_t)(tv.tv_usec / 1000);
	}

	tm_local = localtime(&now);
	if (tm_local == NULL) {
		return;
	}
	tm_now = *tm_local;

	timedata->year = (uint16_t)(tm_now.tm_year + 1900);
	timedata->month = (uint8_t)(tm_now.tm_mon + 1);
	timedata->day = (uint8_t)tm_now.tm_mday;
	timedata->dayofweek = (uint8_t)tm_now.tm_wday;
	timedata->hour = (uint8_t)tm_now.tm_hour;
	timedata->minute = (uint8_t)tm_now.tm_min;
	timedata->second = (uint8_t)tm_now.tm_sec;
}

int host_get_terminal_columns(FILE* stream) {
	int fd;
	struct winsize ws;

	if (stream == NULL) {
		return -1;
	}

	fd = fileno(stream);
	if ((fd < 0) || !isatty(fd)) {
		return -1;
	}

	memset(&ws, 0, sizeof(ws));
	if ((ioctl(fd, TIOCGWINSZ, &ws) != 0) || (ws.ws_col == 0)) {
		return -1;
	}

	return (int)ws.ws_col;
}

int host_fseek64(FILE* file, uint64_t offset, int origin) {
	return fseeko(file, (off_t)offset, origin);
}

uint64_t host_ftell64(FILE* file) {
	off_t offset = ftello(file);

	if (offset < 0) {
		return UINT64_MAX;
	}

	return (uint64_t)offset;
}

int host_create_thread(HOST_THREAD_FUNC_t thread_func, void* param) {
	pthread_t thread;
	return pthread_create(&thread, NULL, (void*(*)(void*))thread_func, param);
}

void host_end_thread(void) {
	pthread_exit(NULL);
}

void host_set_thread_priority(int priority) {
	switch (priority) {
	case HOST_THREAD_PRIORITY_LOWEST:
		nice(15);
		break;
	case HOST_THREAD_PRIORITY_LOWER:
		nice(10);
		break;
	case HOST_THREAD_PRIORITY_LOW:
		nice(5);
		break;
	case HOST_THREAD_PRIORITY_NORMAL:
		nice(0);
		break;
	case HOST_THREAD_PRIORITY_HIGH:
		nice(-5);
		break;
	case HOST_THREAD_PRIORITY_HIGHER:
		nice(-10);
		break;
	case HOST_THREAD_PRIORITY_HIGHEST:
		nice(-15);
		break;
	}
}

int host_do_startup_tasks() {
	//Nothing to do here right now on POSIX hosts.
	//On Windows, we use this function to pin the main thread to P-cores.
	//Too lazy to figure that out on other platforms right now.
	return 0;
}

void host_sleep(uint32_t ms) {
	int res;
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000U);
	ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
	do {
		res = nanosleep(&ts, &ts);
	} while (res && errno == EINTR);
}

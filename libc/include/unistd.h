#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

int open(const char *pathname, int flags);
int close(int fd);
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int getpid(void);
void sleep(unsigned int seconds);

#endif

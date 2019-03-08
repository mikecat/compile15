#include <stdio.h>
#include <stdlib.h>
#include "util.h"

void* malloc_check(size_t size) {
	void* buffer = malloc(size);
	if (buffer == NULL) {
		perror("malloc");
		exit(1);
	}
	return buffer;
}

#ifndef UCM_SPDK_STARTUP_LOCK_H
#define UCM_SPDK_STARTUP_LOCK_H

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define STORE_SESSION_SIZE 128

/* The region lease file stores only the last successfully initialized session.
 * Caller holds the startup gate; publishing is deferred until init succeeds. */
static inline int
store_session_matches(int fd, const char *session)
{
	char saved[STORE_SESSION_SIZE] = {0};
	ssize_t n = pread(fd, saved, sizeof(saved), 0);

	if (n < 0) { return -errno; }
	return n == sizeof(saved) && saved[sizeof(saved) - 1] == '\0' &&
	       strcmp(saved, session) == 0;
}

static inline int
store_session_publish(int fd, const char *session)
{
	char saved[STORE_SESSION_SIZE] = {0};

	if (session != NULL) {
		if (session[0] == '\0' || strlen(session) >= sizeof(saved)) { return -EINVAL; }
		memcpy(saved, session, strlen(session));
	}
	ssize_t n = pwrite(fd, saved, sizeof(saved), 0);

	return n < 0 ? -errno : (n == sizeof(saved) ? 0 : -EIO);
}

/* Serialize startup before acquiring any lifetime maintenance/region leases. */
static inline int
store_startup_open(const char *name, int *fd)
{
	*fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (*fd < 0) {
		return -errno;
	}
	if (flock(*fd, LOCK_EX) != 0) {
		int rc = -errno;

		close(*fd);
		*fd = -1;
		return rc;
	}
	return 0;
}

static inline void
store_startup_close(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

#endif

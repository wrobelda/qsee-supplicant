// SPDX-License-Identifier: BSD-2-Clause
#include "qsee_supplicant.h"
#include "qsee_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fs_handle {
	int protocol_fd;
	int host_fd;
	struct fs_handle *next;
};

static struct fs_handle *handles;
static int next_handle = 3;
static int last_error;

static int reply(void *buffer, uint32_t command, int32_t result)
{
	struct qs_fs_response *response = buffer;

	response->command = command;
	response->result = result;
	return 0;
}

static int fail(void *buffer, uint32_t command)
{
	last_error = errno;
	return reply(buffer, command, -1);
}

static struct fs_handle *lookup(int id)
{
	struct fs_handle *h;
	for (h = handles; h; h = h->next) if (h->protocol_fd == id) return h;
	errno = EBADF; return NULL;
}

static int add_handle(int fd)
{
	struct fs_handle *h = calloc(1, sizeof(*h));
	if (!h) return -1;
	h->protocol_fd = next_handle++; h->host_fd = fd; h->next = handles; handles = h;
	return h->protocol_fd;
}

static int close_handle(int id)
{
	struct fs_handle **p = &handles, *h;
	while ((h = *p)) {
		if (h->protocol_fd == id) { *p = h->next; close(h->host_fd); free(h); return 0; }
		p = &h->next;
	}
	errno = EBADF; return -1;
}

void qs_fs_reset(void)
{
	while (handles) {
		struct fs_handle *next = handles->next;
		close(handles->host_fd);
		free(handles);
		handles = next;
	}
	next_handle = 3;
	last_error = 0;
}

static int normalize(const char *input, char *path, size_t size)
{
	return qs_normalize_path(input, QS_PROTOCOL_NAME_SIZE, path, size);
}

int qs_fs_dispatch(struct qs_store *s, void *buffer, size_t size)
{
	struct qs_fs_path_request *path_request = buffer;
	struct qs_fs_open_request *open_request = buffer;
	struct qs_fs_create_request *create_request = buffer;
	struct qs_fs_fd_request *fd_request = buffer;
	struct qs_fs_read_request *read_request = buffer;
	struct qs_fs_read_response *read_response = buffer;
	struct qs_fs_write_request *write_request = buffer;
	struct qs_fs_lseek_request *lseek_request = buffer;
	struct qs_fs_mkdir_request *mkdir_request = buffer;
	struct qs_fs_rename_request *rename_request = buffer;
	struct fs_handle *h;
	uint32_t command, count, flags;
	char path[512], path2[512], leaf[256], leaf2[256];
	int id, fd, rc, parent = -1, parent2 = -1;
	ssize_t n;

	if (!s || !buffer || size < QS_FS_BUFFER_SIZE)
		return -EINVAL;
	command = path_request->command;
	switch (command) {
	case QS_FS_OPEN:
		if (normalize(open_request->pathname, path, sizeof(path)))
			return fail(buffer, command);
		flags = (uint32_t)open_request->flags;
		if (qs_open_parent(s, path, (flags & O_CREAT) != 0,
				   &parent, leaf, sizeof(leaf)))
			return fail(buffer, command);
		fd = openat(parent, leaf, (int)flags | O_CLOEXEC | O_NOFOLLOW,
			    s->file_mode);
		close(parent);
		if (fd < 0)
			return fail(buffer, command);
		id = add_handle(fd);
		if (id < 0) {
			close(fd);
			return fail(buffer, command);
		}
		return reply(buffer, command, id);
	case QS_FS_CREAT:
		if (normalize(create_request->pathname, path, sizeof(path)) ||
		    qs_open_parent(s, path, true, &parent, leaf, sizeof(leaf)))
			return fail(buffer, command);
		fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
			    create_request->mode & s->file_mode);
		close(parent);
		if (fd < 0)
			return fail(buffer, command);
		id = add_handle(fd);
		if (id < 0) {
			close(fd);
			return fail(buffer, command);
		}
		return reply(buffer, command, id);
	case QS_FS_READ:
		h = lookup(read_request->fd);
		if (!h)
			return fail(buffer, command);
		count = read_request->count;
		if (count > sizeof(read_response->data))
			count = sizeof(read_response->data);
		n = read(h->host_fd, read_response->data, count);
		if (n < 0)
			last_error = errno;
		read_response->command = command;
		read_response->result = (int32_t)n;
		return 0;
	case QS_FS_WRITE:
		h = lookup(write_request->fd);
		if (!h)
			return fail(buffer, command);
		count = write_request->count;
		if (count > sizeof(write_request->data)) {
			errno = EINVAL;
			return fail(buffer, command);
		}
		n = write(h->host_fd, write_request->data, count);
		if (n < 0)
			return fail(buffer, command);
		return reply(buffer, command, (int32_t)n);
	case QS_FS_CLOSE:
		return close_handle(fd_request->fd) ?
		       fail(buffer, command) : reply(buffer, command, 0);
	case QS_FS_LSEEK:
		h = lookup(lseek_request->fd);
		if (!h)
			return fail(buffer, command);
		n = lseek(h->host_fd, lseek_request->offset, lseek_request->whence);
		return n < 0 ? fail(buffer, command) :
		       reply(buffer, command, (int32_t)n);
	case QS_FS_UNLINK:
	case QS_FS_REMOVE:
		if (normalize(path_request->pathname, path, sizeof(path)) ||
		    qs_open_parent(s, path, false, &parent, leaf, sizeof(leaf)))
			return fail(buffer, command);
		rc = unlinkat(parent, leaf, 0);
		close(parent);
		return rc ? fail(buffer, command) : reply(buffer, command, 0);
	case QS_FS_RMDIR:
		if (normalize(path_request->pathname, path, sizeof(path)) ||
		    qs_open_parent(s, path, false, &parent, leaf, sizeof(leaf)))
			return fail(buffer, command);
		rc = unlinkat(parent, leaf, AT_REMOVEDIR);
		close(parent);
		return rc ? fail(buffer, command) : reply(buffer, command, 0);
	case QS_FS_MKDIR:
		if (normalize(mkdir_request->pathname, path, sizeof(path)) ||
		    qs_open_parent(s, path, true, &parent, leaf, sizeof(leaf)))
			return fail(buffer, command);
		rc = mkdirat(parent, leaf, mkdir_request->mode & s->dir_mode);
		close(parent);
		if (rc && errno == EEXIST)
			rc = 0;
		return rc ? fail(buffer, command) : reply(buffer, command, 0);
	case QS_FS_SYNC:
		h = lookup(fd_request->fd);
		if (!h)
			return fail(buffer, command);
		return fsync(h->host_fd) ? fail(buffer, command) :
		       reply(buffer, command, 0);
	case QS_FS_RENAME:
		if (normalize(rename_request->old_filename, path, sizeof(path)) ||
		    qs_normalize_path(rename_request->new_filename,
				      sizeof(rename_request->new_filename),
				      path2, sizeof(path2)) ||
		    qs_open_parent(s, path, false, &parent, leaf, sizeof(leaf)))
			return fail(buffer, command);
		if (qs_open_parent(s, path2, true, &parent2, leaf2, sizeof(leaf2))) {
			rc = errno;
			close(parent);
			errno = rc;
			return fail(buffer, command);
		}
		rc = renameat(parent, leaf, parent2, leaf2);
		close(parent);
		close(parent2);
		return rc ? fail(buffer, command) : reply(buffer, command, 0);
	case QS_FS_GET_ERRNO:
		return reply(buffer, command, last_error);
	case QS_FS_END:
		return reply(buffer, command, 0);
	default:
		errno = ENOSYS;
		return fail(buffer, command);
	}
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reac_handle_priv.h"

#include <stdlib.h>
#include <unistd.h>

struct reac_handle *reac_handle_adopt(int fd)
{
	if (fd < 0)
		return NULL;
	struct reac_handle *h = malloc(sizeof *h);
	if (!h)
		return NULL;
	h->fd = fd;
	return h;
}

void reac_handle_close(struct reac_handle **h)
{
	if (!h || !*h)
		return;
	if ((*h)->fd >= 0)
		close((*h)->fd);
	free(*h);
	*h = NULL;
}

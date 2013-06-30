/***************************************************************************

 Copyright 2013 Intel Corporation.  All Rights Reserved.

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sub license, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial portions
 of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 IN NO EVENT SHALL INTEL, AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
 THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **************************************************************************/

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/ioctl.h>

#include <pciaccess.h>
#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <xf86_OSproc.h>

#include "intel_driver.h"

struct intel_device {
	char *path;
	int fd;
	int open_count;
	int master_count;
};

static int intel_device_key = -1;

static inline struct intel_device *intel_device(ScrnInfoPtr scrn)
{
	if (scrn->entityList == NULL)
		return NULL;

	return xf86GetEntityPrivate(scrn->entityList[0], intel_device_key)->ptr;
}

static inline void intel_set_device(ScrnInfoPtr scrn, struct intel_device *dev)
{
	xf86GetEntityPrivate(scrn->entityList[0], intel_device_key)->ptr = dev;
}

static int fd_set_cloexec(int fd)
{
	int flags;

	if (fd == -1)
		return fd;

#ifdef FD_CLOEXEC
	flags = fcntl(fd, F_GETFD);
	if (flags != -1) {
		flags |= FD_CLOEXEC;
		fcntl(fd, F_SETFD, flags);
	}
#endif

	return fd;
}

static int __intel_open_device(const struct pci_device *pci, char **path)
{
	int fd;

	if (*path == NULL) {
		char id[20];
		int ret;

		snprintf(id, sizeof(id),
			 "pci:%04x:%02x:%02x.%d",
			 pci->domain, pci->bus, pci->dev, pci->func);

		ret = drmCheckModesettingSupported(id);
		if (ret) {
			if (xf86LoadKernelModule("i915"))
				ret = drmCheckModesettingSupported(id);
			if (ret)
				return FALSE;
			/* Be nice to the user and load fbcon too */
			(void)xf86LoadKernelModule("fbcon");
		}

		fd = drmOpen(NULL, id);
		if (fd != -1) {
			*path = drmGetDeviceNameFromFd(fd);
			if (*path == NULL) {
				close(fd);
				fd = -1;
			}
		}
	} else {
#ifdef O_CLOEXEC
		fd = open(*path, O_RDWR | O_CLOEXEC);
#else
		fd = -1;
#endif
		if (fd == -1)
			fd = fd_set_cloexec(open(*path, O_RDWR));
	}

	return fd;
}

int intel_open_device(int entity_num,
		      const struct pci_device *pci,
		      const char *path)
{
	struct intel_device *dev;
	char *local_path;
	int fd;

	if (intel_device_key == -1)
		intel_device_key = xf86AllocateEntityPrivateIndex();
	if (intel_device_key == -1)
		return -1;

	dev = xf86GetEntityPrivate(entity_num, intel_device_key)->ptr;
	if (dev)
		return dev->fd;

	local_path = path ? strdup(path) : NULL;

	fd = __intel_open_device(pci, &local_path);
	if (fd == -1)
		return -1;

	dev = malloc(sizeof(*dev));
	if (dev == NULL) {
		free(local_path);
		close(fd);
		return -1;
	}

	dev->path = local_path;
	dev->fd = fd;
	dev->open_count = 0;
	dev->master_count = 0;

	xf86GetEntityPrivate(entity_num, intel_device_key)->ptr = dev;

	return fd;
}

int intel_get_device(ScrnInfoPtr scrn)
{
	struct intel_device *dev;
	int ret;

	dev = intel_device(scrn);
	assert(dev && dev->fd != -1);

	if (dev->open_count++ == 0) {
		drmSetVersion sv;
		int retry = 2000;

		/* Check that what we opened was a master or a
		 * master-capable FD, by setting the version of the
		 * interface we'll use to talk to it.
		 */
		do {
			sv.drm_di_major = 1;
			sv.drm_di_minor = 1;
			sv.drm_dd_major = -1;
			sv.drm_dd_minor = -1;
			ret = drmIoctl(dev->fd, DRM_IOCTL_SET_VERSION, &sv);
			if (ret == 0)
				break;

			usleep(1000);
		} while (--retry);
		if (ret != 0) {
			xf86DrvMsg(scrn->scrnIndex, X_ERROR,
				   "[drm] failed to set drm interface version: %s [%d].\n",
				   strerror(errno), errno);
			dev->open_count--;
			return -1;
		}
	}

	return dev->fd;
}

const char *intel_get_device_name(ScrnInfoPtr scrn)
{
	struct intel_device *dev = intel_device(scrn);
	assert(dev && dev->path);
	return dev->path;
}

int intel_get_master(ScrnInfoPtr scrn)
{
	struct intel_device *dev = intel_device(scrn);
	int ret;

	assert(dev && dev->fd != -1);

	ret = 0;
	if (dev->master_count++ == 0) {
		int retry = 2000;

		do {
			ret = drmSetMaster(dev->fd);
			if (ret == 0)
				break;
			usleep(1000);
		} while (--retry);
	}

	return ret;
}

int intel_put_master(ScrnInfoPtr scrn)
{
	struct intel_device *dev = intel_device(scrn);
	int ret;

	assert(dev && dev->fd != -1);

	ret = 0;
	assert(dev->master_count);
	if (--dev->master_count == 0) {
		assert(drmSetMaster(dev->fd) == 0);
		ret = drmDropMaster(dev->fd);
	}

	return ret;
}

void __intel_uxa_release_device(ScrnInfoPtr scrn)
{
	struct intel_device *dev = intel_device(scrn);
	if (dev && dev->open_count == 0) {
		intel_set_device(scrn, NULL);

		drmClose(dev->fd);
		free(dev->path);
		free(dev);
	}
}

void intel_put_device(ScrnInfoPtr scrn)
{
	struct intel_device *dev = intel_device(scrn);

	assert(dev && dev->fd != -1);

	assert(dev->open_count);
	if (--dev->open_count)
		return;

	intel_set_device(scrn, NULL);

	drmClose(dev->fd);
	free(dev->path);
	free(dev);
}
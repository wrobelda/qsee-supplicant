// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "qsee_protocol.h"
#include "qsee_supplicant.h"

struct qs_rpmb_device {
	int fd;
	uint32_t sectors;
	uint32_t reliable_frames;
};

int qs_rpmb_emmc_open(struct qs_rpmb_device *device, const char *path);
void qs_rpmb_emmc_close(struct qs_rpmb_device *device);
int qs_rpmb_emmc_read(struct qs_rpmb_device *device,
		      const struct qs_rpmb_frame *request, uint32_t count,
		      struct qs_rpmb_frame *response);
int qs_rpmb_emmc_write(struct qs_rpmb_device *device,
		       const struct qs_rpmb_frame *request, uint32_t count,
		       uint32_t batch, struct qs_rpmb_frame *response);

int qs_rpmb_open(const char *path);
void qs_rpmb_close(void);
extern const struct qs_service qs_rpmb_service;

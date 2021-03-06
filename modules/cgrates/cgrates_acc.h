/*
 * Copyright (C) 2017 Răzvan Crainea <razvan@opensips.org>
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#ifndef _CGRATES_ACC_H_
#define _CGRATES_ACC_H_

#include "../dialog/dlg_load.h"
#include "cgrates_common.h"

/*
 * Starts a CGR accounting session
 * Input:
 *  - account
 *  - destination (optional)
 * Returns:
 *  -  1: user is allowed to call
 *  - -1: internal error
 *  - -2: cgrates error
 *  - -3: invalid message type
 *  - -4: invalid message
 */
int w_cgr_acc(struct sip_msg* msg, char *flag_c, char* acc_c, char *dst_c);

struct cgr_acc_ctx {

	int ref_no;
	gen_lock_t ref_lock;

	unsigned flags;

	/* all branches info */
	str acc;
	str dst;
	time_t setup_time;
	time_t answer_time;
	unsigned int duration;

	/* variables */
	struct list_head *kv_store;
};

struct cgr_acc_ctx *cgr_tryget_acc_ctx(void);
void cgr_ref_acc_ctx(struct cgr_acc_ctx *ctx, int how, const char *who);
void cgr_loaded_callback(struct dlg_cell *dlg, int type,
			struct dlg_cb_params *_params);
int cgr_acc_terminate(json_object *param, json_object **ret);

#define CGRF_DO_CDR		(1<<0)
#define CGRF_DO_MISSED	(1<<1)

#endif /* _CGRATES_ACC_H_ */


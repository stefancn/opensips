/*
 * $Id: drouting.c,v 1.10 2007/10/03 14:10:48 daniel Exp $
 *
 * Copyright (C) 2005-2009 Voice Sistem SRL
 *
 * This file is part of Open SIP Server (OpenSIPS).
 *
 * DROUTING OpenSIPS-module is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * DROUTING OpenSIPS-module is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * For any questions about this software and its license, please contact
 * Voice Sistem at following e-mail address:
 *         office@voice-system.ro
 *
 * History:
 * ---------
 *  2005-02-20  first version (cristian)
 *  2005-02-27  ported to 0.9.0 (bogdan)
 */

#include "stdlib.h"
#include "stdio.h"
#include "assert.h"
#include <unistd.h>

#include "../../sr_module.h"
#include "../../str.h"
#include "../../dprint.h"
#include "../../usr_avp.h"
#include "../../db/db.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../locking.h"
#include "../../action.h"
#include "../../error.h"
#include "../../ut.h"
#include "../../resolve.h"
#include "../../parser/parse_from.h"
#include "../../parser/parse_uri.h"
#include "../../mi/mi.h"

#include "dr_load.h"
#include "prefix_tree.h"
#include "routing.h"
#include "dr_bl.h"


/*** DB relatede stuff ***/
/* parameters  */
static str db_url = {NULL,0};
static str drg_table = str_init("dr_groups");
static str drd_table = str_init("dr_gateways");
static str drr_table = str_init("dr_rules");
static str drl_table = str_init("dr_gw_lists");
/* DRG use domain */
static int use_domain = 1;
/**
 * - 0 - normal order
 * - 1 - random order, full set
 * - 2 - random order, one per set
 */
static int sort_order = 0;
int dr_fetch_rows = 2000;
int dr_force_dns = 1;

/* DRG table columns */
static str drg_user_col = str_init("username");
static str drg_domain_col = str_init("domain");
static str drg_grpid_col = str_init("groupid");
/* variables */
static db_con_t  *db_hdl=0;     /* DB handler */
static db_func_t dr_dbf;        /* DB functions */

/* current dr data - pointer to a pointer in shm */
static rt_data_t **rdata = 0;

/* AVP used to store serial RURIs */
static struct _ruri_avp{
	unsigned short type; /* AVP ID */
	int_str name; /* AVP name*/
}ruri_avp = { 0, {.n=(int)0xad346b2f} };
static str ruri_avp_spec = {0,0};

/* AVP used to store serial ATTRs */
static struct _attrs_avp{
	unsigned short type; /* AVP ID */
	int_str name; /* AVP name*/
}attrs_avp = { 0, {.n=(int)0xad346b30} };
static str attrs_avp_spec = {0,0};

/* statistic data */
int tree_size = 0;
int inode = 0;
int unode = 0;

/* lock, ref counter and flag used for reloading the date */
static gen_lock_t *ref_lock = 0;
static volatile int data_refcnt = 0;
static volatile int reload_flag = 0;

static int dr_init(void);
static int dr_child_init(int rank);
static int dr_exit(void);

static int fixup_do_routing(void** param, int param_no);
static int fixup_from_gw(void** param, int param_no);

static int do_routing(struct sip_msg* msg, dr_group_t *drg);
static int do_routing_0(struct sip_msg* msg, char* str1, char* str2);
static int do_routing_1(struct sip_msg* msg, char* str1, char* str2);
static int use_next_gw(struct sip_msg* msg);
static int is_from_gw_0(struct sip_msg* msg, char* str1, char* str2);
static int is_from_gw_1(struct sip_msg* msg, char* str1, char* str2);
static int is_from_gw_2(struct sip_msg* msg, char* str1, char* str2);
static int goes_to_gw_0(struct sip_msg* msg, char* f1, char* f2);
static int goes_to_gw_1(struct sip_msg* msg, char* f1, char* f2);

static struct mi_root* dr_reload_cmd(struct mi_root *cmd_tree, void *param);
static int mi_child_init();

#define RELOAD_MI_CMD  "DR_RELOAD"


MODULE_VERSION

/*
 * Exported functions
 */
static cmd_export_t cmds[] = {
	{"do_routing",  (cmd_function)do_routing_0,   0,  0, 0,
		REQUEST_ROUTE|FAILURE_ROUTE},
	{"do_routing",  (cmd_function)do_routing_1,   1,  fixup_do_routing, 0,
		REQUEST_ROUTE|FAILURE_ROUTE},
	{"use_next_gw",  (cmd_function)use_next_gw,   0,  0, 0,
		REQUEST_ROUTE|FAILURE_ROUTE},
	{"next_routing",  (cmd_function)use_next_gw, 0,  0, 0,
		FAILURE_ROUTE},
	{"is_from_gw",  (cmd_function)is_from_gw_0,   0,  0, 0,
		REQUEST_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE},
	{"is_from_gw",  (cmd_function)is_from_gw_1,   1,  fixup_from_gw, 0,
		REQUEST_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE},
	{"is_from_gw",  (cmd_function)is_from_gw_2,   2,  fixup_from_gw, 0,
		REQUEST_ROUTE},
	{"goes_to_gw",  (cmd_function)goes_to_gw_0,   0,  0, 0,
		REQUEST_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE},
	{"goes_to_gw",  (cmd_function)goes_to_gw_1,   1,  fixup_from_gw, 0,
		REQUEST_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE},
	{0, 0, 0, 0, 0, 0}
};


/*
 * Exported parameters
 */
static param_export_t params[] = {
	{"db_url",          STR_PARAM, &db_url.s        },
	{"drd_table",       STR_PARAM, &drd_table.s     },
	{"drr_table",       STR_PARAM, &drr_table.s     },
	{"drg_table",       STR_PARAM, &drg_table.s     },
	{"drl_table",       STR_PARAM, &drl_table.s     },
	{"use_domain",      INT_PARAM, &use_domain      },
	{"drg_user_col",    STR_PARAM, &drg_user_col.s  },
	{"drg_domain_col",  STR_PARAM, &drg_domain_col.s},
	{"drg_grpid_col",   STR_PARAM, &drg_grpid_col.s },
	{"ruri_avp",        STR_PARAM, &ruri_avp_spec.s },
	{"attrs_avp",       STR_PARAM, &ruri_avp_spec.s },
	{"sort_order",      INT_PARAM, &sort_order      },
	{"fetch_rows",      INT_PARAM, &dr_fetch_rows   },
	{"force_dns",       INT_PARAM, &dr_force_dns    },
	{"define_blacklist",STR_PARAM|USE_FUNC_PARAM, (void*)set_dr_bl },
	{0, 0, 0}
};


/*
 * Exported MI functions
 */
static mi_export_t mi_cmds[] = {
	{ RELOAD_MI_CMD, dr_reload_cmd, MI_NO_INPUT_FLAG, 0, mi_child_init },
	{ 0, 0, 0, 0, 0}
};



struct module_exports exports = {
	"drouting",
	DEFAULT_DLFLAGS, /* dlopen flags */
	cmds,            /* Exported functions */
	params,          /* Exported parameters */
	0,               /* exported statistics */
	mi_cmds,         /* exported MI functions */
	0,               /* exported pseudo-variables */
	0,               /* additional processes */
	dr_init,         /* Module initialization function */
	(response_function) 0,
	(destroy_function) dr_exit,
	(child_init_function) dr_child_init /* per-child init function */
};



static inline int dr_reload_data( void )
{
	rt_data_t *new_data;
	rt_data_t *old_data;

	new_data = dr_load_routing_info( &dr_dbf, db_hdl,
		&drd_table, &drl_table, &drr_table);
	if ( new_data==0 ) {
		LM_CRIT("failed to load routing info\n");
		return -1;
	}

	/* block access to data for all readers */
	lock_get( ref_lock );
	reload_flag = 1;
	lock_release( ref_lock );

	/* wait for all readers to finish - it's a kind of busy waitting but
	 * it's not critical;
	 * at this point, data_refcnt can only be decremented */
	while (data_refcnt) {
		usleep(10);
	}

	/* no more activ readers -> do the swapping */
	old_data = *rdata;
	*rdata = new_data;

	/* release the readers */
	reload_flag = 0;

	/* destroy old data */
	if (old_data)
		free_rt_data( old_data, 1 );

	/* generate new blacklist from the routing info */
	populate_dr_bls((*rdata)->pgw_addr_l);

	return 0;
}



static int dr_init(void)
{
	pv_spec_t avp_spec;

	LM_INFO("Dynamic-Routing - initializing\n");

	/* check the module params */
	if (db_url.s==NULL || db_url.s[0]==0) {
		LM_CRIT("mandatory parameter \"DB_URL\" found empty\n");
		goto error;
	}
	db_url.len = strlen(db_url.s);

	drd_table.len = strlen(drd_table.s);
	if (drd_table.s[0]==0) {
		LM_CRIT("mandatory parameter \"DRD_TABLE\" found empty\n");
		goto error;
	}

	drr_table.len = strlen(drr_table.s);
	if (drr_table.s[0]==0) {
		LM_CRIT("mandatory parameter \"DRR_TABLE\" found empty\n");
		goto error;
	}

	drg_table.len = strlen(drg_table.s);
	if (drg_table.s[0]==0) {
		LM_CRIT("mandatory parameter \"DRG_TABLE\"  found empty\n");
		goto error;
	}

	drl_table.len = strlen(drl_table.s);
	if (drl_table.s[0]==0) {
		LM_CRIT("mandatory parameter \"DRL_TABLE\"  found empty\n");
		goto error;
	}

	drg_user_col.len = strlen(drg_user_col.s);
	drg_domain_col.len = strlen(drg_domain_col.s);
	drg_grpid_col.len = strlen(drg_grpid_col.s);

	/* fix AVP spec */
	if (ruri_avp_spec.s) {
		ruri_avp_spec.len = strlen(ruri_avp_spec.s);

		if (pv_parse_spec( &ruri_avp_spec, &avp_spec)==0
		|| avp_spec.type!=PVT_AVP) {
			LM_ERR("malformed or non AVP [%.*s] for RURI AVP definition\n",
				ruri_avp_spec.len, ruri_avp_spec.s);
			return E_CFG;
		}

		if( pv_get_avp_name(0, &(avp_spec.pvp), &(ruri_avp.name),
		&(ruri_avp.type) )!=0) {
			LM_ERR("[%.*s]- invalid AVP definition for RURI AVP\n",
				ruri_avp_spec.len, ruri_avp_spec.s);
			return E_CFG;
		}
	}
	if (attrs_avp_spec.s) {
		attrs_avp_spec.len = strlen(attrs_avp_spec.s);

		if (pv_parse_spec( &attrs_avp_spec, &avp_spec)==0
		|| avp_spec.type!=PVT_AVP) {
			LM_ERR("malformed or non AVP [%.*s] for ATTRS AVP definition\n",
				attrs_avp_spec.len, attrs_avp_spec.s);
			return E_CFG;
		}

		if( pv_get_avp_name(0, &(avp_spec.pvp), &(attrs_avp.name),
		&(attrs_avp.type) )!=0) {
			LM_ERR("[%.*s]- invalid AVP definition for ATTRS AVP\n",
				attrs_avp_spec.len, attrs_avp_spec.s);
			return E_CFG;
		}
	}

	if (init_dr_bls()!=0) {
		LM_ERR("failed to init DR blacklists\n");
		return E_CFG;
	}

	/* data pointer in shm */
	rdata = (rt_data_t**)shm_malloc( sizeof(rt_data_t*) );
	if (rdata==0) {
		LM_CRIT("failed to get shm mem for data ptr\n");
		goto error;
	}
	*rdata = 0;

	/* create & init lock */
	if ( (ref_lock=lock_alloc())==0) {
		LM_CRIT("failed to alloc ref_lock\n");
		goto error;
	}
	if (lock_init(ref_lock)==0 ) {
		LM_CRIT("failed to init ref_lock\n");
		goto error;
	}

	/* bind to the mysql module */
	if (db_bind_mod( &db_url, &dr_dbf  )) {
		LM_CRIT("cannot bind to database module! "
			"Did you forget to load a database module ?\n");
		goto error;
	}

	if (!DB_CAPABILITY( dr_dbf, DB_CAP_QUERY)) {
		LM_CRIT( "database modules does not "
			"provide QUERY functions needed by DRounting module\n");
		return -1;
	}

	return 0;
error:
	if (ref_lock) {
		lock_destroy( ref_lock );
		lock_dealloc( ref_lock );
		ref_lock = 0;
	}
	if (db_hdl) {
		dr_dbf.close(db_hdl);
		db_hdl = 0;
	}
	if (rdata) {
		shm_free(rdata);
		rdata = 0;
	}
	return -1;
}



static int dr_child_init(int rank)
{
	/* only workers needs DB connection */
	if (rank <=PROC_MAIN)
		return 0;

	/* init DB connection */
	if ( (db_hdl=dr_dbf.init(&db_url))==0 ) {
		LM_CRIT("cannot initialize database connection\n");
		return -1;
	}

	/* child 1 load the routing info */
	if ( (rank==1) && dr_reload_data()!=0 ) {
		LM_CRIT("failed to load routing data\n");
		return -1;
	}

	/* set GROUP table for workers */
	if (dr_dbf.use_table( db_hdl, &drg_table) < 0) {
		LM_ERR("cannot select table \"%.*s\"\n", drg_table.len, drg_table.s);
		return -1;
	}
	srand(getpid()+time(0)+rank);
	return 0;
}


static int mi_child_init( void )
{
	/* init DB connection */
	if ( (db_hdl=dr_dbf.init(&db_url))==0 ) {
		LM_CRIT("cannot initialize database connection\n");
		return -1;
	}
	return 0;
}


static int dr_exit(void)
{
	/* close DB connection */
	if (db_hdl) {
		dr_dbf.close(db_hdl);
		db_hdl = 0;
	}

	/* destroy data */
	if ( rdata) {
		if (*rdata)
			free_rt_data( *rdata, 1 );
		shm_free( rdata );
		rdata = 0;
	}

	/* destroy lock */
	if (ref_lock) {
		lock_destroy( ref_lock );
		lock_dealloc( ref_lock );
		ref_lock = 0;
	}

	/* destroy blacklists */
	destroy_dr_bls();

	return 0;
}



static struct mi_root* dr_reload_cmd(struct mi_root *cmd_tree, void *param)
{
	int n;

	LM_INFO("\"%s\" MI command received!\n",RELOAD_MI_CMD);

	if ( (n=dr_reload_data())!=0 ) {
		LM_CRIT("failed to load routing data\n");
		goto error;
	}

	return init_mi_tree( 200, MI_OK_S, MI_OK_LEN);
error:
	return init_mi_tree( 500, "Failed to reload",16);
}



static inline int get_group_id(struct sip_uri *uri)
{
	db_key_t keys_ret[1];
	db_key_t keys_cmp[2];
	db_val_t vals_cmp[2];
	db_res_t* res;
	int n;


	/* user */
	keys_cmp[0] = &drg_user_col;
	vals_cmp[0].type = DB_STR;
	vals_cmp[0].nul  = 0;
	vals_cmp[0].val.str_val = uri->user;
	n = 1;

	if (use_domain) {
		keys_cmp[1] = &drg_domain_col;
		vals_cmp[1].type = DB_STR;
		vals_cmp[1].nul  = 0;
		vals_cmp[1].val.str_val = uri->host;
		n++;
	}

	keys_ret[0] = &drg_grpid_col;
	res = 0;

	if ( dr_dbf.query(db_hdl,keys_cmp,0,vals_cmp,keys_ret,n,1,0,&res)<0 ) {
		LM_ERR("DB query failed\n");
		goto error;
	}

	if (RES_ROW_N(res) == 0) {
		LM_ERR("no group for user "
			"\"%.*s\"@\"%.*s\"\n", uri->user.len, uri->user.s,
			uri->host.len, uri->host.s);
		goto error;
	}
	if (res->rows[0].values[0].nul || res->rows[0].values[0].type!=DB_INT) {
		LM_ERR("null or non-integer group_id\n");
		goto error;
	}
	n = res->rows[0].values[0].val.int_val;

	dr_dbf.free_result(db_hdl, res);
	return n;
error:
	if (res)
		dr_dbf.free_result(db_hdl, res);
	return -1;
}



static inline str* build_ruri(struct sip_uri *uri, int strip, str *pri,
																str *hostport)
{
	static str uri_str;
	char *p;

	if (uri->user.len<=strip) {
		LM_ERR("stripping %d makes "
			"username <%.*s> null\n",strip,uri->user.len,uri->user.s);
		return 0;
	}

	uri_str.len = 4 /*sip:*/ + uri->user.len - strip +pri->len +
		(uri->passwd.s?(uri->passwd.len+1):0) + 1/*@*/ + hostport->len +
		(uri->params.s?(uri->params.len+1):0) +
		(uri->headers.s?(uri->headers.len+1):0);

	if ( (uri_str.s=(char*)pkg_malloc( uri_str.len + 1))==0) {
		LM_ERR("no more pkg mem\n");
		return 0;
	}

	p = uri_str.s;
	*(p++)='s';
	*(p++)='i';
	*(p++)='p';
	*(p++)=':';
	if (pri->len) {
		memcpy(p, pri->s, pri->len);
		p += pri->len;
	}
	memcpy(p, uri->user.s+strip, uri->user.len-strip);
	p += uri->user.len-strip;
	if (uri->passwd.len) {
		*(p++)=':';
		memcpy(p, uri->passwd.s, uri->passwd.len);
		p += uri->passwd.len;
	}
	*(p++)='@';
	memcpy(p, hostport->s, hostport->len);
	p += hostport->len;
	if (uri->params.len) {
		*(p++)=';';
		memcpy(p, uri->params.s, uri->params.len);
		p += uri->params.len;
	}
	if (uri->headers.len) {
		*(p++)='?';
		memcpy(p, uri->headers.s, uri->headers.len);
		p += uri->headers.len;
	}
	*p = 0;

	if (p-uri_str.s!=uri_str.len) {
		LM_CRIT("difference between allocated(%d)"
			" and written(%d)\n",uri_str.len,(int)(long)(p-uri_str.s));
		return 0;
	}
	return &uri_str;
}


static int do_routing_0(struct sip_msg* msg, char* str1, char* str2)
{
	return do_routing(msg, NULL);
}

static int do_routing_1(struct sip_msg* msg, char* str1, char* str2)
{
	return do_routing(msg, (dr_group_t*)str1);
}


static int use_next_gw(struct sip_msg* msg)
{
	struct usr_avp *avp;
	int_str val;

	/* search for the first RURI AVP containing a string */
	do {
		avp = search_first_avp(ruri_avp.type, ruri_avp.name, &val, 0);
	}while (avp && (avp->flags&AVP_VAL_STR)==0 );

	if (!avp) return -1;

	if (set_ruri( msg, &val.s)==-1) {
		LM_ERR("failed to rewite RURI\n");
		return -1;
	}
	destroy_avp(avp);

	/* remove the old attrs */
	do {
		avp = search_first_avp(attrs_avp.type, attrs_avp.name, &val, 0);
	}while (avp && (avp->flags&AVP_VAL_STR)==0 );

	if (avp)
		destroy_avp(avp);

	LM_DBG("setting new RURI <%.*s>\n", val.s.len,val.s.s);
	return 1;
}


static int do_routing(struct sip_msg* msg, dr_group_t *drg)
{
	struct to_body  *from;
	struct sip_uri  uri;
	rt_info_t      *rt_info;
	int    grp_id;
	int    i, j, l, t;
	str    *ruri;
	int_str val;
	struct usr_avp *avp;
#define DR_MAX_GWLIST	32
	static int local_gwlist[DR_MAX_GWLIST];
	int gwlist_size;
	int ret;

	ret = -1;

	if ( (*rdata)==0 || (*rdata)->pgw_l==0 ) {
		LM_DBG("empty ruting table\n");
		goto error1;
	}

	/* get the username from FROM_HDR */
	if (parse_from_header(msg)!=0) {
		LM_ERR("unable to parse from hdr\n");
		goto error1;
	}
	from = (struct to_body*)msg->from->parsed;
	/* parse uri */
	if (parse_uri( from->uri.s, from->uri.len, &uri)!=0) {
		LM_ERR("unable to parse from uri\n");
		goto error1;
	}

	/* get user's routing group */
	if(drg==NULL)
	{
		grp_id = get_group_id( &uri );
		if (grp_id<0) {
			LM_ERR("failed to get group id\n");
			goto error1;
		}
	} else {
		if(drg->type==0)
			grp_id = (int)drg->u.grp_id;
		else if(drg->type==1) {
			grp_id = 0; /* call get avp here */
			if((avp=search_first_avp( drg->u.avp_id.type,
			drg->u.avp_id.name, &val, 0))==NULL||(avp->flags&AVP_VAL_STR)) {
				LM_ERR( "failed to get group id\n");
				goto error1;
			}
			grp_id = val.n;
		} else
			grp_id = 0; 
	}
	LM_DBG("using dr group %d\n",grp_id);

	/* get the number */
	ruri = GET_RURI(msg);
	/* parse ruri */
	if (parse_uri( ruri->s, ruri->len, &uri)!=0) {
		LM_ERR("unable to parse RURI\n");
		goto error1;
	}

	/* ref the data for reading */
again:
	lock_get( ref_lock );
	/* if reload must be done, do un ugly busy waiting 
	 * until reload is finished */
	if (reload_flag) {
		lock_release( ref_lock );
		usleep(5);
		goto again;
	}
	data_refcnt++;
	lock_release( ref_lock );

	/* search a prefix */
	rt_info = get_prefix( (*rdata)->pt, &uri.user , (unsigned int)grp_id);
	if (rt_info==0) {
		LM_DBG("no matching for prefix \"%.*s\"\n",
			uri.user.len, uri.user.s);
		/* try prefixless rules */
		rt_info = check_rt( &(*rdata)->noprefix, (unsigned int)grp_id);
		if (rt_info==0) {
			LM_DBG("no prefixless matching for "
				"grp %d\n", grp_id);
			goto error2;
		}
	}

	if (rt_info->route_idx>0 && rt_info->route_idx<RT_NO) {
		ret = run_top_route( rlist[rt_info->route_idx], msg );
		if (ret<1) {
			/* drop the action */
			LM_DBG("script route %d drops routing "
				"by %d\n", rt_info->route_idx, ret);
			goto error2;
		}
		ret = -1;
	}

	gwlist_size
		= (rt_info->pgwa_len>DR_MAX_GWLIST)?DR_MAX_GWLIST:rt_info->pgwa_len;
	
	/* set gw order */
	if(sort_order>=1&&gwlist_size>1)
	{
		j = 0;
		t = 0;
		while(j<gwlist_size)
		{
			/* identify the group: [j..i) */
			for(i=j+1; i<gwlist_size; i++)
				if(rt_info->pgwl[j].grpid!=rt_info->pgwl[i].grpid)
					break;
			if(i-j==1)
			{
				local_gwlist[t++] = j;
				/*LM_DBG("selected gw[%d]=%d\n",
					j, local_gwlist[j]);*/
			} else {
				if(i-j==2)
				{
					local_gwlist[t++]   = j + rand()%2;
					if(sort_order==1)
					{
						local_gwlist[t++] = j + (local_gwlist[j]-j+1)%2;
						/*LM_DBG("selected gw[%d]=%d"
						 *  " gw[%d]=%d\n", j, local_gwlist[j], j+1,
						 *  local_gwlist[j+1]);*/
					}
				} else {
					local_gwlist[t++]   = j + rand()%(i-j);
					if(sort_order==1)
					{
						do{
							local_gwlist[t] = j + rand()%(i-j);
						}while(local_gwlist[t]==local_gwlist[t-1]);
						t++;

						/*
						LM_DBG("selected gw[%d]=%d"
							" gw[%d]=%d.\n",
							j, local_gwlist[j], j+1, local_gwlist[j+1]); */
						/* add the rest in this group */
						for(l=j; l<i; l++)
						{
							if(l==local_gwlist[j] || l==local_gwlist[j+1])
								continue;
							local_gwlist[t++] = l;
							/* LM_DBG("selected gw[%d]=%d.\n",
								j+k, local_gwlist[t]); */
						}
					}
				}
			}
			/* next group starts from i */
			j=i;
		}
	} else {
		for(i=0; i<gwlist_size; i++)
			local_gwlist[i] = i;
		t = i;
	}

	/* do some cleanup first */
	destroy_avps( ruri_avp.type, ruri_avp.name, 1);

	/* push gwlist into avps in reverse order */
	for( j=t-1 ; j>=1 ; j-- ) {
		/* build uri*/
		ruri = build_ruri(&uri, rt_info->pgwl[local_gwlist[j]].pgw->strip,
				&rt_info->pgwl[local_gwlist[j]].pgw->pri,
				&rt_info->pgwl[local_gwlist[j]].pgw->ip);
		if (ruri==0) {
			LM_ERR("failed to build avp ruri\n");
			goto error2;
		}
		LM_DBG("adding gw [%d] as avp \"%.*s\"\n",
			local_gwlist[j], ruri->len, ruri->s);
		/* add ruri avp */
		val.s = *ruri;
		if (add_avp( AVP_VAL_STR|(ruri_avp.type),ruri_avp.name, val)!=0 ) {
			LM_ERR("failed to insert ruri avp\n");
			pkg_free(ruri->s);
			goto error2;
		}
		pkg_free(ruri->s);
		/* add attrs avp */
		val.s = rt_info->pgwl[local_gwlist[j]].pgw->attrs;
		if (add_avp( AVP_VAL_STR|(attrs_avp.type),attrs_avp.name, val)!=0 ) {
			LM_ERR("failed to insert attrs avp\n");
			goto error2;
		}
	}

	/* use first GW in RURI */
	ruri = build_ruri(&uri, rt_info->pgwl[local_gwlist[0]].pgw->strip,
			&rt_info->pgwl[local_gwlist[0]].pgw->pri,
			&rt_info->pgwl[local_gwlist[0]].pgw->ip);

	/* add attrs avp */
	val.s = rt_info->pgwl[local_gwlist[0]].pgw->attrs;
	if (add_avp( AVP_VAL_STR|(attrs_avp.type),attrs_avp.name, val)!=0 ) {
		LM_ERR("failed to insert attrs avp\n");
		goto error2;
	}

	/* we are done reading -> unref the data */
	lock_get( ref_lock );
	data_refcnt--;
	lock_release( ref_lock );

	/* what hev we get here?? */
	if (ruri==0) {
		LM_ERR("failed to build ruri\n");
		goto error1;
	}
	LM_DBG("setting the gw [%d] as ruri \"%.*s\"\n",
			local_gwlist[0], ruri->len, ruri->s);
	if (msg->new_uri.s)
		pkg_free(msg->new_uri.s);
	msg->new_uri = *ruri;
	msg->parsed_uri_ok = 0;

	return 1;
error2:
	/* we are done reading -> unref the data */
	lock_get( ref_lock );
	data_refcnt--;
	lock_release( ref_lock );
error1:
	return ret;
}


static int fixup_do_routing(void** param, int param_no)
{
	char *s;
	dr_group_t *drg;
	pv_spec_t avp_spec;
	str r;

	s = (char*)*param;

	if (param_no==1)
	{
		drg = (dr_group_t*)pkg_malloc(sizeof(dr_group_t));
		if(drg==NULL)
		{
			LM_ERR( "no more memory\n");
			return E_OUT_OF_MEM;
		}
		memset(drg, 0, sizeof(dr_group_t));

		if ( s==NULL || s[0]==0 ) {
			LM_CRIT("empty group id definition");
			return E_CFG;
		}

		if (s[0]=='$') {
			/* param is a PV (AVP only supported) */
			r.s = s;
			r.len = strlen(s);
			if (pv_parse_spec( &r, &avp_spec)==0
			|| avp_spec.type!=PVT_AVP) {
				LM_ERR("malformed or non AVP %s AVP definition\n", s);
				return E_CFG;
			}

			if( pv_get_avp_name(0, &(avp_spec.pvp), &(drg->u.avp_id.name),
			&(drg->u.avp_id.type) )!=0) {
				LM_ERR("[%s]- invalid AVP definition\n", s);
				return E_CFG;
			}
			drg->type = 1;
		} else {
			while(s && *s) {
				if(*s<'0' || *s>'9') {
					LM_ERR( "bad number\n");
					return E_UNSPEC;
				}
				drg->u.grp_id = (drg->u.grp_id)*10+(*s-'0');
				s++;
			}
		}
		pkg_free(*param);
		*param = (void*)drg;
	}

	return 0;
}


static int fixup_from_gw( void** param, int param_no)
{
	unsigned long type;
	int err;

	if (param_no == 1 || param_no == 2) {
		type = str2s(*param, strlen(*param), &err);
		if (err == 0) {
			pkg_free(*param);
			*param = (void *)type;
			return 0;
		} else {
			LM_ERR( "bad number <%s>\n",
				(char *)(*param));
			return E_CFG;
		}
	}
	return 0;
}

static int strip_username(struct sip_msg* msg, int strip)
{
	struct action act;
 
	act.type = STRIP_T;
	act.elem[0].type = NUMBER_ST;
	act.elem[0].u.number = strip;
	act.next = 0;

	if (do_action(&act, msg) < 0)
	{
		LM_ERR( "Error in do_action\n");
		return -1;
	}
	return 0;
}


static int is_from_gw_0(struct sip_msg* msg, char* str, char* str2)
{
	pgw_addr_t *pgwa = NULL;

	if(rdata==NULL || *rdata==NULL || msg==NULL)
		return -1;
	
	pgwa = (*rdata)->pgw_addr_l;
	while(pgwa) {
		if(ip_addr_cmp(&pgwa->ip, &msg->rcv.src_ip))
			return 1;
		pgwa = pgwa->next;
	}
	return -1;
}


static int is_from_gw_1(struct sip_msg* msg, char* str, char* str2)
{
	pgw_addr_t *pgwa = NULL;
	int type = (int)(long)str;

	if(rdata==NULL || *rdata==NULL || msg==NULL)
		return -1;
	
	pgwa = (*rdata)->pgw_addr_l;
	while(pgwa) {
		if( type==pgwa->type && ip_addr_cmp(&pgwa->ip, &msg->rcv.src_ip) )
			return 1;
		pgwa = pgwa->next;
	}
	return -1;
}

static int is_from_gw_2(struct sip_msg* msg, char* str1, char* str2)
{
	pgw_addr_t *pgwa = NULL;
	int type = (int)(long)str1;
	int flags = (int)(long)str2;

	if(rdata==NULL || *rdata==NULL || msg==NULL)
		return -1;
	
	pgwa = (*rdata)->pgw_addr_l;
	while(pgwa) {
		if( type==pgwa->type && ip_addr_cmp(&pgwa->ip, &msg->rcv.src_ip) ) {
			if (flags!=0 && pgwa->strip>0)
				strip_username(msg, pgwa->strip);
			return 1;
		}
		pgwa = pgwa->next;
	}
	return -1;
}


static int goes_to_gw_1(struct sip_msg* msg, char* _type, char* _f2)
{
	pgw_addr_t *pgwa = NULL;
	struct sip_uri puri;
	struct ip_addr *ip;
	str *uri;
	int type;

	if(rdata==NULL || *rdata==NULL || msg==NULL)
		return -1;

	uri = GET_NEXT_HOP(msg);
	type = (int)(long)_type;

	if (parse_uri(uri->s, uri->len, &puri)<0){
		LM_ERR("bad uri <%.*s>\n", uri->len, uri->s);
		return -1;
	}

	if ( ((ip=str2ip(&puri.host))!=0)
#ifdef USE_IPV6
	|| ((ip=str2ip6(&puri.host))!=0)
#endif
	){
		pgwa = (*rdata)->pgw_addr_l;
		while(pgwa) {
			if( (type<0 || type==pgwa->type) && ip_addr_cmp(&pgwa->ip, ip))
				return 1;
			pgwa = pgwa->next;
		}
	}

	return -1;
}


static int goes_to_gw_0(struct sip_msg* msg, char* _type, char* _f2)
{
	return goes_to_gw_1(msg, (char*)(long)-1, _f2);
}


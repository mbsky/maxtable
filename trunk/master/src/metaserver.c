/*
** metaserver.c 2010-06-15 xueyingfei
**
** Copyright flying/xueyingfei.
**
** This file is part of MaxTable.
**
** Licensed under the Apache License, Version 2.0
** (the "License"); you may not use this file except in compliance with
** the License. You may obtain a copy of the License at
**
** http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
** implied. See the License for the specific language governing
** permissions and limitations under the License.
*/

#include <dirent.h>
#include "global.h"
#include "utils.h"
#include "list.h"
#include "master/metaserver.h"
#include "ranger/rangeserver.h"
#include "conf.h"
#include "netconn.h"
#include "parser.h"
#include "tss.h"
#include "token.h"
#include "memcom.h"
#include "row.h"
#include "file_op.h"
#include "strings.h"
#include "type.h"
#include "buffer.h"
#include "block.h"
#include "cache.h"
#include "metadata.h"
#include "tablet.h"
#include "trace.h"
#include "session.h"
#include "tabinfo.h"
#include "sstab.h"
#include "rebalancer.h"
#include "thread.h"
#include "log.h"
#include "m_socket.h"

extern	TSS	*Tss;
extern	char	Kfsserver[32];
extern	int	Kfsport;

#define SSTAB_FREE	0
#define SSTAB_USED	1
#define SSTAB_RESERVED	2

#define	META_RECOVERY_INTERVAL	5

SSTAB_INFOR	*sstab_map;

TAB_SSTAB_MAP *tab_sstabmap;


#define SSTAB_MAP_SET(i, flag)	(sstab_map[i].sstab_stat = flag)

#define SSTAB_MAP_FREE(i)	(sstab_map[i].sstab_stat == SSTAB_FREE)
#define SSTAB_MAP_USED(i)	(sstab_map[i].sstab_stat == SSTAB_USED)
#define SSTAB_MAP_RESERV(i)	(sstab_map[i].sstab_stat == SSTAB_RESERVED)

#define SSTAB_MAP_GET_SPLIT_TS(i)		(sstab_map[i].split_ts)
#define SSTAB_MAP_SET_SPLIT_TS(i, split_ts)	(sstab_map[i].split_ts = (unsigned int)split_ts)



MASTER_INFOR	*Master_infor = NULL;

extern pthread_mutex_t mutex;
extern pthread_cond_t cond;

extern int msg_list_len;
extern MSG_DATA * msg_list_head;
extern MSG_DATA * msg_list_tail;


struct stat st;


static void
meta_heartbeat_setup(RANGE_PROF * rg_addr);

static int
meta_failover_rg(char * req_buf);

static int
meta_get_free_sstab();

static void
meta_update();

static RANGE_PROF *
meta_get_rg();

static RANGE_PROF *
meta_get_rg_by_ip_port(char *rgip, int rgport);

static int
meta_collect_rg(char * req_buf);

static void
meta_save_rginfo();

static char *
meta_checkdata(TREE *command);

static char *
meta_checkranger(TREE *command);

static int
meta_table_is_exist(char *tabname);

static int
meta_load_sysmeta();



void
meta_bld_rglist(char *filepath)
{
	;
}


void 
meta_server_setup(char *conf_path)
{
	int	status;
	char	port[32];
	int	metaport;
	char	rang_server[256];
	int	fd;
	SVR_IDX_FILE	*filebuf;
	int i;


	MEMSET(port, 32);
	Master_infor = MEMALLOCHEAP(sizeof(MASTER_INFOR));
	MEMCPY(Master_infor->conf_path, conf_path, STRLEN(conf_path));

	Master_infor->meta_systab = malloc(sizeof(META_SYSTABLE));
	memset(Master_infor->meta_systab, 0, sizeof(META_SYSTABLE));

	Master_infor->meta_sysobj = malloc(sizeof(META_SYSOBJECT));
	memset(Master_infor->meta_sysobj, 0, sizeof(META_SYSOBJECT));

	Master_infor->meta_syscol = malloc(sizeof(META_SYSCOLUMN));
	memset(Master_infor->meta_syscol, 0, sizeof(META_SYSCOLUMN));

	conf_get_value_by_key(port, conf_path, CONF_PORT_KEY);

	metaport = m_atoi(port, STRLEN(port));
	if(metaport != INDEFINITE)
	{
		Master_infor->port = metaport;
	}
	else
	{
		Master_infor->port = META_DEFAULT_PORT;
	}

	SPINLOCK_ATTR_INIT(Master_infor->mutexattr);
	SPINLOCK_ATTR_SETTYPE(Master_infor->mutexattr, PTHREAD_MUTEX_RECURSIVE);
	SPINLOCK_INIT(Master_infor->rglist_spinlock, &(Master_infor->mutexattr));
	
#ifdef MT_KFS_BACKEND
	MEMSET(Kfsserver, 32);
	conf_get_value_by_key(Kfsserver, conf_path, CONF_KFS_IP);
	conf_get_value_by_key(port, conf_path, CONF_KFS_PORT);

	Kfsport = m_atoi(port, STRLEN(port));

#endif
	if (STAT(MT_META_TABLE, &st) != 0)
	{
		MKDIR(status, MT_META_TABLE, 0755);

		MEMSET(rang_server, 256);
		MEMCPY(rang_server, MT_META_TABLE, STRLEN(MT_META_TABLE));
		str1_to_str2(rang_server, '/', "systable");

		OPEN(fd, rang_server, (O_CREAT|O_WRONLY|O_TRUNC));
			
		WRITE(fd, Master_infor->meta_systab, sizeof(META_SYSTABLE));
	
		CLOSE(fd);		

		Master_infor->last_tabid = 0;
	}
	else
	{		
		MEMSET(rang_server, 256);
		MEMCPY(rang_server, MT_META_TABLE, STRLEN(MT_META_TABLE));
		str1_to_str2(rang_server, '/', "systable");

		OPEN(fd, rang_server, (O_RDONLY));
		
		READ(fd, Master_infor->meta_systab, sizeof(META_SYSTABLE));
	
		CLOSE(fd);

		Master_infor->last_tabid = Master_infor->meta_systab->last_tabid;
	}

	meta_load_sysmeta();

	if (STAT(MT_META_REGION, &st) != 0)
	{
		MKDIR(status, MT_META_REGION, 0755); 

		MEMSET(rang_server, 256);
		MEMCPY(rang_server, MT_META_REGION, STRLEN(MT_META_REGION));
		str1_to_str2(rang_server, '/', "rangeserverlist");
	
		OPEN(fd, rang_server, (O_CREAT|O_WRONLY|O_TRUNC));
		
		
//		WRITE(fd, rang_server, STRLEN(rang_server));

		filebuf = &(Master_infor->rg_list);
		MEMSET(filebuf, SVR_IDX_FILE_SIZE);

		filebuf->freeoff = SVR_IDX_FILE_HDR;
		filebuf->nextrno = 0;
		filebuf->pad2[0] = 'r';
		filebuf->pad2[1] = 'g';
		filebuf->pad2[2] = 'l';
		filebuf->pad2[3] = 'i';
		filebuf->pad2[4] = 's';
		filebuf->pad2[5] = 't';
		
		filebuf->stat = 0;
	
		WRITE(fd, filebuf, SVR_IDX_FILE_SIZE);
	
		CLOSE(fd);		
	}
	else
	{
		
		MEMSET(rang_server, 256);
		MEMCPY(rang_server, MT_META_REGION, STRLEN(MT_META_REGION));
		str1_to_str2(rang_server, '/', "rangeserverlist");
	
		OPEN(fd, rang_server, (O_RDONLY));
		
		
//		WRITE(fd, rang_server, STRLEN(rang_server));

		MEMSET(&(Master_infor->rg_list), SVR_IDX_FILE_SIZE);

		READ(fd, &(Master_infor->rg_list), SVR_IDX_FILE_SIZE);

		
		CLOSE(fd);

		
		RANGE_PROF *rg_addr = (RANGE_PROF *)(Master_infor->rg_list.data);

		for(i = 0; i < Master_infor->rg_list.nextrno; i++)
		{
			/* Metaserver is crash, all the online ranger server will be recovery. */
			if (rg_addr[i].rg_stat & RANGER_IS_ONLINE)
			{
				rg_addr[i].rg_stat &= ~RANGER_IS_ONLINE;
				rg_addr[i].rg_stat |= (RANGER_IS_OFFLINE | RANGER_NEED_RECOVERY);
			}
		}
		
		
	}

	if (STAT(MT_META_INDEX, &st) != 0)
	{
		MKDIR(status, MT_META_INDEX, 0755); 
	}

	if (STAT(LOG_FILE_DIR, &st) != 0)
	{
		MKDIR(status, LOG_FILE_DIR, 0755); 
	}

	if (STAT(BACKUP_DIR, &st) != 0)
	{
		MKDIR(status, BACKUP_DIR, 0755); 
	}
		
	tab_sstabmap = NULL;
	sstab_map = NULL;
	
	ca_setup_pool();

	return;
}


static void
meta_save_rginfo()
{
	char    rang_server[256];
	int     fd;


	P_SPINLOCK(Master_infor->rglist_spinlock);
	
	MEMSET(rang_server, 256);
	MEMCPY(rang_server, MT_META_REGION, STRLEN(MT_META_REGION));
	str1_to_str2(rang_server, '/', "rangeserverlist");

	OPEN(fd, rang_server, (O_RDWR));

	WRITE(fd, &(Master_infor->rg_list), SVR_IDX_FILE_SIZE);

	CLOSE(fd);

	V_SPINLOCK(Master_infor->rglist_spinlock);
}


void
meta_add_server(TREE *command)
{
	char	rang_server[256];
	int		fd;
	SVR_IDX_FILE	*filebuf;
	
	MEMSET(rang_server, 256);
	MEMCPY(rang_server, MT_META_REGION, STRLEN(MT_META_REGION));
	str1_to_str2(rang_server, '/', "rangeserverlist");

	filebuf = (SVR_IDX_FILE *)MEMALLOCHEAP(SVR_IDX_FILE_SIZE);
	
	OPEN(fd, rang_server, (O_CREAT|O_WRONLY|O_TRUNC));

	READ(fd,filebuf,SVR_IDX_FILE_SIZE);
	

	PUT_TO_BUFFER(filebuf->data, filebuf->freeoff, 
				command->sym.command.tabname,
				command->sym.command.tabname_len);
	PUT_TO_BUFFER(filebuf->data, filebuf->freeoff, 
				command->left->right->sym.constant.value,
				command->left->right->sym.constant.len);

	WRITE(fd, filebuf, SVR_IDX_FILE_SIZE);

	CLOSE(fd);	

	MEMFREEHEAP(filebuf);

	return;
}


char *
meta_crtab(TREE *command)
{
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[256];
	char    	tab_dir1[256];	
	int		status;
	int		fd;
	TREE		*col_tree;
	char		*col_buf;	
	int		col_buf_idx;
	int		minlen;
	int		varcol;		
	int		colcnt;
	int		rtn_stat;
	TABLEHDR	*tab_hdr;
	int		tab_key_coloff;
	int		tab_key_colid;
	int		tab_key_coltype;
	COLINFO		col_info;
	char 		*resp;
	SVR_IDX_FILE	*tablet_store;
	SSTAB_INFOR	*sstab_map_tmp;


	Assert(command);
	
	rtn_stat = FALSE;
	resp = NULL;
	tablet_store = NULL;
	sstab_map_tmp = NULL;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;

	
	MEMSET(tab_dir, 256);
	MEMSET(tab_dir1, 256);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));
	str1_to_str2(tab_dir, '/', tab_name);

	if (STAT(tab_dir, &st) != 0)
	{
		MKDIR(status, tab_dir, 0755);        

		if (status < 0)
		{
			goto exit;
		}
	}
	else
	{
		traceprint("Table %s has been created.\n",tab_name);
		goto exit;
	}
	
	
	MEMCPY(tab_dir1, tab_dir, STRLEN(tab_dir));

	str1_to_str2(tab_dir1, '/', "syscolumns");

	OPEN(fd, tab_dir1, (O_CREAT|O_WRONLY|O_TRUNC));

	if (fd < 0)
	{
		goto exit;
	}

	
	//row_buf = MEMALLOCHEAP(4 * sizeof(int) + 64);
	col_buf_idx = 0;
	minlen = sizeof(ROWFMT);
	varcol = 0;
	colcnt = 0;
	col_tree = command->left;

	
	col_buf = MEMALLOCHEAP(COL_MAX_NUM * sizeof(COLINFO));
	while (col_tree)
	{
	        MEMSET(&col_info, sizeof(COLINFO));

		
		//row_buf_idx = 0;

		col_info.col_id = col_tree->sym.resdom.colid;
		col_info.col_len = col_tree->sym.resdom.colen;
		MEMCPY(col_info.col_name, col_tree->sym.resdom.colname,
		STRLEN(col_tree->sym.resdom.colname));          
				
		
		if (col_tree->sym.resdom.colen > 0)
		{
			
			if (col_tree->sym.resdom.colid == 1)
			{
				tab_key_coloff = minlen;
				tab_key_colid = 1;
				tab_key_coltype = col_tree->sym.resdom.coltype;
			}
			
			
                        col_info.col_offset = minlen;
			
			minlen += col_tree->sym.resdom.colen;			
		}
		else
		{
			
			if (col_tree->sym.resdom.colid == 1)
			{
				tab_key_coloff = -(varcol+1);
				tab_key_colid = 1;
				tab_key_coltype = col_tree->sym.resdom.coltype;
			}
			
			
			col_info.col_offset = -(varcol+1);

			varcol++;
		}

		colcnt ++;

		col_info.col_type = col_tree->sym.resdom.coltype;
		
		
		PUT_TO_BUFFER(col_buf, col_buf_idx, &col_info, sizeof(COLINFO));
		
		col_tree = col_tree->left;
	}
	
        
	
         
	
        
	WRITE(fd, col_buf, col_buf_idx);

	CLOSE(fd);
	
	MEMSET(tab_dir1, 256);

	
	MEMCPY(tab_dir1, tab_dir, STRLEN(tab_dir));

	str1_to_str2(tab_dir1, '/', "sysobjects");
	
	OPEN(fd, tab_dir1, (O_CREAT|O_WRONLY|O_TRUNC));

	if (fd < 0)
	{
		goto exit;
	}

	tab_hdr = MEMALLOCHEAP(sizeof(TABLEHDR));

	
	tab_hdr->tab_id = ++Master_infor->last_tabid;
	MEMCPY(tab_hdr->tab_name, tab_name, tab_name_len);
	tab_hdr->tab_tablet = 0;
	tab_hdr->tab_sstab = 0;
	tab_hdr->tab_row_minlen = minlen;
	tab_hdr->tab_key_coloff = tab_key_coloff;
	tab_hdr->tab_key_colid = tab_key_colid;
	tab_hdr->tab_key_coltype = tab_key_coltype;
	tab_hdr->tab_col = colcnt;
	tab_hdr->tab_varcol = varcol;
	tab_hdr->offset_c1 = 0;
	tab_hdr->offset_c2 = -1;
	
	
	WRITE(fd, tab_hdr, sizeof(TABLEHDR));

	CLOSE(fd);
	
	
	sstab_map_tmp = (SSTAB_INFOR *)malloc(SSTAB_MAP_SIZE);

	
	MEMSET(sstab_map_tmp, 1024 * 1024 * sizeof(int));
	
	MEMSET(tab_dir1, 256);

	
	MEMCPY(tab_dir1, tab_dir, STRLEN(tab_dir));

	str1_to_str2(tab_dir1, '/', "sstabmap");
	
	OPEN(fd, tab_dir1, (O_CREAT|O_WRONLY|O_TRUNC));

	if (fd < 0)
	{
		goto exit;
	}
	
	
	WRITE(fd, sstab_map_tmp, SSTAB_MAP_SIZE);

	CLOSE(fd);

	
	tablet_store = (SVR_IDX_FILE *)MEMALLOCHEAP(sizeof(SVR_IDX_FILE));

	
	MEMSET(tab_dir1, TABLE_NAME_MAX_LEN);

	
	MEMCPY(tab_dir1, tab_dir, STRLEN(tab_dir));

	/* TODO: placehoder. */
	str1_to_str2(tab_dir1, '/', "tabletinranger");
	
	OPEN(fd, tab_dir1, (O_CREAT|O_WRONLY|O_TRUNC));

	if (fd < 0)
	{
		goto exit;
	}
	
	WRITE(fd, tablet_store, sizeof(SVR_IDX_FILE));

	CLOSE(fd);

	MEMSET(tab_dir1, TABLE_NAME_MAX_LEN);

	
	MEMCPY(tab_dir1, tab_dir, STRLEN(tab_dir));

	/* TODO: placehoder. */
	str1_to_str2(tab_dir1, '/', "tabletinranger");
	
	OPEN(fd, tab_dir1, (O_CREAT|O_WRONLY|O_TRUNC));

	if (fd < 0)
	{
		goto exit;
	}
	
	WRITE(fd, tablet_store, sizeof(SVR_IDX_FILE));

	CLOSE(fd);

	MEMSET(tab_dir1, 256);
	MEMCPY(tab_dir1, MT_META_TABLE, STRLEN(MT_META_TABLE));
	str1_to_str2(tab_dir1, '/', "systable");

	OPEN(fd, tab_dir1, (O_RDWR));
	

	/* Saving the meta information to the in-memory structure. */
	Master_infor->meta_systab->last_tabid = Master_infor->last_tabid;

	MEMCPY(Master_infor->meta_systab->meta_tabdir[Master_infor->meta_systab->tabnum], tab_dir, STRLEN(tab_dir));
	
	(Master_infor->meta_systab)->tabnum++;

	/* Flush this creating information into the disk. */
	WRITE(fd, Master_infor->meta_systab, sizeof(META_SYSTABLE));

	CLOSE(fd);

	MEMCPY(&(Master_infor->meta_sysobj->sysobject[Master_infor->meta_systab->tabnum - 1]), tab_hdr,sizeof(TABLEHDR));

	Master_infor->meta_syscol->colnum[Master_infor->meta_systab->tabnum - 1] = tab_hdr->tab_col;
	MEMCPY(&(Master_infor->meta_syscol->colinfor[Master_infor->meta_systab->tabnum - 1]),col_buf, col_buf_idx);
	
	MEMFREEHEAP(tab_hdr);

	MEMFREEHEAP(col_buf);
	
	rtn_stat = TRUE;
	
exit:
	if (sstab_map_tmp != NULL)
	{
		free(sstab_map_tmp);
	}

	if (tablet_store != NULL)
	{
		MEMFREEHEAP(tablet_store);
	}

	if (rtn_stat)
	{
		resp = conn_build_resp_byte(RPC_SUCCESS, 0, NULL);
	}
	else
	{
		resp = conn_build_resp_byte(RPC_FAIL, 0, NULL);
	}
	
	return resp;
}


void
meta_ins_systab(char *systab, char *row)
{
	TABINFO		*tabinfo;
	int		minrowlen;
	char		*key;
	int		ign;
	BLK_ROWINFO	blk_rowinfo;
	
	
	tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
	MEMSET(tabinfo, sizeof(TABINFO));

	tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
	MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

	tabinfo->t_rowinfo = &blk_rowinfo;
	MEMSET(tabinfo->t_rowinfo, sizeof(BLK_ROWINFO));
	
	tabinfo_push(tabinfo);

	minrowlen = sizeof(ROWFMT) + 3 * sizeof(int);

	
	key = row_locate_col(row, (sizeof(ROWFMT) + sizeof(int)),
					minrowlen, &ign);
	
	TABINFO_INIT(tabinfo, systab, tabinfo->t_sinfo, minrowlen,
					TAB_META_SYSTAB, 0 ,0);
	SRCH_INFO_INIT(tabinfo->t_sinfo, key, 4, 1, INT4, 
					sizeof(ROWFMT) + sizeof(int));
			
	blkins(tabinfo, row);

	tabinfo_pop();
	MEMFREEHEAP(tabinfo->t_sinfo);
	MEMFREEHEAP(tabinfo);
}




char *
meta_instab(TREE *command, TABINFO *tabinfo)
{
	LOCALTSS(tss);
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	char		tab_tabletschm_dir[TABLE_NAME_MAX_LEN];
	int		fd1;
	int		rtn_stat;
	TABLEHDR	*tab_hdr;
	char		*keycol;
	int		keycolen;
	int		sstab_idx;
	char		sstab_name[SSTABLE_NAME_MAX_LEN];
	int		sstab_namelen;
	char   		*col_buf;
	int		col_buf_idx;
	int		col_buf_len;
	char		*resp;
	char		tablet_name[32];
	int		tablet_min_rlen;
	int		namelen;
	char		*name;
	char		*rp;
	char		*tabletschm_rp;
	int		status;
	char		*rg_addr;
	int		rg_port;
	int		sstab_id;
	int		res_sstab_id;
	RANGE_PROF	*rg_prof;
	int		sstabmap_chg;
	int		rpc_status;
	int		rg_suspect;
	int		tabidx;
	int		tabhdr_update;


	Assert(command);

	rtn_stat = FALSE;
	sstabmap_chg = FALSE;
	tabhdr_update = FALSE;
	sstab_idx = 0;
	col_buf = NULL;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;
	rpc_status = 0;
	rg_suspect = FALSE;	

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if ((tabidx = meta_table_is_exist(tab_dir)) == -1)
	{
		traceprint("Table %s is not exist!\n", tab_name);

		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}


	tab_hdr = &(Master_infor->meta_sysobj->sysobject[tabidx]);

	if (tab_hdr->tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");

		goto exit;
	}

	keycol = par_get_colval_by_colid(command, tab_hdr->tab_key_colid, 
					&keycolen);

	MEMSET(sstab_name, SSTABLE_NAME_MAX_LEN);

	sstab_map = sstab_map_get(tab_hdr->tab_id, tab_dir, &tab_sstabmap);

	Assert(sstab_map != NULL);

	if (sstab_map == NULL)
	{
		traceprint("Table %s has no sstabmap in the metaserver!", tab_name);
		ex_raise(EX_ANY);
	}

	if (tab_hdr->tab_tablet > 0)
	{
		
		MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_tabletschm_dir, tab_dir, STRLEN(tab_dir));
		str1_to_str2(tab_tabletschm_dir, '/', "tabletscheme");

		tabletschm_rp = tablet_schm_srch_row(tab_hdr, tab_hdr->tab_id, 
						     TABLETSCHM_ID, 
						     tab_tabletschm_dir,
						     keycol, keycolen);

		name = row_locate_col(tabletschm_rp, 
				      TABLETSCHM_TABLETNAME_COLOFF_INROW,
				      ROW_MINLEN_IN_TABLETSCHM, &namelen);
		
		
		MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
		str1_to_str2(tab_meta_dir, '/', name);

		int tabletid;

		tabletid = *(int *)row_locate_col(tabletschm_rp, 
						TABLETSCHM_TABLETID_COLOFF_INROW,
						ROW_MINLEN_IN_TABLETSCHM, 
						&namelen);

		
		int ign;
		rg_addr = row_locate_col(tabletschm_rp, 
					 TABLETSCHM_RGADDR_COLOFF_INROW,
					 ROW_MINLEN_IN_TABLETSCHM, &ign);

		rg_port = *(int *)row_locate_col(tabletschm_rp, 
						 TABLETSCHM_RGPORT_COLOFF_INROW,
					 	 ROW_MINLEN_IN_TABLETSCHM, &ign);
				
		traceprint("select ranger server %s/%d for insert\n", rg_addr, rg_port);		
		tss->tcur_rgprof = rebalan_get_rg_prof_by_addr(rg_addr, rg_port);

		Assert(tss->tcur_rgprof);

		if (tss->tcur_rgprof == NULL)
		{
			traceprint("Can't get the profile of ranger server %s\n", rg_addr);

			goto exit;
		}

		if (tss->tcur_rgprof->rg_stat & RANGER_IS_OFFLINE)
		{
			traceprint("Ranger server (%s:%d) is OFF-LINE\n", rg_addr, rg_port);
			goto exit;
		}
		else if (tss->tcur_rgprof->rg_stat & RANGER_IS_SUSPECT)
		{
			traceprint("Ranger server (%s:%d) is SUSPECT\n", rg_addr, rg_port);

			rg_suspect = TRUE;
			goto exit;
		}
		else if (tss->tcur_rgprof->rg_stat & RANGER_NEED_RECOVERY)
		{
			traceprint("Ranger server (%s:%d) is being in the recovery\n", rg_addr, rg_port);

			rg_suspect = TRUE;
			goto exit;
		}
		else if (tss->tcur_rgprof->rg_stat & RANGER_RESTART)
		{
			traceprint("Ranger server (%s:%d) is booting.\n", rg_addr, rg_port);

			rg_suspect = TRUE;
			goto exit;
		}

		//rg_port = tss->tcur_rgprof->rg_port;

		
		tabinfo->t_stat &= ~TAB_TABLET_KEYROW_CHG;
		
		rp = tablet_srch_row(tabinfo, tab_hdr, tab_hdr->tab_id, tabletid, 
				     tab_meta_dir, keycol, keycolen);

		
		name = row_locate_col(rp, TABLET_SSTABNAME_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLET, &sstab_namelen);

			
		MEMCPY(sstab_name, name, STRLEN(name));

		sstab_id = *(int *)row_locate_col(rp, TABLET_SSTABID_COLOFF_INROW, 
						  ROW_MINLEN_IN_TABLET, &namelen);

		char *testcol;
		testcol = row_locate_col(rp, TABLET_RESSSTABID_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLET, &namelen);
		
		res_sstab_id = *(int *)testcol;

		if(   (!SSTAB_MAP_RESERV(res_sstab_id)) 
		   || (tabinfo->t_stat & TAB_TABLET_KEYROW_CHG))
		{
			int rlen, rlen_c5, sstab_res;

			sstab_res = FALSE;
			if (!SSTAB_MAP_RESERV(res_sstab_id))
			{
				Assert(SSTAB_MAP_USED(res_sstab_id));

				if (!SSTAB_MAP_USED(res_sstab_id))
				{
					traceprint("SSTable map hit error!\n");

					goto exit;
				}

				res_sstab_id = meta_get_free_sstab();

				SSTAB_MAP_SET(res_sstab_id, SSTAB_RESERVED);

				sstab_res= TRUE;
				sstabmap_chg = TRUE;
			}

			if (tabinfo->t_stat & TAB_TABLET_KEYROW_CHG)
			{
				
				rlen_c5 = ROW_MINLEN_IN_TABLET + sizeof(int) 
						+ keycolen + sizeof(int);
			}

			
			rlen = (tabinfo->t_stat & TAB_TABLET_KEYROW_CHG) ? 
					rlen_c5 : ROW_GET_LENGTH(rp, 
							ROW_MINLEN_IN_TABLET);
			
			char	*newrp = (char *)MEMALLOCHEAP(rlen);

			if (sstab_res)
			{
				tablet_upd_col(newrp, rp, ROW_GET_LENGTH(rp, 
						ROW_MINLEN_IN_TABLET), 
						TABLET_RESSSTABID_COLID_INROW, 
						(char *)(&res_sstab_id), 
						sizeof(int));
			}

			if (tabinfo->t_stat & TAB_TABLET_KEYROW_CHG)
			{
				tablet_upd_col(newrp, rp, ROW_GET_LENGTH(rp, 
							ROW_MINLEN_IN_TABLET), 
					       TABLET_KEY_COLID_INROW, 
					       keycol, keycolen);								
			}
			
			tablet_del_row(tab_hdr, tab_hdr->tab_id, tabletid,
					tab_meta_dir, rp, ROW_MINLEN_IN_TABLET);

			tablet_ins_row(tab_hdr, tab_hdr->tab_id, tabletid, 
					tab_meta_dir, newrp, 
					ROW_MINLEN_IN_TABLET);

			MEMFREEHEAP(newrp);

			if (tabinfo->t_stat & TAB_TABLET_KEYROW_CHG)
			{
				char	*tabletschm_newrp;
				int	rlen_c3;

				rlen_c3 = ROW_MINLEN_IN_TABLETSCHM + sizeof(int) 
							+ keycolen + sizeof(int);
				tabletschm_newrp = (char *)MEMALLOCHEAP(rlen_c3);

				
				tablet_schm_upd_col(tabletschm_newrp, tabletschm_rp,
							TABLETSCHM_KEY_COLID_INROW, 
						    	keycol, keycolen);
				
				tablet_schm_del_row(tab_hdr->tab_id, TABLETSCHM_ID,
							tab_tabletschm_dir,
							tabletschm_rp);

				tablet_schm_ins_row(tab_hdr->tab_id, TABLETSCHM_ID, 
							tab_tabletschm_dir, 
							tabletschm_newrp,
							tab_hdr->tab_tablet);

				tabinfo->t_stat &= ~TAB_TABLET_KEYROW_CHG;
				
				MEMFREEHEAP(tabletschm_newrp);
			}
			
		}

	}
	else if (tab_hdr->tab_tablet == 0)
	{
		MEMCPY(sstab_name, tab_name, tab_name_len);
		build_file_name("tablet", tablet_name, tab_hdr->tab_tablet);
		MEMCPY((sstab_name + tab_name_len), tablet_name, 
			STRLEN(tablet_name));
		build_file_name("sstable", sstab_name + tab_name_len + STRLEN(tablet_name), 
				tab_hdr->tab_sstab);
	}
	else
	{
		Assert(0);

		ex_raise(EX_ANY);
	}
	
	
	if (tab_hdr->tab_tablet == 0)
	{
		char	*sstab_rp;
		int	sstab_rlen;

		
		sstab_rlen = ROW_MINLEN_IN_TABLET + keycolen + 4 + 4;

		sstab_rp = MEMALLOCHEAP(sstab_rlen);

		sstab_id = meta_get_free_sstab();
		SSTAB_MAP_SET(sstab_id, SSTAB_USED);

		res_sstab_id = meta_get_free_sstab();
		SSTAB_MAP_SET(res_sstab_id, SSTAB_RESERVED);
		sstabmap_chg = TRUE;

		if(Master_infor->rg_list.nextrno > 0)
		{
			
			//rg_prof = (RANGE_PROF *)(Master_infor->rg_list.data);
			rg_prof = meta_get_rg();

			if (!rg_prof)
			{
				traceprint("Ranger server is un-available for insert\n");

				goto exit;
			}

			Assert(rg_prof->rg_stat & RANGER_IS_ONLINE);
		}
		else
		{
			Assert(0);

			ex_raise(EX_ANY);
		}
		
		
		tablet_min_rlen = tablet_bld_row(sstab_rp, sstab_rlen, tab_name,
						tab_name_len, sstab_id, 
						res_sstab_id, sstab_name,
						STRLEN(sstab_name), keycol,
						keycolen, tab_hdr->tab_key_coltype);
	
		rg_addr = rg_prof->rg_addr;
		rg_port = rg_prof->rg_port;
		
		tablet_crt(tab_hdr, tab_dir, rg_addr, sstab_rp, 
				tablet_min_rlen, rg_port);
		
		(tab_hdr->tab_tablet)++;
		(tab_hdr->tab_sstab)++;

		tabhdr_update = TRUE;

		(rg_prof->rg_tablet_num)++;

		meta_save_rginfo();

		MEMFREEHEAP(sstab_rp);
	}

	if (tabhdr_update)
	{
		MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

		str1_to_str2(tab_meta_dir, '/', "sysobjects");


		OPEN(fd1, tab_meta_dir, (O_RDWR));
		
		if (fd1 < 0)
		{
			goto exit;
		}

		
		status = WRITE(fd1, tab_hdr, sizeof(TABLEHDR));
		
		Assert(status == sizeof(TABLEHDR));

		if (status != sizeof(TABLEHDR))
		{
			traceprint("Table %s sysobjects hit error!\n", tab_name);
			CLOSE(fd1);
			ex_raise(EX_ANY);
		}
		
		CLOSE(fd1);	
	}
	

	
	col_buf_len = sizeof(INSMETA) + sizeof(TABLEHDR) 
				+ tab_hdr->tab_col * (sizeof(COLINFO));
	col_buf = MEMALLOCHEAP(col_buf_len);
	MEMSET(col_buf, col_buf_len);


	
	col_buf_idx = 0;
		
	MEMCPY((col_buf + col_buf_idx), rg_addr, STRLEN(rg_addr));
	col_buf_idx += RANGE_ADDR_MAX_LEN;

	*(int *)(col_buf + col_buf_idx) = rg_port;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = RANGER_IS_ONLINE;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = 0;
	col_buf_idx += sizeof(int);

	col_buf_idx += sizeof(int) + sizeof(pthread_t);

	
	*(int *)(col_buf + col_buf_idx) = sstab_id;
	col_buf_idx += sizeof(int);

	
	*(int *)(col_buf + col_buf_idx) = res_sstab_id;
	col_buf_idx += sizeof(int);

	
	*(unsigned int *)(col_buf + col_buf_idx) = SSTAB_MAP_GET_SPLIT_TS(sstab_id);
	col_buf_idx += sizeof(int);
	
	
	MEMCPY((col_buf + col_buf_idx), sstab_name, STRLEN(sstab_name));
	col_buf_idx += SSTABLE_NAME_MAX_LEN;

	
	col_buf_idx += sizeof(int);

	
        *(int *)(col_buf + col_buf_idx) = tab_hdr->tab_col;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = tab_hdr->tab_varcol;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = tab_hdr->tab_row_minlen;
	col_buf_idx += sizeof(int);

	col_buf_idx += sizeof(int);

	
	MEMCPY((col_buf + col_buf_idx), tab_hdr, sizeof(TABLEHDR));
	col_buf_idx += sizeof(TABLEHDR);
	
	
	Assert(Master_infor->meta_syscol->colnum[tabidx] == tab_hdr->tab_col);

	MEMCPY((col_buf + col_buf_idx), &(Master_infor->meta_syscol->colinfor[tabidx]),tab_hdr->tab_col * sizeof(COLINFO));

	col_buf_idx += tab_hdr->tab_col * sizeof(COLINFO);
	
	if (sstabmap_chg)
	{
		sstab_map_put(-1, tss->ttab_sstabmap);
	}
	rtn_stat = TRUE;

exit:

	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;		
		resp = conn_build_resp_byte(rpc_status, col_buf_idx, col_buf);
	}
	else
	{
		if (rg_suspect)
		{
			rpc_status |= RPC_RETRY;
		}
		else
		{
			rpc_status |= RPC_FAIL;
		}
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	if (col_buf != NULL)
	{
		MEMFREEHEAP(col_buf);
	}

	return resp;
}


char *
meta_droptab(TREE *command)
{
	
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	int		fd1;
	int		rtn_stat;
	TABLEHDR	tab_hdr;
	char		*resp;
	int		status;
	char   		*col_buf;
	int		col_buf_idx;
	int		col_buf_len;
	RANGE_PROF	*rg_prof;
	char		*rg_addr;
	int		rg_port;
	int		rpc_status;


	Assert(command);

	rtn_stat = FALSE;
	col_buf = NULL;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;
	rpc_status = 0;

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if (STAT(tab_dir, &st) != 0)
	{
		traceprint("Table %s is not exist!\n", tab_name);
		
		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}
	
	str1_to_str2(tab_meta_dir, '/', "sysobjects");


	OPEN(fd1, tab_meta_dir, (O_RDWR));
	
	if (fd1 < 0)
	{
		goto exit;
	}

	status = READ(fd1, &tab_hdr, sizeof(TABLEHDR));	

	Assert(status == sizeof(TABLEHDR));
	
	if (status != sizeof(TABLEHDR))
	{
		traceprint("Table %s sysobjects hit error!\n", tab_name);
		CLOSE(fd1);
		ex_raise(EX_ANY);
	}

	if (tab_hdr.tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");
		CLOSE(fd1);
		goto exit;
	}
		
	tab_hdr.tab_stat |= TAB_DROPPED;

#ifdef MT_KFS_BACKEND

	CLOSE(fd1);

	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if (STAT(tab_dir, &st) != 0)
	{
		traceprint("Table %s is not exist!\n", tab_name);
		goto exit;
	}
	
	str1_to_str2(tab_meta_dir, '/', "sysobjects");


	OPEN(fd1, tab_meta_dir, (O_RDWR));
	
	if (fd1 < 0)
	{
		goto exit;
	}
#else
	LSEEK(fd1, 0, SEEK_SET);
#endif	
	
	status = WRITE(fd1, &tab_hdr, sizeof(TABLEHDR));

	Assert(status == sizeof(TABLEHDR));
	
	if (status != sizeof(TABLEHDR))
	{
		traceprint("Table %s sysobjects hit error!\n", tab_name);
		CLOSE(fd1);
		ex_raise(EX_ANY);
	}
	
	CLOSE(fd1);

	if(Master_infor->rg_list.nextrno > 0)
	{
		
		rg_prof = meta_get_rg();

		if (!rg_prof)
		{
			traceprint("Ranger server is un-available for insert\n");
			CLOSE(fd1);
			goto exit;
		}

		Assert(rg_prof->rg_stat & RANGER_IS_ONLINE);
			
		if (!(rg_prof->rg_stat & RANGER_IS_ONLINE))
		{
			traceprint("Ranger server %s is off-line\n", rg_prof->rg_addr);
			goto exit;
		}
	}
	else
	{
		Assert(0);

		traceprint("No ranger server is avlable\n");
		ex_raise(EX_ANY);
	}

	rg_addr = rg_prof->rg_addr;
	rg_port = rg_prof->rg_port;

	
	col_buf_len = RANGE_ADDR_MAX_LEN + sizeof(int);
	col_buf = MEMALLOCHEAP(col_buf_len);
	MEMSET(col_buf, col_buf_len);

	col_buf_idx = 0;
		
	MEMCPY((col_buf + col_buf_idx), rg_addr, STRLEN(rg_addr));
	col_buf_idx += RANGE_ADDR_MAX_LEN;

	*(int *)(col_buf + col_buf_idx) = rg_port;
	col_buf_idx += sizeof(int);

	
	rtn_stat = TRUE;

exit:

	
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, col_buf_idx, col_buf);
	}
	else
	{
		rpc_status |= RPC_FAIL;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	if (col_buf != NULL)
	{
		MEMFREEHEAP(col_buf);
	}

	return resp;
}


char *
meta_removtab(TREE *command)
{
	
	char	*tab_name;
	int	tab_name_len;
	char	tab_dir[TABLE_NAME_MAX_LEN];
	char	tab_meta_dir[TABLE_NAME_MAX_LEN];
	int	fd1;
	int	rtn_stat;
	TABLEHDR	tab_hdr;
	char	*resp;
	int	status;
	int	rpc_status;


	Assert(command);

	rtn_stat = FALSE;
	rpc_status = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if (STAT(tab_dir, &st) != 0)
	{
		traceprint("Table %s is not exist!\n", tab_name);
		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}
	
	str1_to_str2(tab_meta_dir, '/', "sysobjects");


	OPEN(fd1, tab_meta_dir, (O_RDONLY));
	
	if (fd1 < 0)
	{
		goto exit;
	}

	status = READ(fd1, &tab_hdr, sizeof(TABLEHDR));	

	Assert(status == sizeof(TABLEHDR));
	
	Assert(tab_hdr.tab_stat & TAB_DROPPED);

	
	if (status != sizeof(TABLEHDR))
	{
		traceprint("Table %s sysobjects hit error!\n", tab_name);
		CLOSE(fd1);
		ex_raise(EX_ANY);
	}
	
	CLOSE(fd1);

#ifdef MT_KFS_BACKEND

	RMDIR(status, tab_dir);
	if(!status)
#else

	char	cmd_str[TABLE_NAME_MAX_LEN];
	MEMSET(cmd_str, TABLE_NAME_MAX_LEN);
	
	sprintf(cmd_str, "rm -rf %s", tab_dir);
	
	if (!system(cmd_str))
#endif
	{
		rtn_stat = TRUE;
	}

exit:	
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}
	else
	{
		rpc_status |= RPC_FAIL;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	return resp;
}


char *
meta_seldeltab(TREE *command, TABINFO *tabinfo)
{
	LOCALTSS(tss);
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	char   		*col_buf;
	int		col_buf_idx;
	int		col_buf_len;
	int		rtn_stat;
	TABLEHDR	*tab_hdr;
	char		*keycol;
	int		keycolen;
	int		sstab_rlen;
	int		sstab_idx;
	char		*resp;
	char		*rp;
	char		*name;
	int		namelen;
	char		sstab_name[SSTABLE_NAME_MAX_LEN];
	int		sstab_id;
	int		res_sstab_id;
	char		*rg_addr;
	int		rg_port;
	int		rpc_status;
	int		rg_suspect;
	int		tabidx;
	

	Assert(command);

	rtn_stat = FALSE;
	col_buf= NULL;
	sstab_rlen = 0;
	sstab_idx = 0;
	rpc_status = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;
	rg_suspect = FALSE; 

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if ((tabidx = meta_table_is_exist(tab_dir)) == -1)
	{
		traceprint("Table %s is not exist.\n", tab_name);
		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}
	
	str1_to_str2(tab_meta_dir, '/', "sysobjects");

	tab_hdr = &(Master_infor->meta_sysobj->sysobject[tabidx]);

	if (tab_hdr->tab_tablet == 0)
	{
		traceprint("Table %s has no data.\n", tab_name);
		goto exit;
	}

	if (tab_hdr->tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");
		goto exit;
	}

	sstab_map = sstab_map_get(tab_hdr->tab_id, tab_dir, &tab_sstabmap);

	Assert(sstab_map != NULL);

	if (sstab_map == NULL)
	{
		traceprint("Table %s has no sstabmap in the metaserver!", tab_name);
		ex_raise(EX_ANY);
	}
	
	keycol = par_get_colval_by_colid(command, tab_hdr->tab_key_colid, 
						&keycolen);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
	str1_to_str2(tab_meta_dir, '/', "tabletscheme");

	rp = tablet_schm_srch_row(tab_hdr, tab_hdr->tab_id, TABLETSCHM_ID,
				tab_meta_dir, keycol, keycolen);

	name = row_locate_col(rp, TABLETSCHM_TABLETNAME_COLOFF_INROW, 
				ROW_MINLEN_IN_TABLETSCHM, &namelen);

	
	int ign;
	rg_addr = row_locate_col(rp, TABLETSCHM_RGADDR_COLOFF_INROW,
				ROW_MINLEN_IN_TABLETSCHM, &ign);
	rg_port = *(int *)row_locate_col(rp, TABLETSCHM_RGPORT_COLOFF_INROW, 
					 ROW_MINLEN_IN_TABLETSCHM, &ign);
	
	tss->tcur_rgprof = rebalan_get_rg_prof_by_addr(rg_addr, rg_port);

	Assert(tss->tcur_rgprof);

	
	if (tss->tcur_rgprof == NULL)
	{
		traceprint("Can't get the profile of ranger server %s\n", rg_addr);

		goto exit;
	}

	if (tss->tcur_rgprof->rg_stat & RANGER_IS_OFFLINE)
	{
		traceprint("Ranger server (%s:%d) is OFF-LINE\n", rg_addr, rg_port);

		goto exit;
	}
	else if (tss->tcur_rgprof->rg_stat & RANGER_IS_SUSPECT)
	{
		traceprint("Ranger server (%s:%d) is SUSPECT\n", rg_addr, rg_port);

		rg_suspect = TRUE;	
		goto exit;
	}
	else if (tss->tcur_rgprof->rg_stat & RANGER_NEED_RECOVERY)
	{
		traceprint("Ranger server (%s:%d) is being in the recovery\n", rg_addr, rg_port);

		rg_suspect = TRUE;
		goto exit;
	}
	else if (tss->tcur_rgprof->rg_stat & RANGER_RESTART)
	{
		traceprint("Ranger server (%s:%d) is booting.\n", rg_addr, rg_port);

		rg_suspect = TRUE;
		goto exit;
	}

	//rg_port = tss->tcur_rgprof->rg_port;
	
	
	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
	str1_to_str2(tab_meta_dir, '/', name);

	int tabletid;

	tabletid = *(int *)row_locate_col(rp, TABLETSCHM_TABLETID_COLOFF_INROW, 
					  ROW_MINLEN_IN_TABLETSCHM, &namelen);

	rp = tablet_srch_row(tabinfo, tab_hdr, tab_hdr->tab_id, tabletid, 
				tab_meta_dir, keycol, keycolen);

	
	name = row_locate_col(rp, TABLET_SSTABNAME_COLOFF_INROW,
				ROW_MINLEN_IN_TABLET, &namelen);

	MEMSET(sstab_name, SSTABLE_NAME_MAX_LEN);
	
			
	MEMCPY(sstab_name, name, STRLEN(name));

	
	sstab_id = *(int *)row_locate_col(rp,TABLET_SSTABID_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLET, &namelen);

	res_sstab_id = *(int *)row_locate_col(rp, TABLET_RESSSTABID_COLOFF_INROW, 
					      ROW_MINLEN_IN_TABLET, &namelen);

	

	
	if (tabinfo->t_sinfo->sistate & SI_NODATA)
	{
		goto exit;
	}
		
	col_buf_len = sizeof(INSMETA) + sizeof(TABLEHDR) 
				+ tab_hdr->tab_col * (sizeof(COLINFO));
	col_buf = MEMALLOCHEAP(col_buf_len);
	MEMSET(col_buf, col_buf_len);


	
	col_buf_idx = 0;
		
	MEMCPY((col_buf + col_buf_idx), rg_addr, STRLEN(rg_addr));
	col_buf_idx += RANGE_ADDR_MAX_LEN;

	*(int *)(col_buf + col_buf_idx) = rg_port;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = RANGER_IS_ONLINE;
	col_buf_idx += sizeof(int);
	
	*(int *)(col_buf + col_buf_idx) = 0;
	col_buf_idx += sizeof(int);

	col_buf_idx += sizeof(int) + sizeof(pthread_t);

	
	*(int *)(col_buf + col_buf_idx) = sstab_id;
	col_buf_idx += sizeof(int);

	
	*(int *)(col_buf + col_buf_idx) = res_sstab_id;
	col_buf_idx += sizeof(int);

	
	*(unsigned int *)(col_buf + col_buf_idx) = SSTAB_MAP_GET_SPLIT_TS(sstab_id);
	col_buf_idx += sizeof(int);

	
	MEMCPY((col_buf + col_buf_idx), sstab_name, SSTABLE_NAME_MAX_LEN);
	col_buf_idx += SSTABLE_NAME_MAX_LEN;

	
	col_buf_idx += sizeof(int);

	
        *(int *)(col_buf + col_buf_idx) = tab_hdr->tab_col;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = tab_hdr->tab_varcol;
	col_buf_idx += sizeof(int);

	*(int *)(col_buf + col_buf_idx) = tab_hdr->tab_row_minlen;
	col_buf_idx += sizeof(int);

	col_buf_idx += sizeof(int);

	
	MEMCPY((col_buf + col_buf_idx), tab_hdr, sizeof(TABLEHDR));
	col_buf_idx += sizeof(TABLEHDR);
	
	Assert(Master_infor->meta_syscol->colnum[tabidx] == tab_hdr->tab_col);
	
	MEMCPY((col_buf + col_buf_idx), &(Master_infor->meta_syscol->colinfor[tabidx]),tab_hdr->tab_col * sizeof(COLINFO));
	
	col_buf_idx += tab_hdr->tab_col * sizeof(COLINFO);
	rtn_stat = TRUE;

exit:
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, col_buf_idx, col_buf);
	}
	else
	{
		if (rg_suspect)
		{
			rpc_status |= RPC_RETRY;
		}
		else
		{
			rpc_status |= RPC_FAIL;
		}
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	if (col_buf != NULL)
	{
		MEMFREEHEAP(col_buf);
	}

	return resp;
}




char *
meta_selrangetab(TREE *command, TABINFO *tabinfo)
{
	LOCALTSS(tss);
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	char   		*col_buf;
	int		col_buf_idx;
	int		col_buf_len;
	int		rtn_stat;
	TABLEHDR	*tab_hdr;
	int		sstab_rlen;
	int		sstab_idx;
	char		*resp;
	char		*rp;
	char		*name;
	int		namelen;
	char		sstab_name[SSTABLE_NAME_MAX_LEN];
	int		sstab_id;
	int		res_sstab_id;
	char		*rg_addr;
	int		rg_port;
	int		key_is_expand;
	int		rpc_status;
	int		rg_suspect;
	int		tabidx;


	Assert(command);

	rtn_stat = FALSE;
	col_buf= NULL;
	sstab_rlen = 0;
	sstab_idx = 0;
	rpc_status = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;
	rg_suspect = FALSE;

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if ((tabidx = meta_table_is_exist(tab_dir)) == -1)
	{
		traceprint("Table %s is not exist.\n", tab_name);
		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}
	
	tab_hdr = &(Master_infor->meta_sysobj->sysobject[tabidx]);

	if (tab_hdr->tab_tablet == 0)
	{
		traceprint("Table %s has no data.\n", tab_name);
		goto exit;
	}

	if (tab_hdr->tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");
		goto exit;
	}

	sstab_map = sstab_map_get(tab_hdr->tab_id, tab_dir, &tab_sstabmap);

	Assert(sstab_map != NULL);

	if (sstab_map == NULL)
	{
		traceprint("Table %s has no sstabmap in the metaserver!", tab_name);
		ex_raise(EX_ANY);
	}

	char	*range_leftkey;
	int	leftkeylen;
	char	*range_rightkey;
	int	rightkeylen;
	
	range_leftkey = par_get_colval_by_colid(command, 1, &leftkeylen);
	range_rightkey = par_get_colval_by_colid(command, 2, &rightkeylen);
	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
	str1_to_str2(tab_meta_dir, '/', "tabletscheme");


	char	*keycol;
	int	keycolen;
	int	k;

	col_buf_len = sizeof(SELRANGE) + sizeof(TABLEHDR) + 
					tab_hdr->tab_col * (sizeof(COLINFO));
	col_buf = MEMALLOCHEAP(col_buf_len);
	MEMSET(col_buf, col_buf_len);
	col_buf_idx = 0;

	
	for (k = 0; k < 2; k++)
	{
		key_is_expand = FALSE;
		
		keycol = (k == 0) ? range_leftkey : range_rightkey;
		keycolen = (k == 0) ? leftkeylen : rightkeylen;

		if ((keycolen == 1) && (!strncasecmp("*", keycol, keycolen)))
		{
			key_is_expand = TRUE;
			
			if (k == 0)
			{
				rp = tablet_schm_get_1st_or_last_row(tab_hdr, 
						tab_hdr->tab_id, TABLETSCHM_ID,
						tab_meta_dir, TRUE);
			}
			else
			{
				rp = tablet_schm_get_1st_or_last_row(tab_hdr, 
						tab_hdr->tab_id, TABLETSCHM_ID,
						tab_meta_dir, FALSE);
			}
		}
		else
		{
			rp = tablet_schm_srch_row(tab_hdr, tab_hdr->tab_id,
						TABLETSCHM_ID, tab_meta_dir, 
						keycol, keycolen);
		}
		
		name = row_locate_col(rp, TABLETSCHM_TABLETNAME_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLETSCHM, &namelen);

		
		int ign;
		rg_addr = row_locate_col(rp, TABLETSCHM_RGADDR_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLETSCHM, &ign);

		rg_port = *(int *)row_locate_col(rp, 
					TABLETSCHM_RGPORT_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLETSCHM, &ign);
		
		tss->tcur_rgprof = rebalan_get_rg_prof_by_addr(rg_addr, rg_port);

		Assert(tss->tcur_rgprof);

		
		if (tss->tcur_rgprof == NULL)
		{
			traceprint("Can't get the profile of ranger server %s\n", rg_addr);

			goto exit;
		}

		if (tss->tcur_rgprof->rg_stat & RANGER_IS_OFFLINE)
		{
			traceprint("Ranger server (%s:%d) is OFF-LINE\n", rg_addr, rg_port);

			goto exit;
		}
		else if (tss->tcur_rgprof->rg_stat & RANGER_IS_SUSPECT)
		{
			traceprint("Ranger server (%s:%d) is SUSPECT\n", rg_addr, rg_port);
			rg_suspect = TRUE;
			goto exit;
		}
		else if (tss->tcur_rgprof->rg_stat & RANGER_NEED_RECOVERY)
		{
			traceprint("Ranger server (%s:%d) is being in the recovery\n", rg_addr, rg_port);

			rg_suspect = TRUE;
			goto exit;
		}
		else if (tss->tcur_rgprof->rg_stat & RANGER_RESTART)
		{
			traceprint("Ranger server (%s:%d) is booting.\n", rg_addr, rg_port);

			rg_suspect = TRUE;
			goto exit;
		}
		
		MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
		str1_to_str2(tab_meta_dir, '/', name);

		int tabletid;

		tabletid = *(int *)row_locate_col(rp, 
					TABLETSCHM_TABLETID_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLETSCHM, &namelen);

		if (key_is_expand)
		{
			if (k == 0)
			{
				rp = tablet_get_1st_or_last_row(tab_hdr,
								tab_hdr->tab_id,
								tabletid, 
								tab_meta_dir,
								TRUE);
			}
			else
			{
				rp = tablet_get_1st_or_last_row(tab_hdr, 
								tab_hdr->tab_id, 
								tabletid, 
								tab_meta_dir, 
								FALSE);
			}
		}
		else
		{
			rp = tablet_srch_row(tabinfo, tab_hdr, tab_hdr->tab_id, 
						tabletid, tab_meta_dir, keycol, 
						keycolen);
		}
		
		name = row_locate_col(rp, TABLET_SSTABNAME_COLOFF_INROW, 
						ROW_MINLEN_IN_TABLET, &namelen);

		MEMSET(sstab_name, SSTABLE_NAME_MAX_LEN);
				
		MEMCPY(sstab_name, name, STRLEN(name));

		
		sstab_id = *(int *)row_locate_col(rp,TABLET_SSTABID_COLOFF_INROW,
						ROW_MINLEN_IN_TABLET, &namelen);

		res_sstab_id = *(int *)row_locate_col(rp, 
						TABLET_RESSSTABID_COLOFF_INROW, 
						ROW_MINLEN_IN_TABLET, &namelen);

		
		if (tabinfo->t_sinfo->sistate & SI_NODATA)
		{
			goto exit;
		}

		
		MEMCPY((col_buf + col_buf_idx), rg_addr, STRLEN(rg_addr));
		col_buf_idx += RANGE_ADDR_MAX_LEN;

		*(int *)(col_buf + col_buf_idx) = rg_port;
		col_buf_idx += sizeof(int);

		*(int *)(col_buf + col_buf_idx) = RANGER_IS_ONLINE;
		col_buf_idx += sizeof(int);
		
		*(int *)(col_buf + col_buf_idx) = 0;
		col_buf_idx += sizeof(int);

		col_buf_idx += sizeof(int) + sizeof(pthread_t);
		
		*(int *)(col_buf + col_buf_idx) = sstab_id;
		col_buf_idx += sizeof(int);

		
		*(int *)(col_buf + col_buf_idx) = res_sstab_id;
		col_buf_idx += sizeof(int);

		
		*(unsigned int *)(col_buf + col_buf_idx) = SSTAB_MAP_GET_SPLIT_TS(sstab_id);
		col_buf_idx += sizeof(int);

		
		MEMCPY((col_buf + col_buf_idx), sstab_name, SSTABLE_NAME_MAX_LEN);
		col_buf_idx += SSTABLE_NAME_MAX_LEN;

		
		col_buf_idx += sizeof(int);

		
	        *(int *)(col_buf + col_buf_idx) = tab_hdr->tab_col;
		col_buf_idx += sizeof(int);

		*(int *)(col_buf + col_buf_idx) = tab_hdr->tab_varcol;
		col_buf_idx += sizeof(int);

		*(int *)(col_buf + col_buf_idx) = tab_hdr->tab_row_minlen;
		col_buf_idx += sizeof(int);

		col_buf_idx += sizeof(int);

	}
	
	MEMCPY((col_buf + col_buf_idx), tab_hdr, sizeof(TABLEHDR));
	col_buf_idx += sizeof(TABLEHDR);
	
	Assert(Master_infor->meta_syscol->colnum[tabidx] == tab_hdr->tab_col);

	MEMCPY((col_buf + col_buf_idx), &(Master_infor->meta_syscol->colinfor[tabidx]),tab_hdr->tab_col * sizeof(COLINFO));

	col_buf_idx += tab_hdr->tab_col * sizeof(COLINFO);
	rtn_stat = TRUE;

exit:
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, col_buf_idx, col_buf);
	}
	else
	{
		if (rg_suspect)
		{
			rpc_status |= RPC_RETRY;
		}
		else
		{
			rpc_status |= RPC_FAIL;
		}
		
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	if (col_buf != NULL)
	{
		MEMFREEHEAP(col_buf);
	}

	return resp;
}



char *
meta_selwheretab(TREE *command, TABINFO *tabinfo)
{
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	char   		*col_buf;
	int		col_buf_idx;
	int		col_buf_len;
	int		rtn_stat;
	TABLEHDR	*tab_hdr;
	int		sstab_rlen;
	int		sstab_idx;
	char		*resp;
	char		*rp;
	char		*name;
	int		namelen;
	int		key_is_expand;
	SELWHERE	selwhere;
	int		rpc_status;
	int		tabidx;


	Assert(command);

	rtn_stat = FALSE;
	col_buf= NULL;
	sstab_rlen = 0;
	sstab_idx = 0;
	rpc_status = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if ((tabidx = meta_table_is_exist(tab_dir)) == -1)
	{
		traceprint("Table %s is not exist.\n", tab_name);
		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}
	
	tab_hdr = &(Master_infor->meta_sysobj->sysobject[tabidx]);

	if (tab_hdr->tab_tablet == 0)
	{
		traceprint("Table %s has no data.\n", tab_name);
		goto exit;
	}

	if (tab_hdr->tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");
		goto exit;
	}


	Assert(Master_infor->meta_syscol->colnum[tabidx] == tab_hdr->tab_col);
	
	col_buf = (char *)(&(Master_infor->meta_syscol->colinfor[tabidx]));

	int i;

	for (i = 0; i < tab_hdr->tab_col; i++)
	{
		
		if (((COLINFO *)col_buf)[i].col_id == 1)
		{
			break;
		}
	}	

	MEMSET(&selwhere, sizeof(SELWHERE));
	
	CONSTANT *cons = par_get_constant_by_colname(command, 
					((COLINFO *)col_buf)[i].col_name);

	char	*range_leftkey;
	int	leftkeylen;
	char	*range_rightkey;
	int	rightkeylen;
	
	char	*keycol;
	int	keycolen;
	int	k;

	if (cons == NULL)
	{
		range_leftkey = "*\0";
		range_rightkey = "*\0";
		leftkeylen = 1;
		rightkeylen= 1;
	}
	else
	{
		range_leftkey = cons->value;
		range_rightkey = cons->rightval;
		leftkeylen = cons->len;
		rightkeylen= cons->rightlen;
	}
	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
	str1_to_str2(tab_meta_dir, '/', "tabletscheme");
	
	for (k = 0; k < 2; k++)
	{
		key_is_expand = FALSE;
		
		keycol = (k == 0) ? range_leftkey : range_rightkey;
		keycolen = (k == 0) ? leftkeylen : rightkeylen;

		if ((keycolen == 1) && (!strncasecmp("*", keycol, keycolen)))
		{
			key_is_expand = TRUE;
			
			if (k == 0)
			{
				
				rp = tablet_schm_get_1st_or_last_row(tab_hdr,
						tab_hdr->tab_id, TABLETSCHM_ID, 
						tab_meta_dir, TRUE);
			}
			else
			{
				
				rp = tablet_schm_get_1st_or_last_row(tab_hdr, 
						tab_hdr->tab_id, TABLETSCHM_ID, 
						tab_meta_dir, FALSE);
			}
		}
		else
		{
			
			rp = tablet_schm_srch_row(tab_hdr, tab_hdr->tab_id, 
						TABLETSCHM_ID, tab_meta_dir, 
					  	keycol, keycolen);
		}

		
		name = row_locate_col(rp, TABLETSCHM_TABLETNAME_COLOFF_INROW, 
					ROW_MINLEN_IN_TABLETSCHM, &namelen);


		if (k == 0)
		{
			MEMCPY(selwhere.lefttabletname, name, STRLEN(name));
			selwhere.leftnamelen = STRLEN(name);
		}
		else
		{
			MEMCPY(selwhere.righttabletname, name, STRLEN(name));
			selwhere.rightnamelen = STRLEN(name);
		}
	}	


	char	*resp_buf;

	col_buf_len = sizeof(SELWHERE) + sizeof(SVR_IDX_FILE) + sizeof(TABLEHDR) + 
					tab_hdr->tab_col * (sizeof(COLINFO));
	
	resp_buf = MEMALLOCHEAP(col_buf_len);
	MEMSET(resp_buf, col_buf_len);


	
	col_buf_idx = 0;		
	
	MEMCPY((resp_buf + col_buf_idx), &selwhere, sizeof(SELWHERE));
	col_buf_idx += sizeof(SELWHERE);


	MEMCPY((resp_buf + col_buf_idx), &(Master_infor->rg_list), 
					sizeof(SVR_IDX_FILE));
	col_buf_idx += sizeof(SVR_IDX_FILE);

	
	/* Save the meta (TABLEHDR) information into the SELWHERE structure. */
	MEMCPY((resp_buf + col_buf_idx), tab_hdr, sizeof(TABLEHDR));

	col_buf_idx += sizeof(TABLEHDR);

	MEMCPY((resp_buf + col_buf_idx), col_buf, tab_hdr->tab_col * sizeof(COLINFO));
	
	col_buf_idx += tab_hdr->tab_col * sizeof(COLINFO);
	


	rtn_stat = TRUE;

exit:
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, col_buf_idx, resp_buf);
	}
	else
	{
		rpc_status |= RPC_FAIL;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	if (resp_buf != NULL)
	{
		MEMFREEHEAP(resp_buf);
	}

	return resp;
}


char *
meta_addsstab(TREE *command, TABINFO *tabinfo)
{
	LOCALTSS(tss);
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	int		fd1;
	int		rtn_stat;
	TABLEHDR	*tab_hdr;
	char		*keycol;
	int		keycolen;
	char		*resp;
	int		tablet_min_rlen;
	int		namelen;
	char		*name;
	char		*rp;
	int		status;
	char		*sstab_name;
	int		sstab_name_len;
	
	char		*sstab_rp;
	int		sstab_rlen;
	char		*rg_addr;
	int		rpc_status;
	int		tabidx;


	Assert(command);

	rtn_stat = FALSE;
	rpc_status = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if ((tabidx = meta_table_is_exist(tab_dir)) == -1)
	{
		traceprint("Table %s is not exist!\n", tab_name);

		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}


	tab_hdr = &(Master_infor->meta_sysobj->sysobject[tabidx]);
	
	Assert(tab_hdr->tab_tablet > 0);

	if (tab_hdr->tab_tablet == 0)
	{
		traceprint("Table %s should be has one tablet at least\n", tab_name);
		ex_raise(EX_ANY);
	}

	if (tab_hdr->tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");
		goto exit;
	}
		
	
	sstab_name= par_get_colval_by_colid(command, 1, &sstab_name_len);

	int colen;
	char *colptr = par_get_colval_by_colid(command, 2, &colen);

	sstab_map = sstab_map_get(tab_hdr->tab_id, tab_dir, &tab_sstabmap);
	
	int sstab_id;

	sstab_id = m_atoi(colptr, colen);

	int split_ts;

	colptr = par_get_colval_by_colid(command, 3, &colen);
	split_ts = m_atoi(colptr, colen);

	int split_sstabid;
	
	colptr = par_get_colval_by_colid(command, 4, &colen);
	split_sstabid = m_atoi(colptr, colen);
	
	keycol = par_get_colval_by_colid(command, 5, &keycolen);

	
	SSTAB_MAP_SET_SPLIT_TS(split_sstabid, split_ts);
	
	SSTAB_MAP_SET(sstab_id, SSTAB_USED);

	
	sstab_rlen = ROW_MINLEN_IN_TABLET + keycolen + 4 + 4;

	sstab_rp = MEMALLOCHEAP(sstab_rlen);



	int res_sstab_id;
	res_sstab_id = meta_get_free_sstab();
	SSTAB_MAP_SET(res_sstab_id, SSTAB_RESERVED);		

	
	tablet_min_rlen = tablet_bld_row(sstab_rp, sstab_rlen, tab_name, 
					 tab_name_len, sstab_id, res_sstab_id,
					 sstab_name, sstab_name_len, keycol, 
					 keycolen, tab_hdr->tab_key_coltype);


	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
	str1_to_str2(tab_meta_dir, '/', "tabletscheme");

	
	rp = tablet_schm_srch_row(tab_hdr, tab_hdr->tab_id, TABLETSCHM_ID, 
				  tab_meta_dir, keycol, keycolen);

	name = row_locate_col(rp, TABLETSCHM_TABLETNAME_COLOFF_INROW, 
			      ROW_MINLEN_IN_TABLETSCHM, &namelen);

	int ign;
	rg_addr = row_locate_col(rp, TABLETSCHM_RGADDR_COLOFF_INROW, 
				 ROW_MINLEN_IN_TABLETSCHM, &ign);
	int rg_port = *(int *)row_locate_col(rp, TABLETSCHM_RGPORT_COLOFF_INROW,
				 ROW_MINLEN_IN_TABLETSCHM, &ign);

	tss->tcur_rgprof = rebalan_get_rg_prof_by_addr(rg_addr, rg_port);


	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));
	str1_to_str2(tab_meta_dir, '/', name);

	int tabletid;
	tabletid = *(int *)row_locate_col(rp, TABLETSCHM_TABLETID_COLOFF_INROW, 
					  ROW_MINLEN_IN_TABLETSCHM, &namelen);

	if (tablet_ins_row(tab_hdr, tab_hdr->tab_id, tabletid, tab_meta_dir, 
			   sstab_rp, ROW_MINLEN_IN_TABLET))
	{
		meta_save_rginfo();
	}

	MEMFREEHEAP(sstab_rp);

	
	(tab_hdr->tab_sstab)++;

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	str1_to_str2(tab_meta_dir, '/', "sysobjects");


	OPEN(fd1, tab_meta_dir, (O_RDWR));
	
	if (fd1 < 0)
	{
		goto exit;
	}
	
	status = WRITE(fd1, tab_hdr, sizeof(TABLEHDR));

	Assert(status == sizeof(TABLEHDR));

	if (status != sizeof(TABLEHDR))
	{
		traceprint("Table %s sysobjects hit error!\n", tab_name);
		CLOSE(fd1);
		ex_raise(EX_ANY);
	}
	
	CLOSE(fd1);

	sstab_map_put(-1, tss->ttab_sstabmap);

	rtn_stat = TRUE;

exit:
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}
	else
	{
		rpc_status |= RPC_FAIL;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}

	return resp;

}



static int
meta_get_free_sstab()
{
	int i = 0;

	while (i < SSTAB_MAP_ITEM)
	{
		if (SSTAB_MAP_FREE(i))
		{
			break;
		}

		i++;
	}

	return i;
}

static void
meta_prt_sstabmap(int begin, int end)
{
	while(begin < end)
	{
		traceprint("sstab_map[%d] == %d \n", begin, sstab_map[begin].sstab_stat);

		begin++;
	}
}
static int
meta_crt_rg_logbackup_file(char *rgip, int rgport)
{
	char	rglogfile[256];
	char	rgname[64];
	LOGFILE	*logfile;
	int	status;
	int	fd;


	MEMSET(rglogfile, 256);
	MEMCPY(rglogfile, LOG_FILE_DIR, STRLEN(LOG_FILE_DIR));

	MEMSET(rgname, 64);
	sprintf(rgname, "%s%d", rgip, rgport);

	str1_to_str2(rglogfile, '/', rgname);

	if (STAT(rglogfile, &st) != 0)
	{
		MKDIR(status, rglogfile, 0755);
	}
	
	str1_to_str2(rglogfile, '/', "log");

	OPEN(fd, rglogfile, (O_CREAT|O_WRONLY|O_TRUNC));

	logfile = (LOGFILE *)MEMALLOCHEAP(sizeof(LOGFILE));
	MEMSET(logfile,sizeof(LOGFILE));

	logfile->magic[0] = 'm';
	logfile->magic[1] = 'a';
	logfile->magic[2] = 'x';
	logfile->magic[3] = 't';
	logfile->magic[4] = 'a';
	logfile->magic[5] = 'b';
	logfile->magic[6] = 'l';
	logfile->magic[7] = 'e';
	logfile->magic[8] = 'l';
	logfile->magic[9] = 'o';
	logfile->magic[10] = 'g';	

	WRITE(fd, logfile, sizeof(LOGFILE));

	CLOSE(fd);	


	MEMSET(rglogfile, 256);
	MEMCPY(rglogfile, BACKUP_DIR, STRLEN(BACKUP_DIR));

	str1_to_str2(rglogfile, '/', rgname);

	if (STAT(rglogfile, &st) != 0)
	{
		MKDIR(status, rglogfile, 0755);
	}

	return status;
}


static int
meta_collect_rg(char * req_buf)
{
	char		*str;
	int		i;
	SVR_IDX_FILE	*rglist;
	int		found, start_heartbeat;
	RANGE_PROF	*rg_addr;
	int		rginfo_save;

	
	if (strncasecmp(RPC_RG2MASTER_REPORT, req_buf, 
				STRLEN(RPC_RG2MASTER_REPORT)) != 0)
	{
		return FALSE;
	}
	
	str = req_buf + RPC_MAGIC_MAX_LEN;

	rglist = &(Master_infor->rg_list);
	rginfo_save = FALSE;
	found = FALSE;
	start_heartbeat = TRUE;
	rg_addr = (RANGE_PROF *)(rglist->data);

	P_SPINLOCK(Master_infor->rglist_spinlock);
	
	for(i = 0; i < rglist->nextrno; i++)
	{
		if (   !strncasecmp(str, rg_addr[i].rg_addr, RANGE_ADDR_MAX_LEN)
		    && (rg_addr[i].rg_port == *(int *)(str + RANGE_ADDR_MAX_LEN))
		   )
		{
			found = TRUE;
			
			if(rg_addr[i].rg_stat & RANGER_IS_OFFLINE)
			{
				if(rg_addr[i].rg_stat & RANGER_NEED_RECOVERY)
				{
					rg_addr[i].rg_stat |= RANGER_RESTART;

					break;
				}
				
				rg_addr[i].rg_stat &= ~RANGER_IS_OFFLINE;
				rg_addr[i].rg_stat |= RANGER_IS_ONLINE;

				rginfo_save = TRUE;
				
				break;
			}
			else if(rg_addr[i].rg_stat & RANGER_IS_ONLINE)
			{
				if (HB_RANGER_IS_ON(&(Master_infor->heart_beat_data[i])))
				{
					/*
					** If the ranger server restart while it just crash in the interval of 
					** heartbeat,  it will hit a false issue here that the ranger has not
					** been create the heartbeat with meta server. In this senario, user 
					** will have to restart the ranger server again.
					*/
					traceprint("\n rg server with same ip and port is already on line \n");
					start_heartbeat = FALSE;
				}
				else
				{
					start_heartbeat = TRUE;
				}
				break;
			}

			
		}
	}


	if (!found)
	{
		MEMCPY(rg_addr[i].rg_addr, str, RANGE_ADDR_MAX_LEN);
		rg_addr[i].rg_port = *(int *)(str + RANGE_ADDR_MAX_LEN);
		rg_addr[i].rg_stat = RANGER_IS_ONLINE;
		rg_addr[i].rg_tablet_num = 0;
		rg_addr[i].rg_index = i;

		(rglist->nextrno)++;

		meta_crt_rg_logbackup_file(rg_addr[i].rg_addr,
						rg_addr[i].rg_port);
		
		rginfo_save = TRUE;

	}

	if(start_heartbeat)
	{			
		meta_heartbeat_setup(rg_addr + i);
	}

	if (rginfo_save)
	{
		meta_save_rginfo();
	}

	V_SPINLOCK(Master_infor->rglist_spinlock);
	
	return TRUE;
}



int
meta_rebalan_svr_idx_file(char *tab_dir, REBALANCE_DATA *rbd)
{
	SVR_IDX_FILE	*tablet_store;
	char		tab_dir1[256];
	int		fd;
	int		status;
	RANGE_PROF	*rg_prof;
	int 		i, j;
	int		max_tablet;
	int		min_tablet;
	int		max_rg;
	int		min_rg;
	int		total_tablet;
	int		transfer_tablet;
	SVR_IDX_FILE	*temp_store;


	tablet_store = (SVR_IDX_FILE *)MEMALLOCHEAP(sizeof(SVR_IDX_FILE));
	total_tablet = 0;
	transfer_tablet = 0;
	
	MEMSET(tab_dir1, 256);

	MEMCPY(tab_dir1, tab_dir, STRLEN(tab_dir));

	str1_to_str2(tab_dir1, '/', "tabletinranger");
	
	OPEN(fd, tab_dir1, (O_RDWR));

	if (fd < 0)
	{
		goto exit;
	}

	
	
	status = READ(fd, tablet_store, sizeof(SVR_IDX_FILE));

	Assert(status == sizeof(SVR_IDX_FILE));

	

	//rg_prof = (RANGE_PROF *)tablet_store->data;
	temp_store = &(Master_infor->rg_list);
	rg_prof = (RANGE_PROF *)(temp_store->data);

	for(j = 0; j < temp_store->nextrno; j++)
	{
		if(rg_prof[j].rg_stat & RANGER_IS_ONLINE)
		{
			max_tablet = min_tablet = rg_prof[j].rg_tablet_num;
			max_rg = min_rg = j;
			break;
		}
	}
	total_tablet += rg_prof[j].rg_tablet_num;
	
	for(i = j + 1; i < temp_store->nextrno; i++)
	{
		if(rg_prof[i].rg_stat & RANGER_IS_ONLINE)
		{
			if (rg_prof[i].rg_tablet_num > max_tablet)
			{
				max_tablet = rg_prof[i].rg_tablet_num;
				max_rg = i;
			}
			else if (rg_prof[i].rg_tablet_num < min_tablet)
			{
				min_tablet = rg_prof[i].rg_tablet_num;
				min_rg = i;
			}
			total_tablet += rg_prof[i].rg_tablet_num;
		}
	}

	if (max_tablet != min_tablet)
	{
		if (((max_tablet - min_tablet) / i) < 1)
		{
			transfer_tablet = 0;
		}
		else 
		{
			transfer_tablet = (total_tablet / i) - min_tablet;
		}
	}

	if (transfer_tablet > 0)
	{
		rg_prof[max_rg].rg_tablet_num = max_tablet - transfer_tablet;
		rg_prof[min_rg].rg_tablet_num = min_tablet + transfer_tablet;

		
		//WRITE(fd, tablet_store, sizeof(SVR_IDX_FILE));

		MEMCPY(rbd->rbd_max_tablet_rg, rg_prof[max_rg].rg_addr, 
			STRLEN(rg_prof[max_rg].rg_addr));
		rbd->rbd_max_tablet_rgport = rg_prof[max_rg].rg_port;
		MEMCPY(rbd->rbd_min_tablet_rg, rg_prof[min_rg].rg_addr, 
			STRLEN(rg_prof[min_rg].rg_addr));
		rbd->rbd_min_tablet_rgport = rg_prof[min_rg].rg_port;

		meta_save_rginfo();
	}

	
exit:
	CLOSE(fd);
	MEMFREEHEAP(tablet_store);

	return transfer_tablet;
}


int
meta_rebalan_process(REBALANCE_DATA *rbd)
{
	int		sockfd;
	RPCRESP		*resp;
	//RANGE_PROF	*rg_prof;
	int		rtn_stat;


	rtn_stat = TRUE;
	//rg_prof = rebalan_get_rg_prof_by_addr(rbd->rbd_max_tablet_rg);
	
	sockfd = conn_open(rbd->rbd_max_tablet_rg, rbd->rbd_max_tablet_rgport);

	//rg_prof = rebalan_get_rg_prof_by_addr(rbd->rbd_min_tablet_rg);

	//rbd->rbd_min_tablet_rgport = rg_prof->rg_port;

	tcp_put_data(sockfd, (char *)rbd, sizeof(REBALANCE_DATA));

	resp = conn_recv_resp(sockfd);

	if ((resp == NULL) || (resp->status_code != RPC_SUCCESS))
	{
		rtn_stat = FALSE;
		traceprint("\n ERROR \n");
	}
	
	conn_close(sockfd, NULL, resp);

	return rtn_stat;

}

char *
meta_rebalancer(TREE *command)
{
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[256];
	char		tab_dir1[256];	
	int		fd;
	char 		*resp;
	REBALANCE_DATA	*rbd;
	int		transfer_tablet;
	char		tab_tabletschm_dir[TABLE_NAME_MAX_LEN];
	int		rtn_stat;
	char		*tablet_schm_bp;
	int		rpc_status;


	Assert(command);
	
	rtn_stat = FALSE;
	resp = NULL;
	rpc_status = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;
	rbd = NULL;
	tablet_schm_bp = NULL;

	
	MEMSET(tab_dir, 256);
	MEMSET(tab_dir1, 256);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));
	str1_to_str2(tab_dir, '/', tab_name);

	if (STAT(tab_dir, &st) != 0)
	{		
		traceprint("Table %s is not exist!\n", tab_name);
		rpc_status |= RPC_TABLE_NOT_EXIST;
		goto exit;
	}

	rbd = (REBALANCE_DATA *)MEMALLOCHEAP(sizeof(REBALANCE_DATA));
	MEMSET(rbd, sizeof(REBALANCE_DATA));
	MEMCPY(rbd->rbd_tabname, tab_name, STRLEN(tab_name));
	
	transfer_tablet = meta_rebalan_svr_idx_file(tab_dir, rbd);

	if (transfer_tablet > 0)
	{
		TABLEHDR	tab_hdr;
		
		MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_tabletschm_dir, tab_dir, STRLEN(tab_dir));

		if (!(STAT(tab_dir, &st) == 0))
		{
			traceprint("Table %s is not exist.\n", tab_name);
			goto exit;
		}
		
		str1_to_str2(tab_tabletschm_dir, '/', "sysobjects");
	
		OPEN(fd, tab_tabletschm_dir, (O_RDONLY));
		
		if (fd < 0)
		{
			traceprint("Table is not exist! \n");
			goto exit;
		}
	
		
		READ(fd, &tab_hdr, sizeof(TABLEHDR));	
	
		CLOSE(fd);
		
		
		MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_tabletschm_dir, tab_dir, STRLEN(tab_dir));

		str1_to_str2(tab_tabletschm_dir, '/', "tabletscheme");

		TABINFO		*tabinfo;
		int		minrowlen;
		BLK_ROWINFO	blk_rowinfo;
		BUF		*bp;
		
		tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
		MEMSET(tabinfo, sizeof(TABINFO));
		tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
		MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

		tabinfo->t_rowinfo = &blk_rowinfo;
		MEMSET(tabinfo->t_rowinfo, sizeof(BLK_ROWINFO));

		tabinfo->t_dold = tabinfo->t_dnew = (BUF *) tabinfo;

		tabinfo_push(tabinfo);

		minrowlen = ROW_MINLEN_IN_TABLETSCHM;

		
		TABINFO_INIT(tabinfo, tab_tabletschm_dir, tabinfo->t_sinfo, minrowlen, 
				TAB_SCHM_INS, tab_hdr.tab_id, TABLETSCHM_ID);
		SRCH_INFO_INIT(tabinfo->t_sinfo, NULL, 0, TABLETSCHM_KEY_COLID_INROW, 
			       VARCHAR, -1);
				
		bp = blk_getsstable(tabinfo);
	
		tablet_schm_bp = (char *)(bp->bsstab->bblk);

		BLOCK *blk;

		int i = 0, rowno;
		int	*offset;
		char 	*rp;
		char	*addr_in_blk;
		int	addrlen_in_blk;
		int port_in_blk;
		int portlen_in_blk;
		//int	result;
		//char	*tabletname;
		//int	namelen;
		//char	tab_tablet_dir[TABLE_NAME_MAX_LEN];
		//int	fd1;
		
		while (TRUE)
		{
			blk = (BLOCK *)(tablet_schm_bp + i * BLOCKSIZE);

			
			for(rowno = 0, offset = ROW_OFFSET_PTR(blk); 
					rowno < blk->bnextrno; rowno++, offset--)
			{
				rp = (char *)blk + *offset;
			
				Assert(*offset < blk->bfreeoff);
			
				addr_in_blk = row_locate_col(rp, 
						TABLETSCHM_RGADDR_COLOFF_INROW,
						ROW_MINLEN_IN_TABLETSCHM, 
						&addrlen_in_blk);
				port_in_blk = *(int *)row_locate_col(rp, 
						TABLETSCHM_RGPORT_COLOFF_INROW, 
						ROW_MINLEN_IN_TABLETSCHM,
						&portlen_in_blk);
			
				if(   !strncasecmp(rbd->rbd_max_tablet_rg, 
						addr_in_blk, RANGE_ADDR_MAX_LEN) 
				   && (rbd->rbd_max_tablet_rgport == port_in_blk))
				{
					MEMCPY(addr_in_blk, rbd->rbd_min_tablet_rg,
							RANGE_ADDR_MAX_LEN);
					
					int *tmp_addr = (int *)row_locate_col(rp, 
							TABLETSCHM_RGPORT_COLOFF_INROW, 
							ROW_MINLEN_IN_TABLETSCHM, 
							&portlen_in_blk);
					
					*tmp_addr = rbd->rbd_min_tablet_rgport;
					
					transfer_tablet--;

					if (transfer_tablet == 0)
					{
						break;
					}
				}
				
				
			}

			i++;

			if (   (i > (BLK_CNT_IN_SSTABLE - 1)) 
			    || (transfer_tablet == 0))
			{
				break;
			}			
		
		}

		bufpredirty(bp->bsstab);
		bufdirty(bp->bsstab);
		bufunkeep(bp->bsstab);
		
		session_close(tabinfo);

		MEMFREEHEAP(tabinfo->t_sinfo);
		MEMFREEHEAP(tabinfo);

		tabinfo_pop();

	}

	rtn_stat = TRUE;
	
exit:

	if (rbd)
	{
		MEMFREEHEAP(rbd);
	}
	
	if (rtn_stat)
	{
		rpc_status |= RPC_SUCCESS;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}
	else
	{
		rpc_status |= RPC_FAIL;
		resp = conn_build_resp_byte(rpc_status, 0, NULL);
	}
	
	return resp;

}


char *
meta_handler(char *req_buf, int fd)
{
	LOCALTSS(tss);
	TREE		*command;
	int		resp_buf_idx;
	int		resp_buf_size;
	char		*resp;
	char		*tmp_req_buf;
	char		crt_tab_cmd[256];
	TABINFO		*tabinfo;

	
	tmp_req_buf = req_buf;
	tss->tmaster_infor = Master_infor;
	
	if (meta_collect_rg(req_buf))
	{		
		
		resp = conn_build_resp_byte(RPC_SUCCESS, 0, NULL);

		return resp;
	}
	//update meta and rg list  process
	if (meta_failover_rg(req_buf))
	{		
		
		resp = conn_build_resp_byte(RPC_SUCCESS, 0, NULL);

		return resp;
	}
	
parse_again:
	if (!parser_open(tmp_req_buf))
	{
		parser_close();
		tss->tstat |= TSS_PARSER_ERR;
		traceprint("PARSER ERR: Please input the command again by the 'help' signed.\n");
		return NULL;
	}

	volatile struct
	{
		TABINFO	*tabinfo;
	} copy;

	command = tss->tcmd_parser;
	resp_buf_idx = 0;
	resp_buf_size = 0;
	copy.tabinfo = NULL;
	tabinfo = NULL;
	resp = NULL;

	if(ex_handle(EX_ANY, yxue_handler))
	{
		tabinfo = copy.tabinfo;

		resp = conn_build_resp_byte(RPC_FAIL, 0, NULL);	
		goto close;
	}

	tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
	MEMSET(tabinfo, sizeof(TABINFO));
	
	tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
	MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

	tabinfo->t_dold = tabinfo->t_dnew = (BUF *) tabinfo;
	
	tabinfo_push(tabinfo);
	
	switch(command->sym.command.querytype)
	{
	    case ADDSERVER:
		meta_add_server(command);
		
		MEMSET(crt_tab_cmd, 256);
		MEMCPY(crt_tab_cmd, "create table", 12);
		str1_to_str2(crt_tab_cmd, ' ', command->sym.command.tabname);
		str1_to_str2(crt_tab_cmd, ' ', "(filename varchar, status int)");
		tmp_req_buf = crt_tab_cmd;
		parser_close();
		
		goto parse_again;
		
		break;
			
	    case TABCREAT:
		resp = meta_crtab(command);

		if (DEBUG_TEST(tss))
		{
			traceprint("I got here - CREAT TABLE\n");
		}
		
		break;

	    case INSERT:
	    	resp = meta_instab(command, tabinfo);
		break;

	    case CRTINDEX:
	    	break;

	    case SELECT:
		resp = meta_seldeltab(command, tabinfo);
		if (DEBUG_TEST(tss))
		{
			traceprint("I got here - SELECT TABLE\n");
		}
		
	    	break;

	    case DELETE:
	    	resp = meta_seldeltab(command, tabinfo);

		if (DEBUG_TEST(tss))
		{
			traceprint("I got here - DELETE TABLE\n");
		}
		
	    	break;

	    case SELECTRANGE:
	    	resp = meta_selrangetab(command, tabinfo);
		if (DEBUG_TEST(tss))
		{
			traceprint("I got here - SELECTRANGE TABLE\n");
		}
		
	    	break;
		
  	    case SELECTWHERE:
		resp = meta_selwheretab(command, tabinfo);
		
		if (DEBUG_TEST(tss))
		{
			traceprint("I got here - SELECTWHERE TABLE\n");
		}

		break;
		
	    case ADDSSTAB:
	    	resp = meta_addsstab(command, tabinfo);
	    	break;
		
	    case DROP:
	    	resp = meta_droptab(command);
	    	break;
		
	    case REMOVE:
	    	resp = meta_removtab(command);
	    	break;
		
	    case MCCTABLE:
	    	resp = meta_checkdata(command);
		break;
		
	    case MCCRANGER:
	    	resp = meta_checkranger(command);
	    	break;
		
	    case REBALANCE:
	    	resp = meta_rebalancer(command);
	    	break;

	    default:
	    	break;
	}

	session_close(tabinfo);	

close:

	tabinfo_pop();
	if (tabinfo!= NULL)
	{
		if (tabinfo->t_sinfo)
		{
			MEMFREEHEAP(tabinfo->t_sinfo);
		}
		MEMFREEHEAP(tabinfo);
	}
	parser_close();

	return resp;
}



static void 
meta_bld_sysrow(char *rp, int rlen, int tabletid, int sstabnum)
{
	int		rowidx;


	row_build_hdr(rp, 0, 0, 0);

	rowidx = sizeof(ROWFMT);

	*(int *)(rp + rowidx) = rlen;
	rowidx += sizeof(int);
	
	*(int *)(rp + rowidx) = tabletid;
	rowidx += sizeof(int);
	
	*(int *)(rp + rowidx) = sstabnum;
	rowidx += sizeof(int);

	Assert(rowidx == rlen);
}

static RANGE_PROF *
meta_get_rg()
{
	RANGE_PROF	*rg_prof;
	int		i, j;
	int		min_tablet;
	int		min_rg;
	SVR_IDX_FILE 	*temp_store;
	
	temp_store = &(Master_infor->rg_list);
	rg_prof = (RANGE_PROF *)(temp_store->data);

	for(i = 0; i < temp_store->nextrno; i++)
	{
		if(rg_prof[i].rg_stat & RANGER_IS_ONLINE)
		{
			min_tablet = rg_prof[i].rg_tablet_num;
			min_rg = i;
			break;
		}
	}
	
	if(i == temp_store->nextrno)
	{
		traceprint("No available rg server for insert!\n");
		return NULL;
	}
	
	for(j = i; j < temp_store->nextrno; j++)
	{
		if(   (rg_prof[j].rg_tablet_num < min_tablet)
		   && (rg_prof[j].rg_stat & RANGER_IS_ONLINE))
		{
			min_tablet = rg_prof[j].rg_tablet_num;
			min_rg = j;
		}			
	}

	return rg_prof + min_rg;
}

static RANGE_PROF *
meta_get_rg_by_ip_port(char *rgip, int rgport)

{
	int		i;
	SVR_IDX_FILE	*rglist;
	int		found, start_heartbeat;
	RANGE_PROF	*rg_addr;

	
	
	rglist = &(Master_infor->rg_list);
	found = FALSE;
	start_heartbeat = TRUE;
	rg_addr = (RANGE_PROF *)(rglist->data);

	for(i = 0; i < rglist->nextrno; i++)
	{
		if (   !strncasecmp(rgip, rg_addr[i].rg_addr, RANGE_ADDR_MAX_LEN)
		    && (rg_addr[i].rg_port == rgport)
		   )
		{
			return (rg_addr + i);
		}
	}

	return NULL;
}

void * meta_heartbeat(void *args)
{
	RANGE_PROF 	*rg_addr = (RANGE_PROF *)args;
	int 		hb_conn;
	char		send_buf[256];
	int 		idx;
	RPCRESP 	*resp;
	int 		rg_index = rg_addr->rg_index;
	char 		*hb_recv_buf;
	MSG_DATA	*new_msg;
	int		sleeptime;

	/* Wait 10s to make sure rg server's network service is ready. */
	sleep(10);

	hb_recv_buf = Master_infor->heart_beat_data[rg_index].recv_data;

	/* TODO: in this case(hb_conn<0), need to tell rg server register is failed. */
	if((hb_conn = conn_open(rg_addr->rg_addr, rg_addr->rg_port)) < 0)
	{
		perror("error in create connection to rg server when meta server start heart beat: ");
		rg_addr->rg_stat |= RANGER_IS_SUSPECT;
		goto finish;

	}

	signal (SIGPIPE,SIG_IGN);

	/* In memory information. */
	HB_SET_RANGER_ON(&(Master_infor->heart_beat_data[rg_index]));
	
	sleeptime = HEARTBEAT_INTERVAL;
	while(TRUE)
	{
		sleep(sleeptime);
		MEMSET(send_buf, 256);
		
		idx = 0;
		PUT_TO_BUFFER(send_buf, idx, RPC_REQUEST_MAGIC, 
					RPC_MAGIC_MAX_LEN);
		PUT_TO_BUFFER(send_buf, idx, RPC_MASTER2RG_HEARTBEAT,
					RPC_MAGIC_MAX_LEN);
		
		tcp_put_data(hb_conn, send_buf, idx);

		traceprint("\n###### meta sent heart beat to %s/%d. \n", rg_addr->rg_addr, rg_addr->rg_port);

		resp = conn_recv_resp_meta(hb_conn, hb_recv_buf);

		traceprint("\n###### meta recv heart beat from %s/%d. \n", rg_addr->rg_addr, rg_addr->rg_port);

		if (resp->status_code == RPC_UNAVAIL)
		{
			traceprint("\n rg server is un-available \n");

			if (rg_addr->rg_stat & RANGER_IS_SUSPECT)
			{
				goto finish;
			}

//			P_SPINLOCK(Master_infor->rglist_spinlock);
			rg_addr->rg_stat |= RANGER_IS_SUSPECT;
//			V_SPINLOCK(Master_infor->rglist_spinlock);
			sleeptime = 3;

		}
		else if (resp->status_code != RPC_SUCCESS)
		{
			traceprint("\n We got a non-success response. \n");

			if (rg_addr->rg_stat & RANGER_IS_SUSPECT)
			{
                        	goto finish;
			}

//			P_SPINLOCK(Master_infor->rglist_spinlock);
			rg_addr->rg_stat |= RANGER_IS_SUSPECT;
//			V_SPINLOCK(Master_infor->rglist_spinlock);
			sleeptime = 3;
		}

		if ((resp->status_code & RPC_SUCCESS) && (rg_addr->rg_stat & RANGER_IS_SUSPECT))
		{
//			P_SPINLOCK(Master_infor->rglist_spinlock);
			rg_addr->rg_stat &= ~RANGER_IS_SUSPECT;
//			V_SPINLOCK(Master_infor->rglist_spinlock);
			sleeptime = HEARTBEAT_INTERVAL;
		}

		/*
		** TODO: maybe more info will be added in hearbeat msg,  such as 
		** overload monitor, so need to add some process routine on resp
		** here in the future
		*/
	}

finish:
	
		//update meta and rg_list here, put update task to msg list

		HB_SET_RANGER_OFF(&(Master_infor->heart_beat_data[rg_index]));
		traceprint("\n HEARTBEAT hit error. \n");
		
		new_msg = (MSG_DATA *)msg_mem_alloc();
				
		idx = 0;
		
		PUT_TO_BUFFER(new_msg->data, idx, RPC_REQUEST_MAGIC, 
						RPC_MAGIC_MAX_LEN);
		PUT_TO_BUFFER(new_msg->data, idx, RPC_FAILOVER, 
						RPC_MAGIC_MAX_LEN);
		PUT_TO_BUFFER(new_msg->data, idx, rg_addr->rg_addr, 
						RANGE_ADDR_MAX_LEN);
		PUT_TO_BUFFER(new_msg->data, idx, &(rg_addr->rg_port), 
						RANGE_PORT_MAX_LEN);
		new_msg->fd = -1;
		new_msg->n_size = idx;
		new_msg->block_buffer = NULL;
		new_msg->next = NULL;
		
		pthread_mutex_lock(&mutex);
		if (msg_list_head == NULL)
		{
			msg_list_head = new_msg;
			msg_list_tail = new_msg;
		} 
		else
		{
			msg_list_tail->next = new_msg;
			msg_list_tail = new_msg;
		}
		msg_list_len++;
		
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&mutex);

		if(hb_conn > 0)
		{
			close(hb_conn);
		}
		
		pthread_detach(pthread_self());

	return NULL;
	
}


static void
meta_heartbeat_setup(RANGE_PROF * rg_addr)
{
	
	pthread_create(&(rg_addr->tid), NULL, meta_heartbeat, (void *)rg_addr);
}

static int
meta_transfer_target(char *target_ip, int * target_port)
{
	RANGE_PROF *rg_prof;
	int i, j;
	int min_tablet;
	int 	min_rg;
	SVR_IDX_FILE *temp_store;
	
	temp_store = &(Master_infor->rg_list);
	rg_prof = (RANGE_PROF *)(temp_store->data);

	for(i = 0; i < temp_store->nextrno; i++)
	{
		if(rg_prof[i].rg_stat & RANGER_IS_ONLINE)
		{
			min_tablet = rg_prof[i].rg_tablet_num;
			min_rg = i;
			break;
		}
	}
	
	if(i == temp_store->nextrno)
	{
		traceprint("No available rg server exist!\n");
		return -1;
	}
	
	for(j = i; j < temp_store->nextrno; j++)
	{
		if(   (rg_prof[j].rg_tablet_num < min_tablet)
		   && (rg_prof[j].rg_stat & RANGER_IS_ONLINE))
		{
			min_tablet = rg_prof[j].rg_tablet_num;
			min_rg = j;
		}			
	}

	MEMCPY(target_ip, rg_prof[min_rg].rg_addr, 
				STRLEN(rg_prof[min_rg].rg_addr));
	*target_port = rg_prof[min_rg].rg_port;

	return min_rg;
}

int meta_transfer_notify(char * rg_addr, int rg_port)
{
	int hb_conn;
	char	send_buf[256];
	int idx;
	RPCRESP * resp;
	int rtn_stat = TRUE;
		
	if((hb_conn = conn_open(rg_addr, rg_port)) < 0)
	{
		perror("error in create connection to rg server on meta server: ");
		rtn_stat= FALSE;
		goto finish;
	
	}
	
	MEMSET(send_buf, 256);
			
	idx = 0;
	PUT_TO_BUFFER(send_buf, idx, RPC_REQUEST_MAGIC, RPC_MAGIC_MAX_LEN);
	PUT_TO_BUFFER(send_buf, idx, RPC_MASTER2RG_NOTIFY, RPC_MAGIC_MAX_LEN);
				
	tcp_put_data(hb_conn, send_buf, idx);
	
	resp = conn_recv_resp(hb_conn);
	
	if ((resp == NULL) || (resp->status_code != RPC_SUCCESS))
	{
		traceprint("\n ERROR when send rsync notify to client \n");
		rtn_stat = FALSE;
			
	}
		
finish:
	if(hb_conn > 0)
		close(hb_conn);

	return rtn_stat;
}


static int
meta_tablet_update(char * table_name, char * rg_addr, int rg_port)
{
	char		tab_dir[256];
	int 		fd;
	char		tab_tabletschm_dir[TABLE_NAME_MAX_LEN];
	char		*tablet_schm_bp;
	char		target_ip[RANGE_ADDR_MAX_LEN];
	int		target_port;
	int		target_index;
	
	tablet_schm_bp = NULL;
		
	MEMSET(tab_dir, 256);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));
	str1_to_str2(tab_dir, '/', table_name);

	if (STAT(tab_dir, &st) != 0)
	{		
		traceprint("Table %s is not exist!\n", table_name);
		goto exit;
	}

	MEMSET(target_ip, RANGE_ADDR_MAX_LEN);	
	target_index = meta_transfer_target(target_ip, &target_port);

	if(target_index < 0)
	{
		traceprint("No available rg server exist!\n");
		return FALSE;
	}
	else
	{
		
		if(rg_port > 0)
		{
			int notify_ret = meta_transfer_notify(target_ip, 
							target_port);
			if(!notify_ret)
			{		
				traceprint("ERROR when send rsync notify to rg server!\n");
				goto exit;
			}
		}
	
		TABLEHDR	tab_hdr;
		
		MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_tabletschm_dir, tab_dir, STRLEN(tab_dir));

		str1_to_str2(tab_tabletschm_dir, '/', "sysobjects");

		OPEN(fd, tab_tabletschm_dir, (O_RDONLY));
		if (fd < 0)
		{
			traceprint("Table is not exist! \n");
			goto exit;
		}

		READ(fd, &tab_hdr, sizeof(TABLEHDR));
		CLOSE(fd);

		MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_tabletschm_dir, tab_dir, STRLEN(tab_dir));
		
		str1_to_str2(tab_tabletschm_dir, '/', "tabletscheme");

		TABINFO		*tabinfo;
		int		minrowlen;
		BLK_ROWINFO	blk_rowinfo;
		BUF		*bp;

		tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
		MEMSET(tabinfo, sizeof(TABINFO));

		tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
		MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

		tabinfo->t_rowinfo = &blk_rowinfo;
		MEMSET(tabinfo->t_rowinfo, sizeof(BLK_ROWINFO));

		tabinfo->t_dold = tabinfo->t_dnew = (BUF *) tabinfo;

		tabinfo_push(tabinfo);

		minrowlen = ROW_MINLEN_IN_TABLETSCHM;

		TABINFO_INIT(tabinfo, tab_tabletschm_dir, tabinfo->t_sinfo,
				minrowlen, TAB_SCHM_INS, tab_hdr.tab_id, 
				TABLETSCHM_ID);

		SRCH_INFO_INIT(tabinfo->t_sinfo, NULL, 0,
				TABLETSCHM_KEY_COLID_INROW, VARCHAR, -1);

		bp = blk_getsstable(tabinfo);

		tablet_schm_bp = (char *)(bp->bsstab->bblk);
			
		BLOCK *blk;
	
		int i, rowno;
		int *offset;
		char	*rp;
		char	*addr_in_blk;
		int addrlen_in_blk;
		int port_in_blk;
		int portlen_in_blk;
		RANGE_PROF *rg_prof = (RANGE_PROF *)(Master_infor->rg_list.data);
			
		for(i = 0; i < BLK_CNT_IN_SSTABLE; i ++)
		{
			blk = (BLOCK *)(tablet_schm_bp + i * BLOCKSIZE);	
				
			for(rowno = 0, offset = ROW_OFFSET_PTR(blk); 
				rowno < blk->bnextrno; rowno++, offset--)
			{
				rp = (char *)blk + *offset;
				
				Assert(*offset < blk->bfreeoff);
				
				addr_in_blk = row_locate_col(rp, 
						TABLETSCHM_RGADDR_COLOFF_INROW,
						ROW_MINLEN_IN_TABLETSCHM,
						&addrlen_in_blk);
				
				port_in_blk = *(int *)row_locate_col(rp,
						TABLETSCHM_RGPORT_COLOFF_INROW, 
						ROW_MINLEN_IN_TABLETSCHM, 
						&portlen_in_blk);

				if(   (!strncasecmp(rg_addr, addr_in_blk,
							RANGE_ADDR_MAX_LEN)
				   && (rg_port == port_in_blk))
						||(rg_port == -1))
				{
					MEMCPY(addr_in_blk, target_ip, 
							RANGE_ADDR_MAX_LEN);
					int *tmp_addr = (int *)row_locate_col(rp, 
							TABLETSCHM_RGPORT_COLOFF_INROW,
							ROW_MINLEN_IN_TABLETSCHM, 
							&portlen_in_blk);
					
					*tmp_addr = target_port;
					
					rg_prof[target_index].rg_tablet_num ++;
					traceprint("transfer meta to: %s/%d \n", target_ip, target_port);
				}
					
					
			}
			
		}
	
		bufpredirty(bp->bsstab);
		bufdirty(bp->bsstab);
		bufunkeep(bp->bsstab);

		session_close(tabinfo);

		MEMFREEHEAP(tabinfo->t_sinfo);
		MEMFREEHEAP(tabinfo);

		tabinfo_pop();

		meta_save_rginfo();
	}
		
exit:

	return TRUE;
	
}


static void
meta_update(char * rg_addr, int rg_port)
{
	char tab_dir[256];	
	
	MEMSET(tab_dir, 256);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	if (STAT(tab_dir, &st) != 0)
	{		
		traceprint("Table dir %s is not exist!\n", tab_dir);
		goto exit;
	}

#ifdef MT_KFS_BACKEND

	MT_ENTRIES	mt_entries;

	MEMSET(&mt_entries, sizeof(MT_ENTRIES));

	if (!READDIR(tab_dir, (char *)&mt_entries))
	{
		traceprint("Read dir %s hit error.\n", tab_dir);
		goto exit;
	}

	int i;

	for (i = 0; i < mt_entries.ent_num; i++)
	{
		if(   strcmp(mt_entries.tabname[i],".")==0 
		   || strcmp(mt_entries.tabname[i],"..")==0)
		{
			continue;
		}
		
		if (!meta_tablet_update(mt_entries.tabname[i], rg_addr, rg_port))
		{
			
			break;
		}
	}

#else
	char tab_name[256];
	DIR *pDir ;
	struct dirent *ent ;
	
	pDir=opendir(tab_dir);
	while((ent=readdir(pDir))!=NULL)
	{
		if(ent->d_type & DT_DIR)
		{
			if(strcmp(ent->d_name,".")==0 || strcmp(ent->d_name,"..")==0)
			{
				continue;
			}
			
			MEMSET(tab_name, 256);
			sprintf(tab_name, "%s", ent->d_name);
			if (!meta_tablet_update(tab_name, rg_addr, rg_port))
			{
				
				break;
			}
		}
	}
#endif

exit:
	return;
}



static int
meta_failover_rg(char * req_buf)
{
	char		*str;
	int		i;
	SVR_IDX_FILE	*rglist;
	int		found;
	RANGE_PROF	*rg_addr;

	
	if (strncasecmp(RPC_FAILOVER, req_buf, STRLEN(RPC_FAILOVER)) != 0)
	{
		return FALSE;
	}
	
	
	str = req_buf + RPC_MAGIC_MAX_LEN;


	rglist = &(Master_infor->rg_list);
	found = FALSE;
	rg_addr = (RANGE_PROF *)(rglist->data);

	P_SPINLOCK(Master_infor->rglist_spinlock);
	
	for(i = 0; i < rglist->nextrno; i++)
	{
		if (   !strncasecmp(str, rg_addr[i].rg_addr, RANGE_ADDR_MAX_LEN)
		    && (rg_addr[i].rg_port == *(int *)(str + RANGE_ADDR_MAX_LEN))
		   )
		{
			found = TRUE;
			
			if(rg_addr[i].rg_stat & RANGER_IS_ONLINE)
			{
				
				/* Before failover, ranger server must be the suspected. */
				Assert(rg_addr[i].rg_stat & RANGER_IS_SUSPECT);

				/* Change the range state to the next state. */
				rg_addr[i].rg_stat &= ~(RANGER_IS_ONLINE | RANGER_IS_SUSPECT);
				rg_addr[i].rg_stat = RANGER_IS_OFFLINE | RANGER_NEED_RECOVERY;
		
				break;
			}
			else
			{
				traceprint("\n error, rg server to be off-line is already off line \n");
			}
		}
	}

	if (!found)
	{
		traceprint("\n error, rg server to be off_line is not exist in rg list \n");
	}

	V_SPINLOCK(Master_infor->rglist_spinlock);
	
	return TRUE;
}

void *
meta_recovery()
{
	int		i;
	SVR_IDX_FILE	*rglist;
	RANGE_PROF	*rg_addr;	
	
again:
	sleep(META_RECOVERY_INTERVAL);
	rglist = &(Master_infor->rg_list);
	rg_addr = (RANGE_PROF *)(rglist->data);

	for(i = 0; i < rglist->nextrno; i++)
	{
		if(rg_addr[i].rg_stat & RANGER_NEED_RECOVERY)
		{
			P_SPINLOCK(Master_infor->rglist_spinlock);
			
			Assert(  (rg_addr[i].rg_stat & RANGER_IS_OFFLINE) 
			       | (rg_addr[i].rg_stat & RANGER_RESTART));

			
			char	send_buf[256];
			char	recv_buf[128];

			MEMSET(send_buf, 256);
			MEMSET(recv_buf, 128);

			int 		idx = 0;
			int		fd;
			RPCRESP 	*resp;

			RANGE_PROF *rg_prof;

			if (rg_addr[i].rg_stat & RANGER_RESTART)
			{
				rg_prof = rg_addr + i;

				Assert(rg_prof);
			}
			else
			{
				rg_prof = meta_get_rg();
			}
			
			if (rg_prof == NULL) 
			{
				traceprint("Can not get the ranger server for the recovery.\n");
				V_SPINLOCK(Master_infor->rglist_spinlock);
				goto again;
			}
			
			if ((fd = conn_open(rg_prof->rg_addr, rg_prof->rg_port)) < 0)
			{
				traceprint("Fail to connect to server (%s:%d) for the recovery.\n", rg_prof->rg_addr, rg_prof->rg_port);
				V_SPINLOCK(Master_infor->rglist_spinlock);
				goto again;
			}
			
			PUT_TO_BUFFER(send_buf, idx, RPC_REQUEST_MAGIC, 
						RPC_MAGIC_MAX_LEN);
			PUT_TO_BUFFER(send_buf, idx, RPC_RECOVERY,
						RPC_MAGIC_MAX_LEN);
			PUT_TO_BUFFER(send_buf, idx, rg_addr[i].rg_addr, 
						RANGE_ADDR_MAX_LEN);
			PUT_TO_BUFFER(send_buf, idx, &(rg_addr[i].rg_port), 
						sizeof(int));
				
			tcp_put_data(fd, send_buf, idx);

			resp = conn_recv_resp_meta(fd, recv_buf);

			if (resp->status_code == RPC_UNAVAIL)
			{
				traceprint("\n rg server is un-available \n");
				conn_close(fd, NULL, NULL);
				V_SPINLOCK(Master_infor->rglist_spinlock);
				goto again;

			}
			else if (resp->status_code != RPC_SUCCESS)
			{
				traceprint("\n We got a non-success response. \n");
	                        conn_close(fd, NULL, NULL);
				V_SPINLOCK(Master_infor->rglist_spinlock);
	                        goto again;
			}

			conn_close(fd, NULL, NULL);

			rg_addr[i].rg_stat &= ~RANGER_NEED_RECOVERY;

			if (rg_prof->rg_stat & RANGER_RESTART)
			{
				Assert((rg_addr + i) == rg_prof);

				rg_addr[i].rg_stat &= ~(RANGER_IS_OFFLINE | RANGER_RESTART);

				rg_addr[i].rg_stat |= RANGER_IS_ONLINE;
			}						
			
			//update tablet
			if(rg_addr[i].rg_tablet_num > 0)
			{				
				meta_update(rg_addr[i].rg_addr, rg_addr[i].rg_port);
			}

			rg_addr[i].rg_tablet_num = 0;

			meta_save_rginfo();

			V_SPINLOCK(Master_infor->rglist_spinlock);
			
			break;

			
		}
		
		
	}

	goto again;
	
	return NULL;
}

int
meta_ranger_is_online(char *rg_ip, int rg_port)
{
	int		i;
	SVR_IDX_FILE 	*rglist = &(Master_infor->rg_list);			
	RANGE_PROF 	*rg_addr = (RANGE_PROF *)(rglist->data);


	for(i = 0; i < rglist->nextrno; i++)
	{
		if (   !strncasecmp(rg_ip, rg_addr[i].rg_addr,
					RANGE_ADDR_MAX_LEN)
		    && (rg_addr[i].rg_port == rg_port)
		   )
		{
			if(rg_addr[i].rg_stat & RANGER_IS_ONLINE)
			{
				return TRUE;
			}
		}
	}

	return FALSE;
}

void
meta_check_tablet(char *tabdir, int tabid, char *tabletname, int tabletid)
{
	char		tab_tabletschm_dir[TABLE_NAME_MAX_LEN];
	char		*tablet_schm_bp;
	TABINFO		*tabinfo;
	int		minrowlen;
	BLK_ROWINFO	blk_rowinfo;
	BUF		*bp;


	MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_tabletschm_dir, tabdir, STRLEN(tabdir));
	
	str1_to_str2(tab_tabletschm_dir, '/', tabletname);
	

	tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
	MEMSET(tabinfo, sizeof(TABINFO));

	tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
	MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

	tabinfo->t_rowinfo = &blk_rowinfo;
	MEMSET(tabinfo->t_rowinfo, sizeof(BLK_ROWINFO));

	tabinfo->t_dold = tabinfo->t_dnew = (BUF *) tabinfo;

	tabinfo_push(tabinfo);

	minrowlen = ROW_MINLEN_IN_TABLET;

	TABINFO_INIT(tabinfo, tab_tabletschm_dir, tabinfo->t_sinfo,
			minrowlen, TAB_SCHM_SRCH, tabid, tabletid);
	SRCH_INFO_INIT(tabinfo->t_sinfo, NULL, 0, TABLET_KEY_COLID_INROW, 
		       VARCHAR, -1);

	bp = blk_getsstable(tabinfo);

	tablet_schm_bp = (char *)(bp->bsstab->bblk);
		
	BLOCK 		*blk;

	int		i, rowno;
	int 		*offset;
	char		*rp;
	char 		*key_in_blk;
	int		keylen_in_blk;
	char		*lastkey_in_blk;
	int		lastkeylen_in_blk;
	int 		result;
	
	for(i = 0; i < BLK_CNT_IN_SSTABLE; i ++)
	{
		blk = (BLOCK *)(tablet_schm_bp + i * BLOCKSIZE);	
			
		for(rowno = 0, offset = ROW_OFFSET_PTR(blk); 
			rowno < blk->bnextrno; rowno++, offset--)
		{
			rp = (char *)blk + *offset;

			
			if(*offset > blk->bfreeoff)
			{
				traceprint("%s(%d): %dth block contains invalid %dth offset\n",tabletname, tabletid, blk->bblkno, rowno);
			}

			key_in_blk = row_locate_col(rp,
						TABLE_KEY_FAKE_COLOFF_INROW,
						minrowlen, &keylen_in_blk);

			if (rowno > 0)
			{
				result = row_col_compare(VARCHAR, key_in_blk,
						keylen_in_blk, lastkey_in_blk, 
						lastkeylen_in_blk);
				
				if (result != GR)
				{
					traceprint("%s(%d): the %dth block hit index issue\n",tabletname, tabletid, blk->bblkno);
				}
			}
			
			lastkey_in_blk = key_in_blk;
			lastkeylen_in_blk = keylen_in_blk; 
		}
		
	}

	bufunkeep(bp->bsstab);
	
	session_close(tabinfo);

	MEMFREEHEAP(tabinfo->t_sinfo);
	MEMFREEHEAP(tabinfo);

	tabinfo_pop();
}


void
meta_check_tabletschme(char *tabdir, int tabid)
{
	char		tab_tabletschm_dir[TABLE_NAME_MAX_LEN];
	char		*tablet_schm_bp;
	TABINFO		*tabinfo;
	int		minrowlen;
	BLK_ROWINFO	blk_rowinfo;
	BUF		*bp;


	MEMSET(tab_tabletschm_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_tabletschm_dir, tabdir, STRLEN(tabdir));
	
	str1_to_str2(tab_tabletschm_dir, '/', "tabletscheme");
	

	tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
	MEMSET(tabinfo, sizeof(TABINFO));

	tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
	MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

	tabinfo->t_rowinfo = &blk_rowinfo;
	MEMSET(tabinfo->t_rowinfo, sizeof(BLK_ROWINFO));

	tabinfo->t_dold = tabinfo->t_dnew = (BUF *) tabinfo;

	tabinfo_push(tabinfo);

	minrowlen = ROW_MINLEN_IN_TABLETSCHM;

	TABINFO_INIT(tabinfo, tab_tabletschm_dir, tabinfo->t_sinfo, minrowlen,
			(TAB_RESERV_BUF | TAB_SCHM_SRCH), tabid, TABLETSCHM_ID);

	SRCH_INFO_INIT(tabinfo->t_sinfo, NULL, 0, TABLETSCHM_KEY_COLID_INROW, 
					VARCHAR, -1);

	
	bp = blk_getsstable(tabinfo);

	tablet_schm_bp = (char *)(bp->bsstab->bblk);
		
	BLOCK 		*blk;

	int		i, rowno;
	int 		*offset;
	char		*rp;
	char		*addr_in_blk;
	int 		port_in_blk;
	char 		*key_in_blk;
	int		keylen_in_blk;
	char		*lastkey_in_blk;
	int		lastkeylen_in_blk;
	int		ign;
	int 		result;
	char		*tabletname;
	int		tabletid;

		
	for(i = 0; i < BLK_CNT_IN_SSTABLE; i ++)
	{
		blk = (BLOCK *)(tablet_schm_bp + i * BLOCKSIZE);	
			
		for(rowno = 0, offset = ROW_OFFSET_PTR(blk); 
			rowno < blk->bnextrno; rowno++, offset--)
		{
			rp = (char *)blk + *offset;

			
			if(*offset > blk->bfreeoff)
			{
				traceprint("tabletscheme: %dth block contains invalid %dth offset\n", blk->bblkno, rowno);
			}

			key_in_blk = row_locate_col(rp, 
						TABLETSCHM_KEY_FAKE_COLOFF_INROW,
						minrowlen, &keylen_in_blk);

			if (rowno > 0)
			{
				result = row_col_compare(VARCHAR, key_in_blk, 
						keylen_in_blk, lastkey_in_blk, 
						lastkeylen_in_blk);
				
				if (result != GR)
				{
					traceprint("tabletscheme: the %dth block hit index issue\n", blk->bblkno);
				}
			}
			
			lastkey_in_blk = key_in_blk;
			lastkeylen_in_blk = keylen_in_blk; 
			
			addr_in_blk = row_locate_col(rp, 
						TABLETSCHM_RGADDR_COLOFF_INROW,
						ROW_MINLEN_IN_TABLETSCHM, &ign);
			
			port_in_blk = *(int *)row_locate_col(rp, 
						TABLETSCHM_RGPORT_COLOFF_INROW, 
						ROW_MINLEN_IN_TABLETSCHM, &ign);

			if (!meta_ranger_is_online(addr_in_blk, port_in_blk))
			{
				traceprint("tabletscheme: ranger server %s:%d is NOT online\n", addr_in_blk, port_in_blk);
			}


			tabletname = row_locate_col(rp, 
					TABLETSCHM_TABLETNAME_COLOFF_INROW, 
					minrowlen, &ign);
			tabletid = *(int *)row_locate_col(rp, 
					TABLETSCHM_TABLETID_COLOFF_INROW, 
					minrowlen, &ign);

			meta_check_tablet(tabdir, tabid, tabletname, tabletid);
				
		}
		
	}

	bufunkeep(bp->bsstab);
	
	session_close(tabinfo);

	MEMFREEHEAP(tabinfo->t_sinfo);
	MEMFREEHEAP(tabinfo);

	tabinfo_pop();

}



static char *
meta_checkdata(TREE *command)
{
	char		*tab_name;
	int		tab_name_len;
	char		tab_dir[TABLE_NAME_MAX_LEN];
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	char   		*col_buf;
	int		fd1;
	int		rtn_stat;
	TABLEHDR	tab_hdr;
	int		sstab_rlen;
	int		sstab_idx;
	char		*resp;



	Assert(command);

	rtn_stat = FALSE;
	col_buf= NULL;
	sstab_rlen = 0;
	sstab_idx = 0;
	tab_name = command->sym.command.tabname;
	tab_name_len = command->sym.command.tabname_len;

	MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_dir, MT_META_TABLE, STRLEN(MT_META_TABLE));

	
	str1_to_str2(tab_dir, '/', tab_name);

	
	MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
	MEMCPY(tab_meta_dir, tab_dir, STRLEN(tab_dir));

	if (!(STAT(tab_dir, &st) == 0))
	{
		traceprint("Table %s is not exist.\n", tab_name);
		goto exit;
	}
	
	str1_to_str2(tab_meta_dir, '/', "sysobjects");

	OPEN(fd1, tab_meta_dir, (O_RDONLY));
	
	if (fd1 < 0)
	{
		traceprint("Table is not exist! \n");
		goto exit;
	}

	
	READ(fd1, &tab_hdr, sizeof(TABLEHDR));	

	CLOSE(fd1);

	if (tab_hdr.tab_tablet == 0)
	{
		traceprint("Table %s has no data.\n", tab_name);
		goto exit;
	}

	if (tab_hdr.tab_stat & TAB_DROPPED)
	{
		traceprint("This table has been dropped.\n");
		goto exit;
	}

	meta_check_tabletschme(tab_dir, tab_hdr.tab_id);
	
	rtn_stat = TRUE;

exit:
	if (rtn_stat)
	{
		
		resp = conn_build_resp_byte(RPC_SUCCESS, 0, NULL);
	}
	else
	{
		resp = conn_build_resp_byte(RPC_FAIL, 0, NULL);
	}

	return resp;
}




static char *
meta_checkranger(TREE *command)
{
	int		rtn_stat;
	char		*resp;
	RANGE_PROF	*rg_prof;
	int		i;
	SVR_IDX_FILE 	*temp_store;


	Assert(command);

	rtn_stat = FALSE;	
	
	temp_store = &(Master_infor->rg_list);
	rg_prof = (RANGE_PROF *)(temp_store->data);

	for(i = 0; i < temp_store->nextrno; i++)
	{
		if (rg_prof[i].rg_stat & RANGER_IS_ONLINE)
		{
			traceprint("Ranger %d (%s:%d) is ON-LINE\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
			rtn_stat = TRUE;
		}

		if (rg_prof[i].rg_stat & RANGER_IS_OFFLINE)
		{
			traceprint("Ranger %d (%s:%d) is OFF-LINE\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
			rtn_stat = TRUE;
		}

		if (rg_prof[i].rg_stat & RANGER_IS_SUSPECT)
		{
			traceprint("Ranger %d (%s:%d) is SUSPECT\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
			rtn_stat = TRUE;
		}

		
		if (rg_prof[i].rg_stat & RANGER_NEED_RECOVERY)
		{
			traceprint("Ranger %d (%s:%d) need to recovry\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
			rtn_stat = TRUE;
		}

		
		if (rg_prof[i].rg_stat & RANGER_RESTART)
		{
			traceprint("Ranger %d (%s:%d) has been restart.\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
			rtn_stat = TRUE;
		}
		
		if (!rtn_stat)
		{
			traceprint("Ranger %d (%s:%d) is invalid-state\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
		}

		if (HB_RANGER_IS_ON(&(Master_infor->heart_beat_data[i])))
		{
			traceprint("Ranger %d (%s:%d) has created the heartbeat with metaserver.\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
		}
		else
		{
			traceprint("Ranger %d (%s:%d) has not created the heartbeat with metaserver.\n", i, rg_prof[i].rg_addr, rg_prof[i].rg_port);
		}
	}

	/* TODO: tablet location checking. */
	
	if (rtn_stat)
	{
		
		resp = conn_build_resp_byte(RPC_SUCCESS, 0, NULL);
	}
	else
	{
		resp = conn_build_resp_byte(RPC_FAIL, 0, NULL);
	}

	return resp;
}

static int
meta_table_is_exist(char *tabname)
{
	int	i;
	int	tabidx;


	tabidx = -1;


	for (i = 0; i < Master_infor->meta_systab->tabnum; i++)
	{
		if(strcmp(tabname, Master_infor->meta_systab->meta_tabdir[i])==0)
		{
			return i;
		}
	}

	return tabidx;
}

static int
meta_load_sysmeta()
{
	int		i;
	int		fd;
	char		tab_meta_dir[TABLE_NAME_MAX_LEN];
	int		status;

	
	for (i = 0; i < Master_infor->meta_systab->tabnum; i++)
	{
		MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
		MEMCPY(tab_meta_dir, Master_infor->meta_systab->meta_tabdir[i], STRLEN(Master_infor->meta_systab->meta_tabdir[i]));

		
		str1_to_str2(tab_meta_dir, '/', "sysobjects");


		OPEN(fd, tab_meta_dir, (O_RDWR));
		
		if (fd < 0)
		{
			return FALSE;
		}

		status = READ(fd, &(Master_infor->meta_sysobj->sysobject[i]), sizeof(TABLEHDR));

		if(status != sizeof(TABLEHDR))
		{
			CLOSE(fd);

			return FALSE;
		}

		CLOSE(fd);


		MEMSET(tab_meta_dir, TABLE_NAME_MAX_LEN);
		
		MEMCPY(tab_meta_dir, Master_infor->meta_systab->meta_tabdir[i], STRLEN(Master_infor->meta_systab->meta_tabdir[i]));
		
		str1_to_str2(tab_meta_dir, '/', "syscolumns");

		OPEN(fd, tab_meta_dir, (O_RDONLY));

		if (fd < 0)
		{
			return FALSE;
		}

		Master_infor->meta_syscol->colnum[i] = Master_infor->meta_sysobj->sysobject[i].tab_col;

		Assert(Master_infor->meta_sysobj->sysobject[i].tab_col < COL_MAX_NUM);
		
		status = READ(fd, &(Master_infor->meta_syscol->colinfor[i]), Master_infor->meta_sysobj->sysobject[i].tab_col * sizeof(COLINFO));

		if(status != Master_infor->meta_sysobj->sysobject[i].tab_col * sizeof(COLINFO))
		{
			CLOSE(fd);

			return FALSE;
		}

		CLOSE(fd);


		

	}

	return TRUE;
}


int main(int argc, char *argv[])
{
	char *conf_path;
	pthread_t pthread_id;


	mem_init_alloc_regions();

	Trace = 0;
	conf_path = META_DEFAULT_CONF_PATH;
	conf_get_path(argc, argv, &conf_path);

	tss_setup(TSS_OP_METASERVER);
	
	meta_server_setup(conf_path);


	pthread_create(&pthread_id, NULL, meta_recovery, NULL);;
	
	startup(Master_infor->port, TSS_OP_METASERVER, meta_handler);

	return TRUE;
}

/*
** log.c 2011-10-27 xueyingfei
**
** Copyright Transoft Corp.
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

#include "global.h"
#include "strings.h"
#include "master/metaserver.h"
#include "ranger/rangeserver.h"
#include "memcom.h"
#include "buffer.h"
#include "block.h"
#include "file_op.h"
#include "utils.h"
#include "tss.h"
#include "log.h"
#include "row.h"
#include "type.h"
#include "timestamp.h"
#include "session.h"
#include "rginfo.h"


extern TSS	*Tss;
RG_LOGINFO	*Rg_loginfo = NULL;

static int
log__redo_insdel(LOGREC *logrec, char *rp);

static int
log_get_last_logoffset(LOGREC *logrec);



static int
log_check_insert(LOGFILE *logfile, int logopid, int logtype)
{
	int	lastopid;
	int	status;


	status = FALSE;

	
	if ((logopid == LOG_BEGIN) && (logfile->logtotnum == 0))
    	{
    		return TRUE;
    	}

	
	Assert((logfile->logtotnum > 0) && (logfile->logtotnum < (LOG_TOTALNUM + 1)));

	
	if (logtype != logfile->logtype)
	{
		return TRUE;
	}

	
	lastopid = logfile->logrec[logfile->logtotnum - 1].opid;

	switch (lastopid)
	{
	    case LOG_BEGIN:
	    	
	    	if (logopid == LOG_DO_SPLIT)
	    	{
	    		status = TRUE;
	    	}
	    	break;
		
	    case LOG_END:
	    	
	    	if (logopid == LOG_BEGIN)
	    	{
	    		status = log_delete(logfile, logfile->logtype);
	    	}
	    	break;
		
	    case LOG_DO_SPLIT:
	    	if (logopid == LOG_END)
	    	{
	    		status = TRUE;
	    	}
	    	break;
		
	    default:
	    			
	    	break;
	}

	return status;
}

static int
log_check_delete(LOGFILE *logfile, int logtype, char *backup)
{
	int	status;
	LOGREC	*logrec;


	status = FALSE;
	
	if (logtype != logfile->logtype)
	{
		return status;
	}

	logrec = logfile->logrec;

	switch (logtype)
	{
	    case SPLIT_LOG:
	    	if (logfile->logtotnum != 3)
	    	{
	    		break;
	    	}

		if (   (logrec[0].opid == LOG_BEGIN) 
		    && (logrec[1].opid == LOG_DO_SPLIT)
		    && (logrec[2].opid == LOG_END))
		{
			char	tmpsstab[TABLE_NAME_MAX_LEN];

			MEMSET(tmpsstab, TABLE_NAME_MAX_LEN);
			int i = strmnstr(logrec[1].oldsstabname, "/", 
					STRLEN(logrec[1].oldsstabname));
	
			MEMCPY(tmpsstab, logrec[1].oldsstabname + i, 
					STRLEN(logrec[1].oldsstabname + i));

			char	cmd_str[TABLE_NAME_MAX_LEN];
			
			MEMSET(cmd_str, TABLE_NAME_MAX_LEN);
			
#ifdef MT_KFS_BACKEND
			sprintf(cmd_str, "%s/%s",  backup, tmpsstab);
			RMFILE(status, cmd_str);
			if(!status)
#else			
			sprintf(cmd_str, "rm -rf %s/%s",  backup, tmpsstab);
			
			if (!system(cmd_str))
#endif
			{
				status = TRUE;
			}
		}

		break;
		
	    	

	    default:
	    			
	    	break;
	}

	return status;
}

void
log_build(LOGREC *logrec, int logopid, unsigned int oldts, unsigned int newts, 
	char *oldsstab, char *newsstab, int minrowlen, int tabid, int sstabid)
{
	MEMSET(logrec, sizeof(LOGREC));
	
	logrec->opid = logopid;
	logrec->oldts = oldts;
	logrec->newts = newts;
	logrec->minrowlen = minrowlen;
	logrec->tabid = tabid;
	logrec->sstabid = sstabid;

	
	if (logopid == CHECKPOINT_BEGIN)
	{
		char *s1 = "CHECKPOINT_BEGIN\0";
		MEMCPY(logrec->oldsstabname, s1, STRLEN(s1));
	}

	if (logopid == CHECKPOINT_COMMIT)
	{
		char *s1 = "CHECKPOINT_COMMIT\0";
		MEMCPY(logrec->oldsstabname, s1, STRLEN(s1));
	}

	if (oldsstab)
	{
		MEMCPY(logrec->oldsstabname, oldsstab,SSTABLE_NAME_MAX_LEN);
	}

	if (newsstab)
	{
		MEMCPY(logrec->newsstabname, newsstab,SSTABLE_NAME_MAX_LEN);
	}
}



int
log_insert_sstab_split(char *logfile_dir, LOGREC *logrec, int logtype)
{
	LOCALTSS(tss);
	int		fd;
	int		status;
	LOGFILE		*logfilebuf;	

	
	logfilebuf = (LOGFILE *)MEMALLOCHEAP(sizeof(LOGFILE));
	
	OPEN(fd, logfile_dir, (O_RDWR));

	if (fd < 0)
	{
		goto exit;
	}

	READ(fd,logfilebuf, sizeof(LOGFILE));

	status = log_check_insert(logfilebuf , logrec->opid, logtype);

	if (status == FALSE)
	{
		goto exit;
	}

	if (logrec->opid == LOG_DO_SPLIT)
	{
		
		char	cmd_str[TABLE_NAME_MAX_LEN];
		
		MEMSET(cmd_str, TABLE_NAME_MAX_LEN);
					
#ifdef MT_KFS_BACKEND
		int i = strmnstr(logrec->oldsstabname, "/", 
				STRLEN(logrec->oldsstabname));
		sprintf(cmd_str, "%s/%s", tss->rgbackpfile, 
				logrec->oldsstabname + i);

		if (COPYFILE(logrec->oldsstabname,cmd_str) != 0)
#else			
		sprintf(cmd_str, "cp %s %s", logrec->oldsstabname, 
				tss->rgbackpfile);
		
		if (system(cmd_str))
#endif
		{
			status = FALSE;
			goto exit;
		}
	}

	
	MEMCPY(&(logfilebuf->logrec[logfilebuf->logtotnum]), logrec, 
				sizeof(LOGREC));
	
	(logfilebuf->logtotnum)++;
	logfilebuf->logtype = logtype;

#ifdef MT_KFS_BACKEND
	CLOSE(fd);
	
	OPEN(fd, logfile_dir, (O_RDWR));

	if (fd < 0)
	{
		goto exit;
	}
#else
	LSEEK(fd, 0, SEEK_SET);
#endif
	WRITE(fd, logfilebuf, sizeof(LOGFILE));

exit:

	CLOSE(fd);	

	MEMFREEHEAP(logfilebuf);

	return status;
}



int
log_insert_insdel(LOGREC *logrec, char *rp, int rlen)
{

	LOCALTSS(tss);
	int		status;
	char		*logbuf;
	int		logbuflen;
	int		idx;


	status = 0;
	idx = 0;
	
	if (rp)
	{
		Assert(rlen > 0);
		logbuflen = sizeof(LOGREC) + sizeof(int) + rlen;
	}
	else
	{
		logbuflen = sizeof(LOGREC);
	}
	
	logbuf = (char *)MEMALLOCHEAP(logbuflen);
	
	if (rp)
	{
		*(int *)logbuf = rlen;
		idx += sizeof(int);
			
		PUT_TO_BUFFER(logbuf, idx, rp, rlen);
	}
	
	logrec->rowend_off = rlen;

	MEMCPY(logrec->logmagic, MT_LOG, LOG_MAGIC_LEN);

	logrec->loglen = logbuflen;

	PUT_TO_BUFFER(logbuf, idx, logrec, sizeof(LOGREC));

	Rg_loginfo->logoffset += logbuflen;

	if (Rg_loginfo->logoffset > SSTABLE_SIZE)
	{
		CLOSE(Rg_loginfo->logfd);
		
		int idxpos = str1nstr(Rg_loginfo->logdir, tss->rglogfile, 
					STRLEN(Rg_loginfo->logdir));

		int logfilenum = m_atoi(Rg_loginfo->logdir+ idxpos, 
					STRLEN(Rg_loginfo->logdir) - idxpos);
		
		logfilenum++;

		MEMSET(Rg_loginfo->logdir, STRLEN(Rg_loginfo->logdir));
		sprintf(Rg_loginfo->logdir, "%s%d", tss->rglogfile, logfilenum);

		OPEN(Rg_loginfo->logfd, Rg_loginfo->logdir, 
				(O_CREAT | O_APPEND | O_RDWR |O_TRUNC));

		Rg_loginfo->logoffset = 0;
	}

	if (Rg_loginfo->logoffset > SSTABLE_SIZE)
	{
		traceprint("The size of logfile (%d) is greater than the sstable size", Rg_loginfo->logoffset);
		goto exit;
	}
	
#ifdef MT_KFS_BACKEND

	status = APPEND(Rg_loginfo->logfd, logbuf, logbuflen);

#else

	APPEND(Rg_loginfo->logfd, logbuf, logbuflen, status);
#endif

exit:
	MEMFREEHEAP(logbuf);

	return status;
}


int
log_delete(LOGFILE *logfilebuf, int logtype)
{
	LOCALTSS(tss);
	int		status;


	status = FALSE;
	
	status = log_check_delete(logfilebuf, logtype, tss->rgbackpfile);

	if (status == FALSE)
	{
		goto exit;
	}

	logfilebuf->logtotnum = 0;
	logfilebuf->logtype = 0;

exit:

	return status;
}

int
log_undo_sstab_split(char *logfile_dir, char *backup_dir, int logtype,
			char *rgip, int rgport)
{
	int		fd;
	int		status;
	LOGFILE		*logfilebuf;

	
	logfilebuf = (LOGFILE *)MEMALLOCHEAP(sizeof(LOGFILE));
	status = FALSE;
	
	OPEN(fd, logfile_dir, (O_RDWR));

	if (fd < 0)
	{
		goto exit;
	}

	READ(fd,logfilebuf, sizeof(LOGFILE));

	if (logfilebuf->logtotnum == 0)
	{
		goto exit;
	}
	
	if (logfilebuf->logrec[logfilebuf->logtotnum - 1].opid == LOG_END)
	{
		status = log_check_delete(logfilebuf, logtype, backup_dir);

		if (status == FALSE)
		{
			goto exit;
		}
	}
	else
	{
		if (logfilebuf->logtype == SPLIT_LOG)
		{
			if (logfilebuf->logtotnum == 2)
			{
				Assert(logfilebuf->logrec[1].opid == LOG_DO_SPLIT);

				
				char	cmd_str[64];
				
				MEMSET(cmd_str, 64);

				LOGREC	*logrec = logfilebuf->logrec;


				char	tmpsstab[TABLE_NAME_MAX_LEN];

				MEMSET(tmpsstab, TABLE_NAME_MAX_LEN);
				int i = strmnstr(logrec[1].oldsstabname, "/", 
						STRLEN(logrec[1].oldsstabname));
		
				MEMCPY(tmpsstab, logrec[1].oldsstabname + i, 
						STRLEN(logrec[1].oldsstabname + i));

				
				char	new_sstab[TABLE_NAME_MAX_LEN];
				
				MEMSET(new_sstab, TABLE_NAME_MAX_LEN);
				MEMCPY(new_sstab, logrec[1].newsstabname,
						STRLEN(logrec[1].newsstabname));
				
				char	srcfile[TABLE_NAME_MAX_LEN];

				MEMSET(srcfile, TABLE_NAME_MAX_LEN);
				sprintf(srcfile, "%s/%s", backup_dir, tmpsstab);				
				

				char	tab_dir[TABLE_NAME_MAX_LEN];
				
				MEMSET(tab_dir, TABLE_NAME_MAX_LEN);
				MEMCPY(tab_dir, logrec[1].oldsstabname, i);			
										
				if (STAT(tab_dir, &st) != 0)
				{
					if (status < 0)
					{
						goto exit;
					}
				}
				
#ifdef MT_KFS_BACKEND
				i = strmnstr(srcfile, "/", STRLEN(srcfile));
				sprintf(cmd_str, "%s", tab_dir);
				sprintf(cmd_str, "%s", srcfile + i);
		
				if (COPYFILE(srcfile, cmd_str) != 0)
				{
					status = FALSE;
					goto exit;
				}

				int	rtn_stat;

				RMFILE(rtn_stat,srcfile);

				if (rtn_stat < 0)

				
#else			
				sprintf(cmd_str, "cp %s %s", srcfile, tab_dir);
				
				if (system(cmd_str))
				{
					status = FALSE;
					goto exit;
				}

				sprintf(cmd_str, "rm %s", srcfile);
				
				if (system(cmd_str))
#endif
				{
					status = FALSE;
					goto exit;
				}

				
				char	rgstate[TABLE_NAME_MAX_LEN];

				ri_get_rgstate(rgstate, rgip, rgport);
				
				ri_rgstat_deldata(rgstate, new_sstab);
			}
		}
	}
	
	logfilebuf->logtotnum = 0;
	logfilebuf->logtype = 0;

#ifdef MT_KFS_BACKEND
	CLOSE(fd);
	
	OPEN(fd, logfile_dir, (O_RDWR));

	if (fd < 0)
	{
		goto exit;
	}
#else

	LSEEK(fd, 0, SEEK_SET);
#endif
	WRITE(fd, logfilebuf, sizeof(LOGFILE));

exit:

	CLOSE(fd);	

	MEMFREEHEAP(logfilebuf);

	return status;
}


int
log_get_sstab_split_logfile(char *rglogfile, char *rgip, int rgport)
{
	MEMSET(rglogfile, 256);
	MEMCPY(rglogfile, LOG_FILE_DIR, STRLEN(LOG_FILE_DIR));

	char	rgname[64];
	
	MEMSET(rgname, 64);
	sprintf(rgname, "%s%d", rgip, rgport);

	str1_to_str2(rglogfile, '/', rgname);

	str1_to_str2(rglogfile, '/', "log");

	if (STAT(rglogfile, &st) != 0)
	{
		traceprint("Log file %s is not exist.\n", rglogfile);
		return FALSE;
	}

	return TRUE;
}

int
log_get_rgbackup(char *rgbackup, char *rgip, int rgport)
{
	MEMSET(rgbackup, 256);
	MEMCPY(rgbackup, BACKUP_DIR, STRLEN(BACKUP_DIR));

	
	char	rgname[64];
		
	MEMSET(rgname, 64);
	sprintf(rgname, "%s%d", rgip, rgport);

	str1_to_str2(rgbackup, '/', rgname);

	if (STAT(rgbackup, &st) != 0)
	{
		traceprint("Backup file %s is not exist.\n", rgbackup);
		return FALSE;
	}

	return TRUE;
}

int
log_get_latest_rginsedelfile(char *rginsdellogfile, char *rg_ip, int port)
{
	int	slen1;
	int	slen2;
	char	logdir[256];
	char	rgname[64];
	int	status;


	MEMSET(rginsdellogfile, 256);
	MEMSET(logdir, 256);

	
	MEMCPY(logdir, LOG_FILE_DIR, STRLEN(LOG_FILE_DIR));

	MEMSET(rgname, 64);
	sprintf(rgname, "%s%d", rg_ip, port);

	str1_to_str2(logdir, '/', rgname);
	
#ifdef MT_KFS_BACKEND
	
	MT_ENTRIES	mt_entries;

	MEMSET(&mt_entries, sizeof(MT_ENTRIES));

	if (!READDIR(logdir, (char *)&mt_entries))
	{
		traceprint("Read dir %s hit error.\n", logdir);
		return FALSE;
	}

	int i;

	for (i = 0; i < mt_entries.ent_num; i++)
	{
		if(strcmp(mt_entries.tabname[i],".")==0 || strcmp(mt_entries.tabname[i],"..")==0)
		{
			continue;
		}

		slen1 = STRLEN(mt_entries.tabname[i]);
		slen2 = STRLEN(rginsdellogfile);

		if (slen1 > slen2)
		{
			MEMCPY(rginsdellogfile, mt_entries.tabname[i], slen1);
		}
		else if (slen1 == slen2)
		{
			if (strcmp(mt_entries.tabname[i], rginsdellogfile))
			{
				MEMCPY(rginsdellogfile, mt_entries.tabname[i], slen1);
			}
		}	

		
		status = (slen2 == 0)? FALSE : TRUE;
	} 
	
#else
	DIR *pDir ;
	struct dirent *ent ;

	pDir=opendir(logdir);
	while((ent=readdir(pDir))!=NULL)
	{		
		if(strcmp(ent->d_name,".")==0 || strcmp(ent->d_name,"..")==0)
		{
			continue;
		}
		
		slen1 = STRLEN(ent->d_name);
		slen2 = STRLEN(rginsdellogfile);

		if (slen1 > slen2)
		{
			MEMCPY(rginsdellogfile, ent->d_name, slen1);
		}
		else if (slen1 == slen2)
		{
			if (strcmp(ent->d_name, rginsdellogfile))
			{
				MEMCPY(rginsdellogfile, ent->d_name, slen1);
			}
		}	

		
                status = (slen2 == 0)? FALSE : TRUE;
	}
#endif

	str1_to_str2(logdir, '/', rginsdellogfile);
	MEMCPY(rginsdellogfile, logdir, STRLEN(logdir));

	if (status == FALSE)
	{
		return FALSE;
	}
	
	return TRUE;
}


int
log_redo_insdel(char *insdellogfile, int scan_first)
{
	int		fd;
	int		status;
	char	 	*logfilebuf;
	int		offset;
	LOGREC		*logrec;
	char		*rp;
	int		rlen;
	
	
	logfilebuf = (char *)malloc(SSTABLE_SIZE);
	status = FALSE;
	memset(logfilebuf, 0, SSTABLE_SIZE);
	
	OPEN(fd, insdellogfile, (O_RDWR));

	if (fd < 0)
	{
		goto exit;
	}
	
	offset = READ(fd, logfilebuf, SSTABLE_SIZE);

	offset = log_get_last_logoffset((LOGREC *)logfilebuf);

	Assert(offset < SSTABLE_SIZE);

	int tmp = offset;
	int row_cnt = 0; 
	int log_scope_start = FALSE;

	if (tmp == 0)
	{
		traceprint("No log to be recovery in the log file %s.\n", insdellogfile);
		status = TRUE;
		goto exit;
	}
	
	
	while(tmp > 0)
	{
		
		logrec = (LOGREC *)(logfilebuf + tmp - sizeof(LOGREC));

		if (logrec->opid == CHECKPOINT_COMMIT)
		{
			Assert(log_scope_start == FALSE);

			log_scope_start = TRUE;

			tmp -= logrec->loglen;
			continue;
		}

		if ((logrec->opid == CHECKPOINT_BEGIN) && (log_scope_start))
		{
			status = TRUE;
			break;
		}

		tmp -= logrec->loglen;

		if (logrec->opid != CHECKPOINT_BEGIN)
		{
			row_cnt++;
		}
	}

	if (scan_first)
	{
		goto exit;
	}

	while (row_cnt > 0)
	{	
		
		if (strncasecmp(logrec->logmagic, MT_LOG, STRLEN(MT_LOG)) != 0)
		{
			
			rlen = *(int *)logrec;
			rp = (char *)logrec + sizeof(int);
			logrec = (LOGREC *)(rp + rlen);
		}
		
		if (   (logrec->opid == CHECKPOINT_BEGIN) 
		    || (logrec->opid == CHECKPOINT_COMMIT)
		   )
		{
			
			Assert(logrec->loglen == sizeof(LOGREC));
			logrec = (LOGREC *)((char *)logrec + sizeof(LOGREC));
			continue;
		}
		else if(logrec->opid == LOG_SKIP)
		{
			logrec = (LOGREC *)((char *)logrec + sizeof(LOGREC));
			continue;
		}
		else if ((logrec->opid == LOG_INSERT) || (logrec->opid == LOG_DELETE))
	   	{
		   	rp = (char *)logrec + sizeof(LOGREC) - logrec->loglen;
			rlen = *(int *)rp;
			rp += sizeof(int);
		}
		else
		{
			traceprint("The log recovery hit error.\n");
			Assert(0);
		}

		
		Assert(strncasecmp(logrec->logmagic, MT_LOG, STRLEN(MT_LOG)) == 0);

		log__redo_insdel(logrec, rp);

		row_cnt--;

		logrec = (LOGREC *)((char *)logrec + sizeof(LOGREC));
	}
	

exit:

	CLOSE(fd);	

	free(logfilebuf);

	return status;

}

static int
log__redo_insdel(LOGREC *logrec, char *rp)
{
	TABINFO		*tabinfo;
	BLK_ROWINFO	blk_rowinfo;
	char		*keycol;
	int		keycollen;
	int		status;

	BUF	*bp;
	int	offset;
	int	minlen;
	int	ign;
	int	rlen;
	int	i;
	int	*offtab;
	int	blk_stat;
	char	*tmprp;


	status = FALSE;
	bp = NULL;
	tabinfo = MEMALLOCHEAP(sizeof(TABINFO));
	MEMSET(tabinfo, sizeof(TABINFO));

	tabinfo->t_sinfo = (SINFO *)MEMALLOCHEAP(sizeof(SINFO));
	MEMSET(tabinfo->t_sinfo, sizeof(SINFO));

	tabinfo->t_rowinfo = &blk_rowinfo;
	MEMSET(tabinfo->t_rowinfo, sizeof(BLK_ROWINFO));

	tabinfo->t_dold = tabinfo->t_dnew = (BUF *) tabinfo;

	keycol = row_locate_col(rp, -1, logrec->minrowlen, &keycollen);

	TABINFO_INIT(tabinfo, logrec->oldsstabname, NULL, 0, tabinfo->t_sinfo,
			logrec->minrowlen, 0, logrec->tabid, logrec->sstabid);
	SRCH_INFO_INIT(tabinfo->t_sinfo, keycol, keycollen, 1, VARCHAR, -1); 		
	
	if (logrec->opid & LOG_INSERT)
	{
		minlen = tabinfo->t_row_minlen;
		
		tabinfo->t_sinfo->sistate |= SI_INS_DATA;
		
		bp = blkget(tabinfo);

		if (tabinfo->t_stat & TAB_RETRY_LOOKUP)
		{
			goto exit;
		}

		if (bp->bsstab->bblk->bsstab_insdel_ts_lo < logrec->oldts)
		{
			traceprint("Some logs has not been recovery.\n");
			goto exit;
		}
		else if (bp->bsstab->bblk->bsstab_insdel_ts_lo > logrec->oldts)
		{
			Assert(   (bp->bsstab->bblk->bsstab_insdel_ts_lo == logrec->newts)
			       || (bp->bsstab->bblk->bsstab_insdel_ts_lo > logrec->newts));

			status = TRUE;

			Assert(!(tabinfo->t_sinfo->sistate & SI_NODATA));
			goto exit;
		}

		

		Assert(tabinfo->t_sinfo->sistate & SI_NODATA);
		
		bufpredirty(bp);

	//	offset = blksrch(tabinfo, bp);

		Assert(tabinfo->t_rowinfo->rblknum == bp->bblk->bblkno);
		Assert(tabinfo->t_rowinfo->rsstabid == bp->bblk->bsstabid);
		offset = tabinfo->t_rowinfo->roffset;

		ign = 0;
		rlen = ROW_GET_LENGTH(rp, minlen);

		blk_stat = blk_check_sstab_space(tabinfo, bp, rp, rlen, offset);


		if (tabinfo->t_stat & TAB_SSTAB_SPLIT)
		{
			Assert(0);			
		}
		
		if (blk_stat & BLK_ROW_NEXT_BLK)
		{
			bp++;
		}

		
		if ((blk_stat & BLK_BUF_NEED_CHANGE))
		{
			offset = blksrch(tabinfo, bp);
		}

		if (bp->bblk->bfreeoff - offset)
		{
			
			offtab = ROW_OFFSET_PTR(bp->bblk);
			
			BACKMOVE((char *)bp->bblk + offset, (char *)bp->bblk + offset + rlen, 
					bp->bblk->bfreeoff - offset);

			
			for (i = bp->bblk->bnextrno; i > 0; i--)
			{
				if (offtab[-(i-1)] < offset)
				{
					break;
				}

				offtab[-i] = offtab[-(i-1)] + rlen;
			
			}
			offtab[-i] = offset;
		}

		bp->bsstab->bblk->bsstab_insdel_ts_lo = mtts_increment(bp->bsstab->bblk->bsstab_insdel_ts_lo);

		PUT_TO_BUFFER((char *)bp->bblk + offset, ign, rp, rlen);

		if (bp->bblk->bfreeoff == offset)
		{
			
			ROW_SET_OFFSET(bp->bblk, BLK_GET_NEXT_ROWNO(bp->bblk), offset);
		}
		


		bp->bblk->bfreeoff += rlen;

		bp->bblk->bminlen = minlen;
		
		BLK_GET_NEXT_ROWNO(bp->bblk)++;
		
		bufdirty(bp);
			
		tabinfo->t_sinfo->sistate &= ~SI_INS_DATA;
	}
	else if (logrec->opid & LOG_DELETE)
	{
		minlen = tabinfo->t_row_minlen;
		
		tabinfo->t_sinfo->sistate |= SI_DEL_DATA;
		
		bp = blkget(tabinfo);

		if (tabinfo->t_stat & TAB_RETRY_LOOKUP)
		{
			bufunkeep(bp->bsstab);
			return FALSE;
		}

		if (bp->bsstab->bblk->bsstab_insdel_ts_lo < logrec->oldts)
		{
			traceprint("Some logs has not been recovery.\n");
			goto exit;
		}
		else if (bp->bsstab->bblk->bsstab_insdel_ts_lo > logrec->oldts)
		{
			Assert(   (bp->bsstab->bblk->bsstab_insdel_ts_lo == logrec->newts)
			       || (bp->bsstab->bblk->bsstab_insdel_ts_lo > logrec->newts));

			status = TRUE;

			goto exit;
		}

		

		Assert(!(tabinfo->t_sinfo->sistate & SI_NODATA));
		

		bufpredirty(bp);

	//	offset = blksrch(tabinfo, bp);

		Assert(tabinfo->t_rowinfo->rblknum == bp->bblk->bblkno);
		Assert(tabinfo->t_rowinfo->rsstabid == bp->bblk->bsstabid);
		offset = tabinfo->t_rowinfo->roffset;

		if (tabinfo->t_sinfo->sistate & SI_NODATA)
		{
			traceprint("We can not find the row to be deleted.\n");	
			bufunkeep(bp->bsstab);
			return FALSE;
		}

		tmprp = (char *)(bp->bblk) + offset;
		rlen = ROW_GET_LENGTH(tmprp, minlen);

		if ((bp->bblk->bblkno == 0) && (offset == BLKHEADERSIZE))
		{
			ROW_SET_STATUS(tmprp, ROW_DELETED);
			goto delfinish;
		}
			
		if (bp->bblk->bfreeoff - offset)
		{
			MEMCPY(tmprp, tmprp + rlen, (bp->bblk->bfreeoff - offset - rlen));
		}

		offtab = ROW_OFFSET_PTR(bp->bblk);

		
		for (i = bp->bblk->bnextrno; i > 0; i--)
		{
					
			if (offtab[-(i-1)] < offset)
			{
				break;
			}
		}

		int j;
		for(j = i; j < (bp->bblk->bnextrno - 1); j++)
		{
			offtab[-j] = offtab[-(j + 1)] - rlen;
		
		}

		bp->bsstab->bblk->bsstab_insdel_ts_lo = mtts_increment(bp->bsstab->bblk->bsstab_insdel_ts_lo);
		
		bp->bblk->bfreeoff -= rlen;
		
		BLK_GET_NEXT_ROWNO(bp->bblk)--;

delfinish:
		bufdirty(bp);
			
		tabinfo->t_sinfo->sistate &= ~SI_DEL_DATA;
		
	}


	status = TRUE;
exit:
	if (bp)
	{
		bufunkeep(bp->bsstab);
	}
	
	session_close(tabinfo);

	if (tabinfo!= NULL)
	{
		MEMFREEHEAP(tabinfo->t_sinfo);

		if (tabinfo->t_insrg)
		{
			Assert(0);

		}
		
		MEMFREEHEAP(tabinfo);
//		tss->ttabinfo = NULL;
	}
	
	

	return status;

}

int
log_recov_rg(char *rgip, int rgport)
{
	char	logfile[TABLE_NAME_MAX_LEN];
	char	backup[TABLE_NAME_MAX_LEN];

	log_get_sstab_split_logfile(logfile, rgip, rgport);
	log_get_rgbackup(backup, rgip, rgport);

	log_undo_sstab_split(logfile, backup, SPLIT_LOG, rgip, rgport);

	if (!log_get_latest_rginsedelfile(logfile, rgip, rgport))
	{
		return TRUE;
	}

	if (!log_redo_insdel(logfile, TRUE))
	{	
		char prelogfile[TABLE_NAME_MAX_LEN];
		
		int idxpos = str1nstr(logfile, "/log\0", STRLEN(logfile));
	
		int logfilenum = m_atoi(logfile + idxpos, 
					STRLEN(logfile) - idxpos);

		if (logfilenum > 0)
		{				
			logfilenum--;

			MEMSET(prelogfile, STRLEN(prelogfile));
			MEMCPY(prelogfile, logfile, idxpos);
			sprintf(prelogfile + idxpos, "%d", logfilenum);

			
			log_redo_insdel(prelogfile, FALSE);
		}
	}

	log_redo_insdel(logfile, FALSE);	

	return TRUE;
}

static int
log_get_last_logoffset(LOGREC *logrec)
{
	int	rlen;
	char	*rp;
	int	logoffset;
	char	*tmp;


	logoffset = 0;
	tmp = (char *)logrec + SSTABLE_SIZE;
	
	while (logoffset < SSTABLE_SIZE)
	{	
		
		if (strncasecmp(logrec->logmagic, MT_LOG, STRLEN(MT_LOG)) != 0)
		{
			
			rlen = *(int *)logrec;
			
			rp = (char *)logrec + sizeof(int);
			logrec = (LOGREC *)(rp + rlen);

			if ((char *)logrec > tmp)
			{
				
				break;
			}
		}
		
		if (   (logrec->opid == CHECKPOINT_BEGIN) 
		    || (logrec->opid == CHECKPOINT_COMMIT)
		   )
		{
			Assert(logrec->loglen == sizeof(LOGREC));

			logoffset += logrec->loglen;

			//traceprint("logoffset -- %d, logrec->opid  -- %d\n", logoffset, logrec->opid );
			
			logrec = (LOGREC *)((char *)logrec + sizeof(LOGREC));
			
			continue;
		}
		else if(logrec->opid == LOG_SKIP)
		{
			logoffset += logrec->loglen;

			//traceprint("logoffset -- %d, logrec->opid  -- %d\n", logoffset, logrec->opid );
			
			logrec = (LOGREC *)((char *)logrec + sizeof(LOGREC));
			continue;
		}

		
		if(strncasecmp(logrec->logmagic, MT_LOG, STRLEN(MT_LOG)) == 0)
		{
			logoffset += logrec->loglen;
			//traceprint("logoffset -- %d, logrec->opid  -- %d\n", logoffset, logrec->opid );
		}
		else
		{
			break;
		}
		

		logrec = (LOGREC *)((char *)logrec + sizeof(LOGREC));
	}

	return logoffset;
}

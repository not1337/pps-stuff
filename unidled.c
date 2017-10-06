/*
 * unidled - a linux cpu idle management daemon for gpsd with gps pps input
 *
 * Copyright (c) 2017 Andreas Steinmetz (ast@domdv.de)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Compile and link:
 *
 * gcc -Wall -O3 -s -o unidled unidled.c -lrt
 *
 * Using gpsd (gps with pps attached) and unidled in combination with chronyd
 * (SOCK refclock) results in a average input deviation well below 1us according
 * to chronyc while keeping the CPU cool (e.g. 100-250ns on a i7-7500U, refclock
 * configured with "poll 5 filter 80" and then "corrtimeratio 3.2").
 *
 * The default is for systems using the intel_idle driver, the "-a" option
 * uses the generic idle management method and should work for all other
 * systems.
 *
 * For a list of all options, run "unidled -h".
 *
 * When using unidled, assert that all of gpsd, chronyd and unidled are set
 * to run on the same core and with realtime privilege (unidled highest,
 * followed by gpsd and then chronyd whith lowest privilege). Make sure that
 * the pps serial interrupt is served by the same core, use a script to
 * irqbalance to assert this, if required.
 *
 * When using the chronyd SOCK refclock the daemon start sequence is first
 * chronyd, then gpsd (requires chronyd socket) and finally unidled (requires
 * pps device created by gpsd).
 */

#define _GNU_SOURCE
#include <linux/types.h>
#include <linux/pps.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#ifdef __GNUC__
#define LIKELY(a)	__builtin_expect((a),1)
#define UNLIKELY(a)	__builtin_expect((a),0)
#define HOT		__attribute__((hot))
#define COLD		__attribute__((cold))
#define NORETURN	__attribute__((noreturn))
#else
#define __attribute__(x)
#define LIKELY(a)	a
#define UNLIKELY(a)	a
#define HOT
#define COLD
#define NORETURN
#endif

#define PPSCAPS (PPS_CAPTUREBOTH|PPS_CANWAIT)

struct common
{
	int state;
	int max;
	int high;
	int first;
	int poh;
	int pof;
	int prf;
	int prh;
	int prl;
	int prio;
	int cpu;
	int thres;
	int fg;
	int all;
	char *dev;
	char *pid;
	timer_t id;
	struct itimerspec it;
};

static int doterm;
static char idlelist[32][64];
static int idlefd[32];

static COLD int getlimit(int cpu,int max,int thres,int *high)
{
	int i;
	int fd;
	long val;
	char *end;
	char bfr[64];

	if(cpu<0||cpu>=1024)return -1;

	for(*high=0,i=0;i<max;i++)
	{
		sprintf(bfr,
			"/sys/devices/system/cpu/cpu%d/cpuidle/state%d/latency",
			cpu,i);
		if((fd=open(bfr,O_RDONLY|O_CLOEXEC))==-1)break;
		val=read(fd,bfr,sizeof(bfr)-1);
		close(fd);
		if(val<1)break;
		bfr[val]=0;
		val=strtol(bfr,&end,10);
		if(end==bfr||(*end&&*end!='\n'))break;
		*high=i;
		if(val>thres)break;
	}
	if(!*high)return -1;
	return 0;
}

static COLD int buildlist(int cpu,int *max)
{
	int i;
	struct stat stb;

	if(cpu<0||cpu>=1024)return -1;

	for(*max=0,i=0;i<32;i++)sprintf(idlelist[i],
		"/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable",cpu,i);
	for(i=0;i<32;i++)if(!stat(idlelist[i],&stb)&&S_ISREG(stb.st_mode))
		*max=i+1;
	else break;
	if(!*max)return -1;
	return 0;
}

static COLD int openidle(int max)
{
	int i;

	if(!max)
	{
		if((idlefd[0]=open("/dev/cpu_dma_latency",
			O_WRONLY|O_NONBLOCK|O_CLOEXEC))==-1)return -1;
	}
	else for(i=0;i<max;i++)
	    if((idlefd[i]=open(idlelist[i],O_WRONLY|O_NONBLOCK|O_CLOEXEC))==-1)
	{
		while(--i>=0)close(idlefd[i]);
		return -1;
	}
	return 0;
}

static COLD void closeidle(int max)
{
	int i;

	if(!max)close(idlefd[0]);
	for(i=0;i<max;i++)close(idlefd[i]);
}

static COLD int setcpu(int cpu)
{
	cpu_set_t set;

	if(cpu<0||cpu>=1024)return -1;

	CPU_ZERO(&set);
	CPU_SET(cpu,&set);
	if(sched_setaffinity(0,sizeof(set),&set))return -1;
	return 0;
}

static COLD int setprio(int prio)
{
	struct sched_param param;

	if(prio<1||prio>99)return -1;

	memset(&param,0,sizeof(param));
	param.sched_priority=prio;
	if(sched_setscheduler(0,SCHED_RR,&param))return -1;
	return 0;
}

static COLD int openpps(char *dev)
{
	int r=-1;
	int fd;
	int l;
	DIR *d;
	struct dirent *e;
	struct stat stb;
	struct pps_kparams prm;
	char bfr[1024];

	if(!dev||!*dev)return -1;

	if(!(d=opendir("/sys/class/pps")))return -1;
	while((e=readdir(d)))if(!strncmp(e->d_name,"pps",3))
	{
		sprintf(bfr,"/sys/class/pps/%s/path",e->d_name);
		if(!stat(bfr,&stb)&&S_ISREG(stb.st_mode))
			if((fd=open(bfr,O_RDONLY|O_CLOEXEC))!=-1)
		{
			if((l=read(fd,bfr,sizeof(bfr)-1))>0)
			{
				if(bfr[l-1]=='\n')l--;
				bfr[l]=0;
			}
			else bfr[0]=0;
			close(fd);
			if(!strcmp(bfr,dev))
			{
				sprintf(bfr,"/dev/%s",e->d_name);
				if((r=open(bfr,O_RDWR|O_CLOEXEC))!=-1)
				{
					if(ioctl(r,PPS_GETCAP,&l))goto fail;
					if((l&PPSCAPS)!=PPSCAPS)goto fail;
					if(ioctl(r,PPS_GETPARAMS,&prm))goto fail;
					if(prm.api_version!=PPS_API_VERS)
						goto fail;
					prm.mode|=PPS_CAPTUREBOTH;
					prm.mode&=
					    ~(PPS_OFFSETASSERT|PPS_OFFSETCLEAR);
					memset(&prm.assert_off_tu,0,
						sizeof(prm.assert_off_tu));
					memset(&prm.clear_off_tu,0,
						sizeof(prm.clear_off_tu));
					if(ioctl(r,PPS_SETPARAMS,&prm))goto fail;
				}
				if(0)
				{
fail:					close(r);
					r=-1;
				}
				break;
			}
		}
	}
	closedir(d);

	return r;
}

static COLD void term(int unused)
{
	doterm=1;
}

static COLD void setsigs(void)
{
	sigset_t set;

	sigfillset(&set);
	sigdelset(&set,SIGINT);
	sigdelset(&set,SIGTERM);
	sigdelset(&set,SIGHUP);
	sigdelset(&set,SIGQUIT);
	sigprocmask(SIG_BLOCK,&set,NULL);
	signal(SIGINT,term);
	signal(SIGTERM,term);
	signal(SIGHUP,term);
	signal(SIGQUIT,term);
}

static NORETURN COLD void usage(void)
{
	fprintf(stderr,
	"Usage: unidled -d <device> [options]\n"
	"       unidled -h\n\n"
	"-d <device> is the serial device the pps signal is attached to.\n"
	"-h displays this help text.\n\n"
	"Options are:\n\n"
	"-c <core>	the core to be used (0-1023)\n"
	"-r <prio>	the realtime priority (1-99)\n"
	"-t <latency>	the lower latency threshold (1-1000, 50us default)\n"
	"-P <millisecs>	the post pps pulse poll mode time (1-1000, 1 default)\n"
	"-p <millisecs>	the pre pps pulse poll mode time (0-1000, 1 default)\n"
	"-l <millisecs>	the pre poll mode lower latency time"
	" (0-1000, 0 default)\n"
	"-L <millisecs>	the pre poll mode lower latency time"
	" (0-1000, 0 default)\n"
	"-a		modify all cores instead of single core\n"
	"-f <pidfile>	the pid file (default /run/unidled.pid)\n"
	"-n		don't daemonize\n");
	exit(1);
}

static COLD void parse(int argc,char *argv[],struct common *c)
{
	int x;
	long v;
	char *end;
	struct stat stb;

	c->first=1;
	c->state=0;
	c->prio=1;
	c->cpu=0;
	c->thres=50;
	c->pof=1;
	c->poh=0;
	c->prh=0;
	c->prf=1;
	c->fg=0;
	c->all=0;
	c->dev=NULL;
	c->pid="/run/unidled.pid";

	while((x=getopt(argc,argv,"c:r:d:t:P:p:L:l:f:nah"))!=-1)switch(x)
	{
	case 'c':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<0||v>1023)usage();
		c->cpu=(int)v;
		break;

	case 'r':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<1||v>99)usage();
		c->prio=(int)v;
		break;

	case 'd':
		if(!*optarg||stat(optarg,&stb)||!S_ISCHR(stb.st_mode))usage();
		c->dev=optarg;
		break;

	case 't':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<1||v>1000)usage();
		c->thres=(int)v;
		break;

	case 'P':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<1||v>1000)usage();
		c->pof=(int)v;
		break;

	case 'p':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<0||v>1000)usage();
		c->prf=(int)v;
		break;

	case 'l':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<0||v>1000)usage();
		c->prh=(int)v;
		break;

	case 'L':
		v=strtol(optarg,&end,10);
		if(optarg==end||*end||v<0||v>1000)usage();
		c->poh=(int)v;
		break;

	case 'f':
		if(!*optarg)usage();
		c->pid=optarg;
		break;

	case 'n':
		c->fg=1;
		break;

	case 'a':
		c->all=1;
		break;

	default:usage();
	}

	if(!c->dev||c->poh+c->pof+c->prf+c->prh>1000)usage();

	c->poh*=1000000;
	c->pof*=1000000;
	c->prf*=1000000;
	c->prh*=1000000;
	c->prl=1000000000-c->pof-c->poh-c->prh-c->prf;
	memset(&c->it,0,sizeof(c->it));
}

static HOT int modify(int base,int total,int mode)
{	       
	for(;base<total;base++)
	{
		if(UNLIKELY(write(idlefd[base],mode?"1\n":"0\n",2)!=2))
			return -1;
	}
	return 0;
}	       

static HOT int idleset(int val)
{
	if(UNLIKELY(write(idlefd[0],&val,sizeof(val))!=sizeof(val)))return -1;
	return 0;
}

static HOT void timer(union sigval param)
{
	struct common *c=param.sival_ptr;

	if(LIKELY(!c->first))switch(c->state++)
	{
	case 0:	if(!c->poh)c->state++;
		else
		{
			c->it.it_value.tv_nsec=c->poh;
			timer_settime(c->id,0,&c->it,NULL);
			if(c->all)idleset(c->thres);
			else modify(1,c->high,0);
			break;
		}

	case 1:	if(!c->prl)c->state++;
		else
		{
			c->it.it_value.tv_nsec=c->prl;
			timer_settime(c->id,0,&c->it,NULL);
			if(c->all)idleset(-1);
			else modify(c->poh?c->high:1,c->max,0);
			break;
		}

	case 2:	if(!c->prh)c->state++;
		else
		{
			c->it.it_value.tv_nsec=c->prh;
			timer_settime(c->id,0,&c->it,NULL);
			if(c->all)idleset(c->thres);
			else modify(c->high,c->max,1);
			break;
		}

	case 3:	if(c->prf)
		{
			c->it.it_value.tv_nsec=c->prf;
			timer_settime(c->id,0,&c->it,NULL);
			if(c->all)idleset(0);
			else modify(1,c->prh?c->high:c->max,1);
		}
		break;
	}
}

static COLD int prepare(struct common *c)
{
	int i;
	int fd;
	struct sigevent sev;
	FILE *fp;

	setsigs();

	if(UNLIKELY(mlockall(MCL_CURRENT|MCL_FUTURE)))
	{
		perror("mlockall");
		return -1;
	}

	if(UNLIKELY(setcpu(c->cpu)))
	{
		fprintf(stderr,"Unable to set affinity to selected core\n");
		return -1;
	}

	if(UNLIKELY(setprio(c->prio)))
	{
		fprintf(stderr,"Unable to set realtime priority\n");
		return -1;
	}

	memset(&sev,0,sizeof(sev));
	sev.sigev_notify=SIGEV_THREAD;
	sev.sigev_value.sival_ptr=c;
 	sev.sigev_notify_function=timer;

	if(c->all)c->max=0;
	else
	{
		if(UNLIKELY(buildlist(c->cpu,&c->max)))
		{
			fprintf(stderr,"Unable to collect power states\n");
			return -1;
		}

		if(c->prf||c->pof)
			if(UNLIKELY(getlimit(c->cpu,c->max,c->thres,&c->high)))
		{
			fprintf(stderr,"Unable to get intermediate threshold\n");
			return -1;
		}
	}

	if(UNLIKELY(openidle(c->max)))
	{
		fprintf(stderr,"Unable to access idle controls\n");
		return -1;
	}

	i=0;
repeat:	if((fd=openpps(c->dev))==-1)
	{
		if(i++<80)
		{
			usleep(25000);
			goto repeat;
		}

		fprintf(stderr,"Unable to access pps device for %s\n",c->dev);
		return -1;
	}

	if(LIKELY(!c->fg))
	{
		if(UNLIKELY(daemon(0,0)))
		{
			perror("daemon");
			return -1;
		}

		if(LIKELY((fp=fopen(c->pid,"we"))!=NULL))
		{
			fprintf(fp,"%d\n",getpid());
			fclose(fp);
		}
	}

	if(UNLIKELY(timer_create(CLOCK_MONOTONIC,&sev,&c->id)))
	{
		perror("timer_create");
		return -1;
	}

	return fd;
}

HOT int main(int argc,char *argv[])
{
	int ppsfd;
	long delta;
	long nsec;
	struct common c;
	struct pps_fdata data;

	doterm=0;

	memset(&data,0,sizeof(data));
	data.timeout.sec=1;
	data.timeout.nsec=100000000;

	parse(argc,argv,&c);
	if((ppsfd=prepare(&c))==-1)return 1;

	while(LIKELY(!doterm))
	{
		if(UNLIKELY(c.first==1))
		{
			if(c.all)idleset(-1);
			else modify(1,c.max,0);
			c.first=2;
		}

repeat:		if(UNLIKELY(ioctl(ppsfd,PPS_FETCH,&data)==-1))switch(errno)
		{
		case ETIMEDOUT:
			if(!c.first)c.first=1;
			continue;
		case EINTR:
			goto out;
		default:goto repeat;
		}

		if(UNLIKELY(c.first))
		{
			c.first=0;
			c.state=0;
			c.it.it_value.tv_nsec=0;
			timer_settime(c.id,0,&c.it,NULL);
			continue;
		}

		if(UNLIKELY(!data.info.clear_sequence)&&
		    UNLIKELY(!data.info.clear_tu.sec)&&!data.info.clear_tu.nsec)
		{
			delta=600000000;
			nsec=data.info.assert_tu.nsec;
		}
		else if(data.info.assert_tu.sec>data.info.clear_tu.sec)
		{
			nsec=data.info.assert_tu.nsec;
			delta=data.info.assert_tu.sec-data.info.clear_tu.sec;
			if(data.info.assert_tu.nsec<data.info.clear_tu.nsec)
			{
				delta--;
				data.info.assert_tu.nsec+=1000000000;
			}
			delta+=data.info.assert_tu.nsec-data.info.clear_tu.nsec;
		}
		else if(data.info.assert_tu.sec<data.info.clear_tu.sec)
		{
			nsec=data.info.clear_tu.nsec;
			delta=data.info.clear_tu.sec-data.info.assert_tu.sec;
			if(data.info.clear_tu.nsec<data.info.assert_tu.nsec)
			{
				delta--;
				data.info.clear_tu.nsec+=1000000000;
			}
			delta+=data.info.clear_tu.nsec-data.info.assert_tu.nsec;
		}
		else if(data.info.assert_tu.nsec>data.info.clear_tu.nsec)
		{
			delta=data.info.assert_tu.nsec-data.info.clear_tu.nsec;
			nsec=data.info.assert_tu.nsec;
		}
		else if(data.info.assert_tu.nsec<data.info.clear_tu.nsec)
		{
			delta=data.info.clear_tu.nsec-data.info.assert_tu.nsec;
			nsec=data.info.clear_tu.nsec;
		}
		else
		{
			if(!c.first)c.first=1;
			continue;
		}

		if(delta<600000000)continue;

		if(nsec>=500000000)
		{
			nsec-=1000000000;
			if(UNLIKELY(nsec<=-1000000))nsec=-999999;
		}
		else if(UNLIKELY(nsec>=1000000))nsec=999999;

		c.state=0;
		c.it.it_value.tv_nsec=c.pof-nsec;
		timer_settime(c.id,0,&c.it,NULL);
	}

out:	c.it.it_value.tv_nsec=0;
	timer_settime(c.id,0,&c.it,NULL);
	timer_delete(c.id);

	if(c.all)idleset(-1);
	else modify(1,c.max,0);

	closeidle(c.max);
	close(ppsfd);

	if(LIKELY(!c.fg))unlink(c.pid);

	return 0;
}

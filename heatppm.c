/*
 * heatppm - a linux processor heater with chronyd frequency offset output
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
 * gcc -Wall -O3 -s -o heatppm heatppm.c -lpthread
 *      
 * For a list of all options, run "heatppm -h".
 *
 * Keep the system heatppm is to run on as idle as possible whith the exception
 * of chronyd, gpsd and unidled and assert that the system has cooled down
 * before starting heatppm. Running heatppm can take many hours, so be patient.
 */

#include <pthread.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sched.h>
#include <poll.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int hfd;
static int cfd;
static long on;
static pthread_mutex_t mtx=PTHREAD_MUTEX_INITIALIZER;

static void *pwm(void *unused)
{
	int x;
	int pfd;
	long val=0;
	uint64_t dummy;
	struct pollfd p[2];
	struct itimerspec it;

	if((pfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC))==-1)
	{
		perror("timerfd_create");
		exit(1);
	}

	p[0].fd=hfd;
	p[0].events=POLLIN;
	p[1].fd=pfd;
	p[1].events=POLLIN;
	memset(&it,0,sizeof(it));

	while(1)
	{
		switch(poll(p,2,-1))
		{
		case -1:perror("poll");
			exit(1);

		case 0:	continue;
		}

		if(p[0].revents&POLLIN)
			if(read(hfd,&dummy,sizeof(dummy))==sizeof(dummy))
		{
			it.it_value.tv_nsec=0;
			if(timerfd_settime(pfd,0,&it,NULL))
			{
				perror("timerfd_settime");
				exit(1);
			}
			x=read(pfd,&dummy,sizeof(dummy));

			pthread_mutex_lock(&mtx);
			val=on;
			pthread_mutex_unlock(&mtx);
		}

		if(p[1].revents&POLLIN)
			if(read(pfd,&dummy,sizeof(dummy))==sizeof(dummy))
		{
			x=-1;
			x=write(cfd,&x,sizeof(x));
			it.it_value.tv_nsec=0;
			if(timerfd_settime(pfd,0,&it,NULL))
			{
				perror("timerfd_settime");
				exit(1);
			}
			x=read(pfd,&dummy,sizeof(dummy));
			continue;
		}

		switch(val)
		{
		case 0L:x=-1;
			x=write(cfd,&x,sizeof(x));
			it.it_value.tv_nsec=0;
			if(timerfd_settime(pfd,0,&it,NULL))
			{
				perror("timerfd_settime");
				exit(1);
			}
			x=read(pfd,&dummy,sizeof(dummy));
			break;

		case 1000000000L:
			x=0;
			x=write(cfd,&x,sizeof(x));
			it.it_value.tv_nsec=0;
			if(timerfd_settime(pfd,0,&it,NULL))
			{
				perror("timerfd_settime");
				exit(1);
			}
			x=read(pfd,&dummy,sizeof(dummy));
			break;

		default:x=0;
			x=write(cfd,&x,sizeof(x));
			it.it_value.tv_nsec=val;
			if(timerfd_settime(pfd,0,&it,NULL))
			{
				perror("timerfd_settime");
				exit(1);
			}
			break;
		}
	}

	pthread_exit(NULL);
}

static int tracer(double *time,double *freq,double *res,double *skew)
{
	FILE *fp;
	char *t;
	char *f;
	char *r;
	char *s;
	char *mem;
	char line[256];

	if(!(fp=popen("chronyc -c tracking","re")))return -1;
	if(!fgets(line,sizeof(line),fp))
	{
		pclose(fp);
		return -1;
	}
	pclose(fp);

	strtok_r(line,",\n",&mem);
	strtok_r(NULL,",\n",&mem);
	strtok_r(NULL,",\n",&mem);
	t=strtok_r(NULL,",\n",&mem);
	strtok_r(NULL,",\n",&mem);
	strtok_r(NULL,",\n",&mem);
	strtok_r(NULL,",\n",&mem);
	f=strtok_r(NULL,",\n",&mem);
	r=strtok_r(NULL,",\n",&mem);
	s=strtok_r(NULL,",\n",&mem);

	if(!t||!*t||!f||!*f||!r||!*r||!s||!*s)return -1;

	*time=strtod(t,&mem);
	if(*mem)return -1;
	*freq=strtod(f,&mem);
	if(*mem)return -1;
	*res=strtod(r,&mem);
	if(*mem)return -1;
	*skew=strtod(s,&mem);
	if(*mem)return -1;

	return 0;
}

static int temp(char *fn,double *temp)
{
	FILE *fp;
	char *t;
	char *mem;
	char line[256];

	if(!(fp=fopen(fn,"re")))return -1;
	if(!fgets(line,sizeof(line),fp))
	{
		fclose(fp);
		return -1;
	}
	fclose(fp);

	t=strtok_r(line,",\n",&mem);

	if(!t||!*t)return -1;

	*temp=strtod(t,&mem);
	if(*mem)return -1;

	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: heatppm [options] -t temperature-source\n"
		"       heatppm -h\n\n"
		"temperature-source is any of (make a sensible choice):\n"
		"/sys/class/hwmon/hwmon*/temp*_input\n\n"
		"Options are:\n"
		"-h	display this help\n"
		"-w wait	amount of updates from chronyc before "
			"validation (4-16, default 5)\n"
		"-l temp	maximum temperature to test for (30-99, "
			"default 85)\n"
		"-m skew	minimum skew required in ppb (1-100, "
			"default 15)\n"
		"-r	relaxed temperature acceptance (+/-0.25°C instead of "
			"exact value)\n");
	exit(1);
}

int main(int argc,char *argv[])
{
	int i;
	int x;
	int tfd;
	int inited=-8;
	int idx=0;
	int xact=1;
	int nohit=0;
	int nl=0;
	int wait=5;
	unsigned int base=0;
	unsigned int ticks=0;
	long delta;
	long pulse=0;
	uint64_t dummy;
	char *info;
	char *tempsrc=NULL;
	double minskew=0.015;
	double target=0;
	double high=85000;
	double avg;
	double deg;
	double prev=0;
	double curr;
	double freq;
	double res;
	double skew;
	double deglst[8];
	struct pollfd p;
	struct sched_param sched;
	struct timeval tv;
	struct itimerspec it;
	pthread_t h;

	while((x=getopt(argc,argv,"t:w:l:m:rh"))!=-1)switch(x)
	{
	case 't':
		tempsrc=optarg;
		break;

	case 'w':
		wait=atoi(optarg);
		if(wait<4||wait>16)usage();

	case 'l':
		high=(double)atoi(optarg);
		if(high<30||high>99)usage();
		high*=1000;
		break;

	case 'm':
		minskew=(double)atoi(optarg);
		if(minskew<1||minskew>100)usage();
		minskew/=1000;
		break;

	case 'r':
		xact=0;
		break;

	case 'h':
	default:usage();
	}

	if(!tempsrc)usage();

	memset(&sched,0,sizeof(sched));
	sched.sched_priority=sched_get_priority_max(SCHED_RR);
	if(sched_setscheduler(0,SCHED_RR,&sched))
	{
		perror("sched_setscheduler");
		return 1;
	}

	memset(&it,0,sizeof(it));
	it.it_interval.tv_sec=1;
	it.it_value.tv_sec=1;

	if((tfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC))==-1)
	{
		perror("timerfd_create");
		return 1;
	}

	if((hfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC))==-1)
	{
		perror("timerfd_create");
		return 1;
	}

	if((cfd=open("/dev/cpu_dma_latency",O_WRONLY|O_NONBLOCK|O_CLOEXEC))==-1)
	{
		perror("open");
		return 1;
	}

	p.fd=tfd;
	p.events=POLLIN;
	on=0;

	if(pthread_create(&h,NULL,pwm,NULL))
	{
		perror("pthread_create");
		return 1;
	}

	printf("\rWait...");
	fflush(stdout);
	nl=1;

	while(1)
	{
		gettimeofday(&tv,NULL);
		if(tv.tv_usec>=450000&&tv.tv_usec<=550000)break;
		usleep(5000);
	}

	if(timerfd_settime(tfd,0,&it,NULL))
	{
		perror("timerfd_settime");
		return 1;
	}

	if(timerfd_settime(hfd,0,&it,NULL))
	{
		perror("timerfd_settime");
		return 1;
	}

	printf("\rInitializing...");
	fflush(stdout);
	nl=1;

	while(1)
	{
		switch(poll(&p,1,-1))
		{
		case -1:perror("poll");
			return 1;

		case 0:	continue;

		case 1:	if(read(tfd,&dummy,sizeof(dummy))!=sizeof(dummy))
				continue;
			break;
		}

		if(temp(tempsrc,&deg))
		{
			fprintf(stderr,"can't read temperature data\n");
			return 1;
		}

		if(tracer(&curr,&freq,&res,&skew))
		{
			fprintf(stderr,"can't get chrony tracking data\n");
			return 1;
		}

		if(curr!=prev)
		{
			ticks++;
			prev=curr;
		}

		deglst[idx++]=deg;
		idx&=7;
		if(inited<0)
		{
			inited++;
			continue;
		}

		if(!inited)
		{
			for(i=0,avg=0;i<8;i++)avg+=deglst[i];
			avg/=8;
			for(i=0,x=0;i<8;i++)
				if(deglst[i]+1000<avg||deglst[i]-1000>avg)
			{
				x=1;
				break;
			}

			printf("\r%3.3f [------]  -  % 4.3f % 4.3f % 3.3f",
				avg/1000,freq,res,skew);
			fflush(stdout);
			nl=1;

			if(!x&&!res&&skew<=minskew)
			{
				target=(double)(((int)(avg+999))/1000)*1000;
				inited=1;
				base=ticks;
				nohit=0;

				if(avg==target)
				{
					printf("\r%.0f %.3f"
						"          "
						"          "
						"          "
						"          "
						"          "
						"          "
						"\n",avg,freq);
					target+=1000;
					nl=0;
				}
				if(target>high)break;
			}
			else if(++nohit>=3600)break;
			else continue;
		}

		for(i=0,avg=0;i<8;i++)avg+=deglst[i];
		avg/=8;

		if(avg<target)
		{
			if(target-avg>500)
			{
				delta=1000000L;
				info=">>>";
			}
			else if(target-avg>250)
			{
				delta=500000L;
				info=" >>";
			}
			else
			{
				delta=100000L;
				info=" > ";
			}
			if(pulse+delta>1000000000L)
			{
				pulse=1000000000L;
				info=" >|";
			}
			else pulse+=delta;
		}
		else if(avg>target)
		{
			if(avg-target>500)
			{
				delta=1000000L;
				info="<<<";
			}
			else if(avg-target>250)
			{
				delta=500000L;
				info="<< ";
			}
			else
			{
				delta=100000L;
				info=" < ";
			}
			if(pulse-delta<0L)
			{
				pulse=0L;
				info="|< ";
			}
			else pulse-=delta;
		}
		else
		{
			delta=0L;
			info=" - ";
		}

		pthread_mutex_lock(&mtx);
		on=pulse;
		pthread_mutex_unlock(&mtx);

		printf("\r%3.3f [%3.3f] %s % 4.3f % 4.3f % 3.3f",
			avg/1000,target/1000,info,freq,res,skew);
		fflush(stdout);
		nl=1;

		if((xact&&delta)||delta>100000L||ticks-base<wait||res||
			skew>minskew)
		{
			if(++nohit>=3600)break;
			continue;
		}

		printf("\r%.0f %.3f"
			"          "
			"          "
			"          "
			"          "
			"          "
			"          "
			"\n",avg,freq);
		target+=1000;
		nl=0;
		base=ticks;
		nohit=0;
		if(target>high)break;
	}

	if(nl)printf("\r"
		"          "
		"          "
		"          "
		"          "
		"          "
		"          "
		"          \r");
	fflush(stdout);

	return 0;
}

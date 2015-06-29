/* 
        Program:     super shell
        Author:      S <super@innu.org>
        Description: A simple shell that includes background 
                     processing and history features.
*/

#include<stdio.h>
#include<stdlib.h>
#include<stdarg.h>
#include<string.h>
#include<sys/types.h>
#include<unistd.h>
#include<sys/wait.h>
#include<signal.h>
#include<errno.h>
#include<ctype.h>

/* 
	Output an error message and fail.

	Postcondition: program exit
*/

static void shfail(char*f)
{
        perror(f);
        exit(EXIT_FAILURE);
}

/*
	Output an fault message.
*/

static void shfault(char*fmt, ...)
{
	va_list ap;

	va_start(ap,fmt);
	fputs("supersh: ",stderr);
	vfprintf(stderr,fmt,ap);
	fputc('\n',stderr);
	va_end(ap);
}


/*
	Print program exit status information.
*/

static void wait_handler(int stat_loc)
{
        if(WIFSIGNALED(stat_loc))
        {
                char*signame;

                switch(WTERMSIG(stat_loc))
                {
                        case SIGABRT:
                                signame="Aborted";
                                break;
                        case SIGFPE:
                                signame="Floating Point Exception";
                                break;
                        case SIGILL:
                                signame="Illegal Instruction";
                                break;
                        case SIGINT:
                                signame="Interrupted";
                                break;
                        case SIGSEGV:
                                signame="Segmentation Fault";
                                break;
                        case SIGTERM:
                                signame="Terminated";
                                break;
                        default:
                                signame="Signaled";
                }

                fputs(signame,stderr);

#ifdef WCOREDUMP
                if(WCOREDUMP(stat_loc))
                        fputs(" (Dumped Core!)",stderr);
#endif

                putchar('\n');
        }
}

/* 
	Output a message to the terminal.

	Precondition: line!=NULL
*/
   

static void builtin_echo(char*line)
{
        register char*p=strpbrk(line," \t");

        if(p&&*++p)
                puts(p);
        else
                putchar('\n');
}

/* 
	Exit the shell. 

	Precondition: line!=NULL&&strlen(line)>3
*/

static void builtin_exit(char*line)
{
        char*p=line+4;

        if(!*p)
                exit(EXIT_SUCCESS);
}

/* 
	Enumerate built-in shell commands.
*/

static void builtin_help(char*help)
{
        puts("\nsupersh by Derek Callaway");
        puts("^^^^^^^^^^^^^^^^^^^^^^^^^");
        puts("echo    - output messages to terminal standard output");
        puts("exit    - terminate shell process");
        puts("help    - print this message");
        puts("history - view previously executed commands");
        puts("jobs    - list background commands");
        puts("set     - assign environment variable values\n");
}

typedef struct Input_def
{
        char**cmdvec;
        void(*internal)(char*);
        struct Input_def*next;
        unsigned int background:1;
	unsigned int historical:1;
} Input;

static Input*histlist=NULL;

/*
	Show previously executed commands. 
*/

static void builtin_history(char*line)
{
        register unsigned int cnt=0;
        Input*hp;

        for(hp=histlist;hp;hp=hp->next)
                printf("%u %s %s",++cnt,hp->cmdvec[0],hp->background?"&\n":"\n");
}

typedef struct Job_def
{
        pid_t pid;
        char*cmdbuf;
        struct Job_def*next;
} Job;

static Job*joblist=NULL;

/* 
	List currently executing background commands.
*/

static void builtin_jobs(char*line)
{
        register unsigned int cnt=0;
        Job*jp;

        for(jp=joblist;jp;jp=jp->next)
                printf("Running\tpid: %d job: %u argv: %s\n",(int)jp->pid,++cnt,jp->cmdbuf);
}

extern char**environ;

/* 
	Display or modify environment variables. 
  
	Precondition: line!=NULL&&strlen(line)>2
	Postcondition: If a variable name or binding was specified,
		       then an element of environ was added or modified.
*/

static void builtin_set(char*line)
{
        char*p=line+3;

	p+=strspn(p," \t\r\n\v\f");

        if(!*p)
	{
		register char**pp;

                for(pp=environ;*pp;pp++)
                        puts(*pp);
	}
        else
        {
                char*eq_flag=strchr(p,'='),*binding;
               
		if(eq_flag==p)
			shfault("syntax error near: '='");

		binding=malloc(strlen(p)+eq_flag?1:2);

                if(!binding)
                        shfail("malloc");

                /* account for whitespace before equal sign? */
                strcpy(binding,p);

                if(!eq_flag)
                {
                        char*p2=&binding[strlen(p)];
                        *p2++='=';
                        *p2='\0';
                }

                if(putenv(binding))
                        shfail("putenv");
        }
}

/*
	Parse user-provided command line input.

 	Precondition: in!=NULL
	Postcondition: 
		if((i=parse_inbuf))
		{
			i->next=NULL;
			i->cmdvec!=NULL;

			if(i->internal)
				internal==builtin_*;
			
			i->cmdvec[*]==&in[0..strlen(in)];
		}
*/
	

static Input*parse_inbuf(char*in)
{
	char*cl[BUFSIZ],*amp,*oldp;
        register char*p=in,**pp=cl;
        Input*ret=NULL;
        size_t len;

        ret=calloc(1,sizeof *ret);
        if(!ret)
                shfail("calloc");

        /* The following sequence of if-else statements corresponds to 
           commands which are internal to the shell. */
        if(!strncmp(in,"echo",4)&&isspace((int)in[4]))
                ret->internal=builtin_echo;
        else if(!strncmp(in,"exit",4)&&isspace((int)in[4]))
                ret->internal=builtin_exit;
        else if(!strncmp(in,"help",4)&&isspace((int)in[4]))
                ret->internal=builtin_help;
        else if(!strncmp(in,"history",7)&&isspace((int)in[7]))
                ret->internal=builtin_history;
        else if(!strncmp(in,"jobs",4)&&isspace((int)in[4]))
                ret->internal=builtin_jobs;
        else if(!strncmp(in,"set",3)&&isspace((int)in[3]))
                ret->internal=builtin_set;

        pp=cl;

        if(ret->internal)
                *pp++=in;
        else
	{
		/* Deal with history reference, if any. */
		char*exc=strchr(p,'!');

		if(exc)
		{
			unsigned long hr;

			p=++exc;
			p+=strcspn(exc," \t\r\n\v\f");
			*p='\0';

			hr=strtoul(exc,NULL,10);
			if(!hr)
			{
				perror("strtoul");
				free(ret);
				ret=NULL;
			}
			else
			{
				register unsigned int cnt=0;
				Input*hp;

				for(hp=histlist;hp;hp=hp->next)
					if(++cnt==hr)
						break;

				if(cnt!=hr)
				{
					shfault("!%lu: event not found",hr);
					return NULL;
				}

				memcpy(ret,hp,sizeof *ret);
				ret->historical=1;
			}

			return ret;
		}

                do
                {
			/* Fill an array appropriate for passing to execvp(). */
                       	p+=strspn(p," \t\r\n\v\f");

                       	if(!*p)
                               	break;

                       	oldp=p;
                       	p+=strcspn(p," \t\r\n\v\f");
                        *pp++=oldp;

			if(*p)
                        	*p='\0';
			else
				break;
                } while(*++p);
	}

        *pp=NULL;

        len=pp-cl;

        ret->cmdvec=malloc((len+1)*sizeof *(ret->cmdvec));
        if(!ret->cmdvec)
                shfail("malloc");

        memcpy(ret->cmdvec,cl,(len+1)*sizeof *(ret->cmdvec));

        /* Handle a possible background processing operator. */
        p=cl[len-1];

        amp=strchr(p,'&');
        if(amp)
        {
                ret->background=1;

                if(amp==p)
                        ret->cmdvec[len-1]=NULL;
        }
        else
                amp=&cl[len-1][strlen(cl[len-1])];

        if(amp!=in)
        {
                do amp--; while(amp>in&&isspace((int)*amp));
                *++amp='\0';
        }

        return ret;
}

void handler(int signum){}

int main(void)
{
        char*inbuf;
        pid_t pid;
        Input*input_data,*hptr;
        unsigned long count_commands=1;
        register char*p;

        puts(":-) Welcome to supersh. Type help for help.\n");

	signal(SIGINT,SIG_IGN);
	signal(SIGTERM,SIG_IGN);

        while(1)
        {
		Job*jptr,*jprev=NULL;
		register unsigned int cnt=0;

                printf("[%lu]%c ",count_commands,getuid()?'$':'#');

                inbuf=malloc(BUFSIZ);
                if(!inbuf)
                        shfail("malloc");

                p=inbuf;

                if(!fgets(p,BUFSIZ,stdin))
                        exit(EXIT_SUCCESS);

                for(jptr=joblist;jptr;jptr=jptr->next)
                {
			int stat_loc=0;

                       	cnt++;

                       	if(waitpid(jptr->pid,&stat_loc,WNOHANG)==jptr->pid)
			{
                       		if(WIFEXITED(stat_loc))
                        	{
                       			printf("End\tpid: %d job: %u argv: %s exit: %d\n",
						(int)jptr->pid,cnt,jptr->cmdbuf,WEXITSTATUS(stat_loc));

					if(jptr->next)
					{
                                		if(jprev)
                                        		jprev->next=jptr->next;
                                        	else
							joblist=jptr->next;

						free(jptr);
					}
					else
					{
						free(joblist);
						joblist=NULL;
						jptr=joblist;
						break;
				 	}
				}

                         	wait_handler(stat_loc);
                         	jprev=jptr;
                	}
		}

                input_data=NULL;

                p+=strspn(p," \t\r\n\v\f");
                if(!*p)
                        continue;

                count_commands++;

                if(!histlist)
                {
                        histlist=calloc(1,sizeof *histlist);
                        if(!histlist)
                                shfail("malloc");

                        hptr=histlist;
                }
                else
                {
                        register unsigned int cnt=0;

                        hptr=histlist;

                        if(hptr)
                        {
                                cnt++;

                                while(hptr&&hptr->next)
                                {
                                        hptr=hptr->next;
                                        cnt++;
                                } 

                                if(cnt>=BUFSIZ)
                                {
                                        Input*histp=histlist->next;

                                        free(histlist);
                                        histlist=histp;
                                }

                                hptr->next=calloc(1,sizeof *(hptr->next));
                                hptr=hptr->next;
                        }
                        else
                        {
                                histlist=calloc(1,sizeof *histlist);
                                hptr=histlist;
                        }

                        if(!hptr)
                                shfail("calloc");
                }

                if(!input_data)
		{
                        input_data=parse_inbuf(inbuf);
			if(!input_data)
				continue;
		}

                hptr->cmdvec=input_data->cmdvec;
                hptr->background=input_data->background;
                hptr->internal=input_data->internal;

                if(input_data->internal&&!input_data->background)
                {
                        input_data->internal(input_data->historical?input_data->cmdvec[0]:inbuf);
                        continue;
                }

                pid=fork();
                if(!pid) /* child */
                {
                        if(input_data->internal)
                                input_data->internal(inbuf);
			else
			{
                        	execvp(input_data->cmdvec[0],input_data->cmdvec);
				shfault("%s: %s",input_data->cmdvec[0],strerror(errno));
			}

                        exit(EXIT_SUCCESS);
                }
                else     /* parent */
                {
                        if(pid<0)
                        {
				shfault("%s",strerror(errno));
                                continue;
                        }
                        else
                        {
                                register unsigned int cnt=0;
				int stat_loc=0;

                                if(!input_data->background)
                                {
                                        waitpid(pid,&stat_loc,0);
                                        wait_handler(stat_loc);
                                        continue;
                                }

                                for(jptr=joblist;jptr&&jptr->next;jptr=jptr->next)
					cnt++;

                                if(!jptr)
                                {
                                        joblist=malloc(sizeof *joblist);
                                        jptr=joblist;
                                }
                                else
                                {
                                        jptr->next=malloc(sizeof *jptr);
                                        jptr=jptr->next;
                                }

                                if(!jptr)
                                        shfail("malloc");

                                jptr->pid=pid;
                                jptr->cmdbuf=inbuf;
                                printf("Begin\tpid: %d job: %u argv: %s\n",(int)pid,++cnt,inbuf);
                        }
                }
        }

        return 0;
}

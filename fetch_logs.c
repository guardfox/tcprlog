#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <utime.h>
#include <signal.h>
#include "st.h"


#define str(s) #s
#define xstr(s) str(s)


#define IP_SIZE 16
#define FILE_SIZE 128
#define LOG_SIZE 32
#define LINE_SIZE 1024
#define TIME_S_SIZE 20
#define SIZE_BUF 131072
#define CHMOD_NEW_LOGS 0644

#define TIMEOUT_1s (1000000LL)

#define OK_s "OK\n"
#define ERR_s "ERR\n"
#define PARTIAL_WRITE "partial write"
#define CONNECTION_CLOSED "connection closed"
#define TIMEOUT "timeout"

static char zabuff[SIZE_BUF+1];
static int n,nothreads = 0,exitasap = 0,relogs_ts_glob = 0,restart = 0;
static int no_servers;
static st_thread_t *threads = NULL;
static char *conf_file;
static char *snmp_dir;

typedef struct {
   in_addr_t addr;
   char status_file[FILE_SIZE+1];
   char ip[IP_SIZE+1];
   int no_err,last_status;
   time_t last_update;
} server;

static server *servers;

static void Signal(int sig, void (*handler)(int))
{ 
  struct sigaction sa;

  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(sig, &sa, NULL);
}

static void wdog_sighandler(int signo)
{
   int i;
   if(signo == SIGINT || (signo == SIGTERM)) exitasap = 1;
   else if(signo == SIGUSR1) relogs_ts_glob = st_time();
   else if(signo == SIGHUP) restart = 1;
   for(i=0;i<nothreads;i++) if(threads[i]) st_thread_interrupt(threads[i]);
}

static void make_runners(void);

void check_reschedule_threads(void)
{
   if(!--nothreads && !exitasap)
      make_runners();

}

static char *my_time(void)
{
   time_t t = st_time();
   static char t_[TIME_S_SIZE+1];
   strftime(t_,TIME_S_SIZE,"%Y-%m-%dT%H:%M:%S",localtime(&t));
   return t_;
}

static void print_sys_error(const char *msg)
{
   fprintf(stderr, "%s %s: %s\n", my_time(), msg, strerror(errno));
}

static void print_sys_error_2msg(const char *msg, const char *msg2)
{
   fprintf(stderr, "%s %s %s: %s\n", my_time(), msg, msg2, strerror(errno));
}

static void print_sys_error_3msg(const char *msg, const char *msg2, const char *msg3)
{
   fprintf(stderr, "%s %s %s %s: %s\n", my_time(), msg, msg2, msg3, strerror(errno));
}

static void update_serv_status(server *serv,int status_change)
{
   int last_status = (serv->no_err == 0),status;
   serv->no_err += status_change;
   status = (serv->no_err == 0);
   if(last_status != status) {
      int sf = open(serv->status_file,O_RDWR|O_TRUNC|O_CREAT,CHMOD_NEW_LOGS);
      int str_l;
      char *str;
      serv->last_update = st_time();
      if(sf<0) {
         print_sys_error_2msg("open RW",serv->status_file);
         return;
      }
      if(status) {str = OK_s;str_l = sizeof OK_s;}
      else {str = ERR_s;str_l = sizeof ERR_s;}
      str_l--;
      if(write(sf,str,str_l) < str_l)
         print_sys_error_2msg("write to",serv->status_file);
      if(close(sf) < 0)
         print_sys_error_2msg("close",serv->status_file);
   } else if(serv->last_update + 50 < st_time()) {
      if(utime(serv->status_file,NULL)<0) {
         print_sys_error_2msg("utime",serv->status_file);
      }

      serv->last_update = st_time();
   }

}

static void write_status(char *status_line,char *status_line2,char *log_file,int status,int *last_status,server *serv)
{
   update_serv_status(serv,status - *last_status);
   *last_status = status;
   if(status)
      return;
   fprintf(stdout,"%s %s %s %s %s\n",my_time(),serv->ip,log_file,status_line,status_line2);
}

static int update_pointer_file(char *pointer_file,char *pointer)
{
   static char buf[256];
   int buf_l,sf;
   sf = open(pointer_file,O_RDWR|O_TRUNC|O_CREAT,0644);
   if(sf<0) {
      print_sys_error_2msg("open RW",pointer_file);
      return -1;
   }
   buf_l = snprintf(buf,sizeof buf,"%s\n",pointer);
   if(write(sf,buf,buf_l) < buf_l) {
      print_sys_error_2msg("write to",pointer_file);
      close(sf);
      return -1;
   }
   if(close(sf)<0) {
      print_sys_error_2msg("close",pointer_file);
      return -1;
   }
   return 0;
}

static int relay_data(char *ptr,int n,unsigned long int *local_size,int out_fd,char *pointer,char *pointer_file,
      char *log_file,int *last_status,server *serv)
{
   int i = 0;
   char *l_ptr = ptr;
   while(n--) {
      if(*ptr == '\a') {
         if(i) {
            int written = write(out_fd,l_ptr,i);
            if(written < i) {
               if(written < 0)
                  write_status("ERR write to data file:",strerror(errno),log_file,0,last_status,serv);
               else
                  write_status("ERR write to data file:",PARTIAL_WRITE,log_file,0,last_status,serv);
               return -1;
            }
            *local_size += i;
         }
         i = -1;
         l_ptr = ptr + 1;
      }
      ptr++;i++;
   }
   if(i>0) {
      int written = write(out_fd,l_ptr,i);
      if(written < i) {
         if(written < 0)
            write_status("ERR write data file:",strerror(errno),log_file,0,last_status,serv);
         else
            write_status("ERR write data file:",PARTIAL_WRITE,log_file,0,last_status,serv);
         return -1;
      }
      *local_size += i;
   }
   write_status("OK fetching data","",log_file,1,last_status,serv);
   return 0;
}


static void* fetch_file(void *line)
{
   int srv_port,s,out_fd,last_status = 0,relogs_ts = 0;
   unsigned long int local_size;
   FILE *f_sf;
   char log_file[FILE_SIZE+1],ofile[FILE_SIZE+1],
        pointer_file[FILE_SIZE+1],pointer[LOG_SIZE+1];
   st_netfd_t s_st;
   struct sockaddr_in srv_addr;
   server *serv = NULL,*sp;
   {
      char srv_ip[IP_SIZE+1];
      if(sscanf(line,"%s%*[ 	]%i%*[ 	]%s%*[ 	]%s%*[ 	]%s",
            srv_ip,&srv_port,log_file,ofile,pointer_file) == 5) {
         out_fd = open(ofile,O_RDWR|O_APPEND|O_CREAT,CHMOD_NEW_LOGS);
         if(out_fd < 0) {
            print_sys_error_2msg("open APPEND",ofile);
            st_sleep(5);
            goto boil_out;
         }
         srv_addr.sin_family = AF_INET;
         srv_addr.sin_port = htons(srv_port);
         srv_addr.sin_addr.s_addr = inet_addr(srv_ip);
         for(sp = servers; sp < servers+no_servers;sp++) {
            if(sp->addr == srv_addr.sin_addr.s_addr) {
               serv = sp;
               break;
            }
         }
         if(!serv) {
            int sf;
            serv = servers+no_servers;
            no_servers++;
            serv->addr = srv_addr.sin_addr.s_addr;
            strncpy(serv->ip,srv_ip,sizeof serv->ip);
            serv->no_err = -1;
            serv->last_status = 0;
            serv->last_update = 0;
            snprintf(serv->status_file,sizeof serv->status_file,"%s/%s",snmp_dir,srv_ip);
            sf = open(serv->status_file,O_RDWR|O_TRUNC|O_CREAT,0644);
            if(sf<0) {
               print_sys_error_2msg("open RW",serv->status_file);
               st_sleep(5);
               goto boil_out;
            }
            if(write(sf,ERR_s,sizeof ERR_s - 1) < (sizeof ERR_s - 1)) {
               print_sys_error_2msg("write to",serv->status_file);
               close(sf);
               st_sleep(5);
               goto boil_out;
            }
            if(close(sf) < 0) {
               print_sys_error_2msg("close",serv->status_file);
               st_sleep(5);
               goto boil_out;
            }
         } else {
            serv->no_err--;
         }
         f_sf = fopen(pointer_file,"r");
         if(f_sf) {
            struct stat buf;
            fscanf(f_sf,"%" xstr(LOG_SIZE) "s",pointer);
            fclose(f_sf);
            if(fstat(out_fd,&buf) < 0) {
               print_sys_error_2msg("fstat",ofile);
               st_sleep(5);
               goto boil_out;
            }
            local_size = buf.st_size;
         } else {
            pointer[0] = '0';pointer[1] = '\0';
            local_size = 0;
         }
      } else {
         fprintf(stderr, "skiping line: %s", (char*)line);
         free(line);
         goto boil_out;
      }
   }
   free(line);
   

   while(1) {
      if(s_st) { st_netfd_close(s_st); s_st = NULL;}
      if(exitasap || restart) {
         if(close(out_fd) < 0)
            print_sys_error_2msg("close",ofile);
         out_fd = -1;
         break;
      } else if(relogs_ts < relogs_ts_glob) {
         struct stat buf;
         relogs_ts = relogs_ts_glob;
         if(close(out_fd) < 0)
            print_sys_error_2msg("close",ofile);
         out_fd = open(ofile,O_RDWR|O_APPEND|O_CREAT,CHMOD_NEW_LOGS);
         if(out_fd < 0) {
            print_sys_error_2msg("open APPEND",ofile);
            st_sleep(5);
            goto boil_out;
         }
         if(fstat(out_fd,&buf) < 0) {
            print_sys_error_2msg("fstat",ofile);
            st_sleep(5);
            goto boil_out;
         }
         local_size = buf.st_size;
      }
      if(st_sleep(5) < 0 && (errno == EINTR))
         continue;
      if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
         print_sys_error("socket");
         continue;
      }
      {
         int opt = 131071;
         setsockopt(s, SOL_SOCKET, SO_RCVBUF, &opt, sizeof opt);
      }
      if ((s_st = st_netfd_open_socket(s)) == NULL) {
         print_sys_error("st_netfd_open_socket");
         close(s);
         continue;
      }
      if (st_connect(s_st, (struct sockaddr *)&srv_addr,
           sizeof(srv_addr), 3*TIMEOUT_1s) < 0) {
         if(errno != EINTR) write_status("ERR init connect:",strerror(errno),log_file,0,&last_status,serv);
         continue;
      }
      {
         char buf[LOG_SIZE+FILE_SIZE+23];
         size_t buf_l;
         buf_l = snprintf(buf,sizeof buf,"%s %s %li\n",log_file,pointer,
               local_size);
         if (st_write_resid(s_st, buf, &buf_l, 3*TIMEOUT_1s) < 0) {
            if(errno != EINTR) write_status("ERR init write:",strerror(errno),log_file,0,&last_status,serv);
            continue;
         }
      }
      {
         struct pollfd pd;
         int r_p;         
         pd.fd = s;
         pd.events =  POLLIN;
         pd.revents = 0;
         r_p = st_poll(&pd, 1, 3*TIMEOUT_1s);
         if (r_p <= 0) {
            if(r_p < 0) {
               if(errno != EINTR) write_status("ERR init read:",strerror(errno),log_file,0,&last_status,serv);
            } else
               write_status("ERR init read:",TIMEOUT,log_file,0,&last_status,serv);
            continue;
         }
      }
      n = (int) st_read(s_st, zabuff, sizeof zabuff, 0);
      if (n <= 0) {
         if(n < 0) {
            if(errno != EINTR) write_status("ERR init read:",strerror(errno),log_file,0,&last_status,serv);
         } else {
            write_status("ERR init read:",CONNECTION_CLOSED,log_file,0,&last_status,serv);
         }
         continue;
      }
      {
         int header_ok = 0,lost = 0,i;
         for(i=0; i<n; i++) {
            if(zabuff[i] == '\0') break;
            else if(zabuff[i] == '\n') {
               if((i > 2) && (LOG_SIZE > i-3) && (zabuff[1] == ' ') ) {
                  if(zabuff[0] == 'L') header_ok = lost = 1;
                  else if(zabuff[0] == 'O') header_ok = 1;
                  zabuff[i] = '\0';
               }
               break;
            }
         }
         if(!header_ok) {
            write_status("ERR init header is broken","",log_file,0,&last_status,serv);
            continue;
         } else {
            if(lost) fprintf(stderr, "%s %s:%i:%s lost %s:%li\n",my_time(),serv->ip,srv_port,log_file,pointer,local_size);
            if(strcmp(pointer,zabuff+2)) {
               char rot_name[FILE_SIZE];
               fprintf(stdout, "%s %s %s rotated to %s\n",my_time(),serv->ip,log_file,zabuff+2);
               if(close(out_fd) < 0) {
                  out_fd = -1;
                  print_sys_error_2msg("close",ofile);
                  st_sleep(5);
                  goto boil_out;
               }
               out_fd = -1;
               snprintf(rot_name,sizeof rot_name,"%s.%s",ofile,my_time());
               if(rename(ofile,rot_name) < 0) {
                  print_sys_error_3msg("rename",ofile,rot_name);
                  st_sleep(5);
                  goto boil_out;
               }
               strcpy(pointer,zabuff+2);
               if(update_pointer_file(pointer_file,pointer) < 0) {
                  st_sleep(5);
                  goto boil_out;
               }
               out_fd = open(ofile,O_RDWR|O_APPEND|O_CREAT,CHMOD_NEW_LOGS);
               if(out_fd < 0) {
                  print_sys_error_2msg("open APPEND",ofile);
                  st_sleep(5);
                  goto boil_out;
               }
               local_size = 0;
            }
         }
         i++;
         if(i<n)
            if(relay_data(zabuff+i,n-i,&local_size,out_fd,pointer,pointer_file,log_file,&last_status,serv) < 0)
               goto boil_out;
      }
      while(1) {
         struct pollfd pd;
         int r_p;

         pd.fd = s;
         pd.events =  POLLIN;
         pd.revents = 0;
         r_p = st_poll(&pd, 1, 120*TIMEOUT_1s);
         if (r_p <= 0) {
            if(r_p < 0) {
               if(errno != EINTR) write_status("ERR read:",strerror(errno),log_file,0,&last_status,serv);
            } else
               write_status("ERR read:",TIMEOUT,log_file,0,&last_status,serv);
            break;
         }
         n = (int) st_read(s_st, zabuff, sizeof zabuff, 0);
         if(n <= 0) {
            if(n < 0) {
               if(errno != EINTR) write_status("ERR read:",strerror(errno),log_file,0,&last_status,serv);
            } else
               write_status("ERR read:",CONNECTION_CLOSED,log_file,0,&last_status,serv);
            break;
         }
         if(relay_data(zabuff,n,&local_size,out_fd,pointer,pointer_file,log_file,&last_status,serv) < 0)
            goto boil_out;
      }
   }

boil_out:
   if(out_fd >= 0)
      if(close(out_fd) < 0)
         print_sys_error_2msg("close",ofile);
   if(s_st) st_netfd_close(s_st);
   check_reschedule_threads();
   return NULL;
}

static void make_runners(void)
{
   int i = 0;
   FILE *f;
   void *line;
   restart = 0;
   f = fopen(conf_file,"r");
   if(!f) {
     fprintf(stderr,"open RDONLY %s: %s\n",conf_file,strerror(errno));
     exit(1);
   }
   line = malloc(LINE_SIZE);
   while(fgets(line,LINE_SIZE,f)) {
      if(*(char*)line == '#') { continue;}
      nothreads++;
   }
   fclose(f);
   free(line);
   if(threads) free(threads);
   if(servers) free(servers);
   threads = malloc(sizeof (st_thread_t) * nothreads);
   servers = malloc(sizeof (server) * nothreads);
   no_servers = 0;
   f = fopen(conf_file,"r");
   if(!f) {
     fprintf(stderr,"open RDONLY %s: %s\n",conf_file,strerror(errno));
     exit(1);
   }
   line = malloc(LINE_SIZE);
   while(fgets(line,LINE_SIZE,f)) {
      if(*(char*)line == '#') { continue;}
      threads[i] = st_thread_create(fetch_file, line, 0, 0);
      if (threads[i] == NULL) {
         perror("st_thread_create");
         exit(1);
      }
      i++;
      line = malloc(LINE_SIZE);
   }
   fclose(f);
   free(line);
}

int main (int argc,char **argv)
{
   snmp_dir = argv[1];
   conf_file = argv[2];
   if(!snmp_dir || !conf_file) {
      fprintf(stderr,"usage: %s snmp_dir conf_file\n",argv[0]);
      exit(1);
   }
   if (st_init() < 0) {
      perror("st_init");
      exit(1);
   }
   setlinebuf(stdout);
   setlinebuf(stderr);
   st_timecache_set(1);     
   make_runners();
   Signal(SIGTERM, wdog_sighandler);
   Signal(SIGINT, wdog_sighandler);
   Signal(SIGHUP, wdog_sighandler);
   Signal(SIGUSR1, wdog_sighandler);
   st_thread_exit(NULL);
   /* NOTREACHED */
   return 1;
       
}

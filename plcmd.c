/////////////////////////////////////////////////////////////////////////////////

// libnodave library based daemon to get information from 
// SIEMENS SIMATIC 300 and 400 series
// v.1.0 by cirus

/////////////////////////////////////////////////////////////////////////////////

#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"
#include "signal.h"
#include "string.h"
#include "nodave.h"
#include "openSocket.h"
#include "mysql.h"
#include "time.h"

#define KNRM  "\x1B[0m"		//normal console color
#define KGRN  "\x1B[32m"	//green console color

#define NUM_PLC 	256		// max number of PLCs
#define NUM_ITEM 	256 	// max number of items of one PLC

#define DIR_CFG 	""
#define FILE_CFG 	"plcmd.conf"

#define DIR_PID		"/var/run/"
#define FILE_PID	"plcmd.pid"

// functions

long long clock_gettime_mcs(void);
void db_write(MYSQL mysql, int item_id, long long ts, long a );
void init(void);
int is_daemon_run (void);
void strrem(char * str1, char * str2);
void daemon_body(int i);
void daemon_stop(int i);

// vars

char file_pid[80];

MYSQL mysql;
int CYCLE_DELAY = 1000;

int plc[NUM_PLC][7];						//PLCs data
char plc_ip[NUM_PLC][15];
    
int item[NUM_PLC][NUM_ITEM][9];				//items data
long long item_timer[NUM_PLC][NUM_ITEM];	//current timers for items

long alast[NUM_PLC][NUM_ITEM];				// last answers for mode=by value change


/////////////////////////////////////////////////////////////////////////////////

// do

/////////////////////////////////////////////////////////////////////////////////
    
int main(int argc, char **argv) {
	
	// define signals to stop PLCMD
	
	struct sigaction sa;
	sigset_t newset;
	sigemptyset(&newset);
	sigaddset(&newset, SIGHUP);
	sigprocmask(SIG_BLOCK, &newset, 0);
	sa.sa_handler = daemon_stop;
	sigaction(SIGTERM, &sa, 0);
	
	pid_t pid;
	FILE *fpid;
	
	char buf[80];			// temporary var
	char manual[256];		// string how to use daemon
	
	sprintf (file_pid,"%s%s",DIR_PID,FILE_PID);
	sprintf (manual,"\nUsage:\n%s./plcmd start%s - start as daemon\n%s./plcmd stop%s - stop daemon\n%s./plcmd console%s - work in console mode\n",KGRN,KNRM,KGRN,KNRM,KGRN,KNRM);
	
	
	// if no arguments - show manual
	
	if (argc < 2) {
	   printf(manual);
	   exit(1);
    }
    
    // console mode - print information to console
    
    if (strcmp(argv[1],"console")==0) {
		daemon_body(3);
	}
	
	// start in daemon mode
	
	else if (strcmp(argv[1],"start")==0) {
		if (is_daemon_run()) {
			printf("\nPLCMD is already running.\n\n");
			exit(0);
		}
		if((pid=fork())<0) {                  
			exit(0);               
		}
		else if (pid!=0) {
			printf("[%i]\n",pid);
			if((fpid=fopen(file_pid, "w")) == NULL) {
				printf("\nCannot write PID file into %s folder.\nRun anyway ? (y/n): ",DIR_PID);
				scanf("%s",buf);
				if (strcmp(buf,"y") != 0) {
					printf("Good by!\n\n");
					kill(pid,SIGTERM);
					exit(0);
				} else {
					printf("Started...\n\n"); 
				}
			} else {
				fprintf(fpid,"%i",pid);
				fclose(fpid);
			}
			exit(0);
		}
		setsid();  
		init();  
		daemon_body(1);
	}
	
	// stop all PLCMD daemons
	
	else if (strcmp(argv[1],"stop")==0) {
		if((fpid=fopen(file_pid, "r")) == NULL) {
			printf("\nCannot open %s file.\nTry to find PID....\n",FILE_PID);
			pid = 1;
			while (pid != 0) {
				pid = is_daemon_run();
				if (pid) {
					printf("PID = %i... Stopped\n",pid);
					kill(pid,SIGTERM);
					sleep(2);
				}
			}
			if (pid == 0) {
				printf("Cannot find any PLCMD.\nPLCMD Stopped.\n\n");
				exit(0);
			}
		} else {
			fgets(buf,80,fpid);
			pid = atoi(buf);
			fclose(fpid);
		}
		printf("PLCMD stopped.\n\n");
		kill(pid,SIGTERM);
		
	}
	
	else {
		printf(manual);
		exit(0);
	}          
	return 0;
    mysql_close(&mysql);
}

/////////////////////////////////////////////////////////////////////////////////

// FUNCTIONS

/////////////////////////////////////////////////////////////////////////////////

// get current time in microseconds

/////////////////////////////////////////////////////////////////////////////////

long long clock_gettime_mcs(void) {
	struct timespec mt1; 
	long long tt;
	
	clock_gettime (CLOCK_REALTIME, &mt1);
	tt = (long long) mt1.tv_sec;
	tt = tt * 1000000 + mt1.tv_nsec/1000;
	
	return tt;
}

/////////////////////////////////////////////////////////////////////////////////

// write answer to the database

/////////////////////////////////////////////////////////////////////////////////

void db_write(MYSQL mysql, int item_id, long long ts, long a ) {
	char query[255];
	char buf[32];
	
	sprintf(query,"INSERT INTO log (item_id,ts,answer) VALUES (%i,%lld,%ld)",item_id,ts,a);
    mysql_query(&mysql, query);
}

/////////////////////////////////////////////////////////////////////////////////

// remove substring str2 from string str1

/////////////////////////////////////////////////////////////////////////////////

void strrem(char * str1, char * str2) {
	int len1,len2,start,end,i;
	char *pos;
	
	while (strstr(str1,str2) != NULL) {
		len1 = strlen(str1);
		len2 = strlen(str2);
		pos = strstr(str1,str2);
		start = pos - str1;
		end = len1 - len2;
		
		for(i = start; i <= end; i++) {
			str1[i] = str1[i+len2];
		}
	}
}

/////////////////////////////////////////////////////////////////////////////////

// read settings from configuration file, 
// connect to the database 
// and get information about PLCs and their items

/////////////////////////////////////////////////////////////////////////////////

void init(void) {
	
	char file_cfg[256];
	FILE * fcfg;
    char host[20], user[20], password[20], db_name[20];
    int port = 0; 
    char * pos;
    int delimiter = '=';
    int varlen;
    char str[64];
    char varname[32];
    char varvalue[32];
    
    int num_plc, num_item;
    int p,i,j;

    MYSQL_ROW row;
    MYSQL_RES * result_plc, * result_item, * result_param;
    char query[255];
    
	sprintf (file_cfg,"%s%s",DIR_CFG,FILE_CFG);
    
    // reading configuration file

    if ((fcfg = fopen(file_cfg,"r")) == NULL) {
		fprintf(stderr, "Failed to open configuration file '%s'\n",FILE_CFG);    
		exit(0);
    }
    while ( !feof(fcfg) ) {
		fgets(str,255,fcfg);
		pos = strchr(str, delimiter);
		if (pos) {
			
			// get variable name
			
			varlen = abs (str - pos);
			strncpy(varname,str,varlen);
			varname[varlen] = 0;
			
			// get variable value
			
			strncpy(varvalue,str + varlen + 1,strlen(str) - varlen);
			varvalue[strlen(str) - varlen - 2] = 0;

			// set mysql connection params
			
			if (!strcmp(varname,"host")) {
				strcpy(host,varvalue);
			}
			if (!strcmp(varname,"port")) {
				port = atoi(varvalue);
			}
			if (!strcmp(varname,"user")) {
				strcpy(user,varvalue);
			}
			if (!strcmp(varname,"password")) {
				strcpy(password,varvalue);
			}
			if (!strcmp(varname,"db_name")) {
				strcpy(db_name,varvalue);
			}
		}
      
    }
    fclose (fcfg); 
	
    // reset PLC and item arrays
    
    for (p = 0; p < NUM_PLC; p++) {
		plc[p][0] = 0;
		for (i = 0; i < NUM_ITEM; i++) {
			item[p][i][0] = 0;
			item_timer[p][i] = 0;
			alast[p][i] = 0;
		}
	}
	
	// connect to the database
    
    mysql_init(&mysql);

    if (!mysql_real_connect(&mysql, host, user, password, db_name, port, NULL, 0)) {
		fprintf(stderr, "Failed to connect to database: Error: %s\n", mysql_error(&mysql));    
		exit(0);
    }
    
    // get params
    
    strcpy(query, "SELECT value FROM param_plcm WHERE name='CYCLE_DELAY'");
    mysql_query(&mysql, query);
    result_param = mysql_store_result(&mysql);
    row = mysql_fetch_row(result_param);
    CYCLE_DELAY = abs(atoi(row[0]));
    
	// get active PLCs from database and put them into array
	
	strcpy(query, "SELECT id,daveProto,daveSpeed,daveTimeout,MPI,rack,slot,ip  FROM plc WHERE active=1");
	
    mysql_query(&mysql, query);
    result_plc = mysql_store_result(&mysql);
    num_plc = mysql_num_rows(result_plc);
    
    for (p = 0; p < num_plc; p++) {
		row = mysql_fetch_row(result_plc);
		for (j = 0; j <= 6; j++) {
			plc[p][j] = atoi(row[j]);
			strcpy(plc_ip[p],row[7]);
		}

		sprintf(query,"SELECT id,plc_id,area,DB,start,len,mode,timer,write_mode FROM item WHERE active=1 AND plc_id=%s",row[0]);
		
		mysql_query(&mysql, query);
		result_item = mysql_store_result(&mysql);
		num_item = mysql_num_rows(result_item);
		
		// get active items for current PLC and put them into array
		
		for (i = 0; i < num_item; i++) {
			row = mysql_fetch_row(result_item);
			for (j = 0; j <= 8; j++) {
				item[p][i][j] = atoi(row[j]);
			}			
			
		}
		item[p][num_item][0] = 0;		// mark the end of items array for this PLC
		
		mysql_free_result(result_item);
	}
	plc[num_plc][0] = 0;				// mark the end of PLCs array
	
	mysql_free_result(result_plc);
}

/////////////////////////////////////////////////////////////////////////////////

// is daemon already run ?

/////////////////////////////////////////////////////////////////////////////////

int is_daemon_run (void) {
    FILE *pf;
    char command[20],smypid[10];
    char data[512];
    int pid=0,mypid;
    int retval=0;

    // use pipe and system 'pidof' command
    
    sprintf(command, "pidof plcmd"); 
    pf = popen(command,"r"); 
    fgets(data, 512 , pf);
    
    // remove our pid from answer
    
    sprintf(smypid,"%i", getpid());
    strrem(data,smypid);

    pid = atoi(data);
    mypid = atoi(smypid);
    
    // if we have another pid of PLCMD return pid

    if (pid == mypid) return 0;
    else return pid;
}

/////////////////////////////////////////////////////////////////////////////////

// daemon body

/////////////////////////////////////////////////////////////////////////////////

void daemon_body (int d) {
	
	int p,i;
	long long ts, ts1, ts2;		// current timers
	
	// libnodave vars
	
	int res;
	long a;										// PLCs answer
	daveInterface * di[NUM_PLC];
	daveConnection * dc[NUM_PLC];
	_daveOSserialType fds[NUM_PLC];
		
	int daveProto, daveSpeed, daveTimeout;
	int MPI, rack, slot;
	
	// daveSetDebug(daveDebugPrintErrors);

	// open connections to the PLCs
	
	p = 0;
	while (plc[p][0]) {
		if (plc_ip[p]) {
			fds[p].rfd = openSocket(102, plc_ip[p]);
			fds[p].wfd = fds[p].rfd;
			
			if (fds[p].rfd > 0) { 

				// daveProto = plc[p][1];
				// daveSpeed = plc[p][2];
				// daveTimeout = plc[p][3];
				
				// MPI = plc[p][4];
				// rack = plc[p][5];
				// slot = plc[p][6];
				
				di[p] =daveNewInterface(fds[p],"IF1",0, plc[p][1], plc[p][2]);
				daveSetTimeout(di[0],plc[p][3]);
				dc[p] =daveNewConnection(di[p],plc[p][4],plc[p][5], plc[p][6]);
				
				if (daveConnectPLC(dc[p]) != 0) {
					fprintf(stderr,"Couldn't connect to PLC.\n");	
					//return -2;
				}
			} else {
			fprintf(stderr,"Couldn't open TCP port. \nPlease make sure a PLC is connected and the IP address is ok. \n");	
				//return -1;
			}
		}
		p++;
	}
	
	// items interrogation
	
	for (;;) {
		p = 0;
		while (plc[p][0]) {
			if (plc_ip[p] && fds[p].rfd > 0) {
				i = 0;
				while (item[p][i][0]) {
					
					// if 'mode' is set to 'by timer'
					
					if (item[p][i][6] == 0) {
						ts = clock_gettime_mcs();
						if (ts >= item_timer[p][i]) {
							
							//item[p][i][2] = area
							//item[p][i][3] = DB
							//item[p][i][4] = start
							//item[p][i][5] = len
							
							res=daveReadBytes(dc[p],item[p][i][2],item[p][i][3],item[p][i][4],item[p][i][5],NULL);
							
							// set timer to the next time
							
							item_timer[p][i] = ts + item[p][i][7];
							if (res == 0) { 
								if (item[p][i][5] == 8) a = daveGetU8(dc[p]);
								if (item[p][i][5] == 16) a = daveGetU16(dc[p]);
								if (item[p][i][5] == 32) a = daveGetU32(dc[p]);
								ts1 = clock_gettime_mcs();
								db_write(mysql,item[p][i][0],ts,a);
								ts2 = clock_gettime_mcs();
							}  else fprintf(stderr,"failed! (%d)\n",res); 
						}
					}
					
					// if 'mode' is set to 'by value change'
					
					if (item[p][i][6] == 1) {
						ts = clock_gettime_mcs();
						res=daveReadBytes(dc[p],item[p][i][2],item[p][i][3],item[p][i][4],item[p][i][5],NULL);
						if (res == 0) {
							if (item[p][i][5] == 8) a = daveGetU8(dc[p]);
							if (item[p][i][5] == 16) a = daveGetU16(dc[p]);
							if (item[p][i][5] == 32) a = daveGetU32(dc[p]);
							if (a != alast[p][i]) {
								db_write(mysql,item[p][i][0],ts,a);
								alast[p][i] = a;
							}
						}
					}
					i++;
				}
			}
			p++;
			usleep(CYCLE_DELAY);
		}
	}
}

/////////////////////////////////////////////////////////////////////////////////

// daemon stop

/////////////////////////////////////////////////////////////////////////////////

void daemon_stop(int i) {
	unlink(file_pid);
	mysql_close(&mysql);
	exit(EXIT_SUCCESS);
}

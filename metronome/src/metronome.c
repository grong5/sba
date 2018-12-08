#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <string.h>
#include <ctype.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/types.h>
#include <sys/siginfo.h>
#include <time.h>

#define ROWNUM 8

#define MY_PULSE_CODE   _PULSE_CODE_MINAVAIL

int counter = 0;

typedef union {
	struct _pulse   pulse;
	char msg[255];
} my_message_t;

typedef struct DataTable
{
	int top; //time-signature-top (O)
	int bottom; //time-signature-bottom
	int interval; //Number of Intervals within each beat
	char *pattern;
} DataTableRow;

DataTableRow t[] = {
		{2, 4, 4, "|1&2&"},
		{3, 4, 6, "|1&2&3&"},
		{4, 4, 8, "|1&2&3&4&"},
		{5, 4, 10, "|1&2&3&4-5-"},
		{3, 8, 6, "|1-2-3-"},
		{6, 8, 6, "|1&a2&a"},
		{9, 8, 9, "|1&a2&a3&a"},
		{12, 8, 12, "|1&a2&a3&a4&a"}
};
char data[255];
int server_coid;


int checkInt (const char *operand);
void printOutput(char *output);
void *childThread(void *arg);

//@Override int io_read()
int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{

	int nb;

	if (data == NULL) return 0;

	nb = strlen(data);

	//test to see if we have already sent the whole message.
	if (ocb->offset == nb)
		return 0;

	//We will return which ever is smaller the size of our data or the size of the buffer
	nb = min(nb, msg->i.nbytes);

	//Set the number of bytes we will return
	_IO_SET_READ_NBYTES(ctp, nb);

	//Copy data into reply buffer.
	SETIOV(ctp->iov, data, nb);

	//update offset into our data used to determine start position for next read.
	ocb->offset += nb;

	//If we are going to send any bytes update the access time for this resource.
	if (nb > 0)
		ocb->attr->flags |= IOFUNC_ATTR_ATIME;

	return(_RESMGR_NPARTS(1));
}

//@Override int io_write()
int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
	int nb = 0;

	if( msg->i.nbytes == ctp->info.msglen - (ctp->offset + sizeof(*msg) ))
	{
		/* have all the data */
		char *buf;
		char *pause_msg;
		int i, small_integer;
		buf = (char *)(msg+1);

		if(strstr(buf, "pause") != NULL){
			for(i = 0; i < 2; i++){
				pause_msg = strsep(&buf, " ");
			}
			small_integer = atoi(pause_msg);
			if(small_integer >= 1 && small_integer <= 9){
				//FIXME :: replace getprio() with SchedGet()
				MsgSendPulse(server_coid, SchedGet(0,0,NULL), _PULSE_CODE_MINAVAIL, small_integer);
			} else {
				printf("Integer is not between 1 and 99.\n");
			}
		} else {
			strcpy(data, buf);
		}

		nb = msg->i.nbytes;
	}
	_IO_SET_WRITE_NBYTES (ctp, nb);

	if (msg->i.nbytes > 0)
		ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;

	return (_RESMGR_NPARTS (0));
}


//@Override int io_open()
int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
	if ((server_coid = name_open("metronome", 0)) == -1) {
		perror("name_open failed.");
		return EXIT_FAILURE;
	}
	return (iofunc_open_default (ctp, msg, handle, extra));
}

int main(int argc, char** argv) {

	int j, rcvid;
	int flag = 0;

	double beats, time_top, pause_time, intervals;
	double time_bottom;
	double second_p, space;
	char *output;
	name_attach_t *att;
	struct sigevent event;
	struct itimerspec itime;
	timer_t timer_id;
	my_message_t msg;

	pthread_attr_t attr; // Thread attribute

	// Checking the arguments
	if(argc != 4) {
		fprintf (stderr, "usage: program [beats-per-minute] [time-signature-top] [time-signature-bottom]\n");
		exit (EXIT_FAILURE);
	}

	// Checking the valid input
	if(checkInt(argv[1])){
		fprintf (stderr, "Sorry. It must be a number.\n\n");
		exit (EXIT_FAILURE);
	}
	beats = (double)atoi(argv[1]);

	// Checking the valid input
	if(checkInt(argv[2])){
		fprintf (stderr, "Sorry. It must be a number.\n\n");
		exit (EXIT_FAILURE);
	}
	time_top = (double)atoi(argv[2]);

	// Checking the valid input
	if(checkInt(argv[3])){
		fprintf (stderr, "Sorry. It must be a number.\n\n");
		exit (EXIT_FAILURE);
	}
	time_bottom = (double)atoi(argv[3]);

	if ((att = name_attach(NULL, "metronome", 0)) == NULL) {
		return EXIT_FAILURE;
	}

	for(j = 0; j < ROWNUM; j++){
		if(time_top == t[j].top && time_bottom == t[j].bottom){
			intervals = t[j].interval;
			output = t[j].pattern;
			flag = 0;
			break;
		}else
			flag = 1;
	}

	if(flag == 1){
		fprintf (stderr, "Error. Try again\n\n");
		exit (EXIT_FAILURE);
	}

	second_p = (60 / beats) * time_top;

	space = second_p / intervals;



	event.sigev_notify = SIGEV_PULSE;
	event.sigev_coid = ConnectAttach(ND_LOCAL_NODE, 0,att->chid,_NTO_SIDE_CHANNEL, 0);
	event.sigev_priority = SchedGet(0,0,NULL);
	event.sigev_code = MY_PULSE_CODE;
	timer_create(CLOCK_REALTIME, &event, &timer_id);

	itime.it_value.tv_sec = 0;
	/* 500 million nsecs = .5 secs */
	itime.it_value.tv_nsec = 1;
	itime.it_interval.tv_sec = 0;
	/* 500 million nsecs = .5 secs */
	itime.it_interval.tv_nsec = space * 1000000000;
	timer_settime(timer_id, 0, &itime, NULL);

	while(1){

		pthread_attr_init(&attr); // Initializes a default thread attribute
		pthread_create(NULL, &attr, &childThread, NULL); // Create a new thread
		pthread_attr_destroy(&attr); // Destroy a thread attribute

		rcvid = MsgReceivePulse(att->chid, &msg, sizeof(msg), NULL);
		if (rcvid == 0) { /* we got a pulse */
			printf("Small Integer: %d\n", msg.pulse.value.sival_int);
			if (msg.pulse.code == MY_PULSE_CODE) {
				fflush(stdout);
				printOutput(output);
			} /* else other pulses ... */
		}

	}
	printf("\n%f\n", space);
	return EXIT_SUCCESS;
}

/*******************************************************************************
 * checkInt( ): int
 ******************************************************************************/
int checkInt (const char *num){

	/* Validate input as a number */
	const char *check = num;
	while(*check != '\0'){
		if(!isdigit(*check)){
			return 1;
		}
		check++;
	}
	return 0;
}

void printOutput(char *output){
	if(counter == strlen(output))
		counter = 0;
	if(counter == 0){
		printf("%c%c", output[counter], output[counter+1]);
		counter = 2;
	}else{
		printf("%c", output[counter]);
		counter++;
	}
}
/*******************************************************************************
 * childThread( ): void *
 ******************************************************************************/
void *childThread(void *arg){
	dispatch_t* dpp;
	resmgr_io_funcs_t io_funcs;
	resmgr_connect_funcs_t connect_funcs;
	iofunc_attr_t ioattr;
	dispatch_context_t   *ctp;
	int id;

	dpp = dispatch_create();
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
	connect_funcs.open = io_open;
	io_funcs.read = io_read;
	io_funcs.write = io_write;

	iofunc_attr_init(&ioattr, S_IFCHR | 0666, NULL, NULL);

	id = resmgr_attach(dpp, NULL, "/dev/local/metronome", _FTYPE_ANY, NULL, &connect_funcs, &io_funcs, &ioattr);

	ctp = dispatch_context_alloc(dpp);
	// The message receive loop
	while(1) {
		ctp = dispatch_block(ctp);
		dispatch_handler(ctp);
	}
}


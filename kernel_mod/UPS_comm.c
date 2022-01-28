#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#define MODPATH "/sys/module/UPS_powermod/parameters/"

const char *BATSTAT[]={"unknown","charging","discharging","not-charging","full"};

int set_interface_attribs(int fd,int speed,int parity){
	struct termios tty;
	if (tcgetattr(fd,&tty)!=0){
		fprintf(stderr,"UPS: Error %d: from tcgetattr.\n",errno);
		return -1;
	}

	cfsetospeed(&tty,speed);
	cfsetispeed(&tty,speed);

	tty.c_cflag = (tty.c_cflag&~CSIZE)|CS8;// 8-bit chars
	// disable IGNBRK for mismatched speed tests; otherwise receive break as \000 chars
	tty.c_iflag &= ~IGNBRK;// disable break processing
	tty.c_lflag = 0;// no signaling chars, no echo, no canonical processing
	tty.c_oflag = 0;// no remapping, no delays
	tty.c_cc[VMIN] = 0;// read doesn't block
	tty.c_cc[VTIME] = 5;// 0.5 seconds read timeout

	tty.c_iflag &= ~(IXON|IXOFF|IXANY);// shut off xon/xoff ctrl

	tty.c_cflag |= (CLOCAL|CREAD);// ignore modem controls, enable reading
	tty.c_cflag &= ~(PARENB|PARODD);// shut off parity
	tty.c_cflag |= parity;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CRTSCTS;

	if (tcsetattr(fd,TCSANOW,&tty)!=0){
		fprintf(stderr,"UPS: Error %d: from tcsetattr.\n",errno);
		return -1;
	}
	return 0;
}

void set_blocking(int fd,int should_block){
	struct termios tty;
	memset(&tty,0,sizeof tty);
	if (tcgetattr(fd,&tty)!=0){
		fprintf(stderr,"UPS: Error %d: from tggetattr.\n",errno);
		return;
	}

	tty.c_cc[VMIN] = should_block?1:0;
	tty.c_cc[VTIME] = 5;// 0.5 seconds read timeout

	if (tcsetattr(fd,TCSANOW,&tty)!=0)
		fprintf(stderr, "UPS: Error %d: setting term attributes.\n",errno);
}

// Initialize module parameters
int upsmod_init(){
	// The following parameters (capacity and presence) are not actually supported. Presence is changed to present when ever the serial connection is successful.
	int out_eng,out_bat;
	char wbuf[10];
	out_eng = open(MODPATH "battery_energy",O_WRONLY);
	out_bat = open(MODPATH "battery_present",O_WRONLY);
	if (!(out_eng&&out_bat)) return -1;
	sprintf(wbuf,"%d\0",3700000);
	write(out_eng,wbuf,strlen(wbuf));
	sprintf(wbuf,"%d\0",1);
	write(out_bat,wbuf,strlen(wbuf));
	close(out_eng);
	close(out_bat);
	return 0;
}

int main(int argc,char *argv[]){
	// Parse command line arguments for serial device path.
	if (argc!=2) {
		fprintf(stderr,"UPS: Invalid arguments!\n");
		return(-1);
	}

	int serial = open(argv[1],O_RDWR|O_NOCTTY|O_SYNC);
	if (serial < 0){
		fprintf(stderr,"UPS: Error %d opening %s: %s\n",errno,argv[1],strerror(errno));
		return -1;
	}

	// Setup serial device connection.
	set_interface_attribs(serial,B9600,0);  // set speed to 115,200 bps, 8n1 (no parity)
	set_blocking(serial,1);                // set blocking

	// Setup `select` test for serial read.
	fd_set sel_set;
	struct timeval sel_timeout;
	int sel_result;

	FD_ZERO(&sel_set);
	FD_SET(serial, &sel_set);

	sel_timeout.tv_sec = 3;
	sel_timeout.tv_usec = 0;

	// Test connection to the UPS and update module info on successful read.
	// Check if any data is sent by UPS from the serial port. Return -1 when nothing was received within 1 sec.
	sel_result = select(serial+1,&sel_set,NULL,NULL,&sel_timeout);
	if (sel_result==0) {
		fprintf(stderr,"UPS: Read from serial device timed out. The UPS might not be online.\n");
		close(serial);
		return -1;
	}
	else if (sel_result==-1) {
		fprintf(stderr,"UPS: Error %d selecting on serial port %s: %s\n",errno,argv[1],strerror(errno));
		close(serial);
		return -1;
	}

	if (upsmod_init()) {
		fprintf(stderr, "UPS: Error %d communicating with the UPS kernel module: %s. Please check if the module is loaded and you have the proper permissions.\n",errno,strerror(errno));
		close(serial);
		return -1;
	}

	// Loop condition. Reduced when parsing error occurs. Reset after each successful updates.
	int errcount=5;

	char rbuf[100],wbuf[10],rol,*ptr;
	int n = 0;

	int out_stat,out_bat,out_etchg,out_etdsc,out_ext,out_vlt;

	int stat,bat,etchg,etdsc,ext,vlt;
	int stat_l,bat_l,etchg_l,etdsc_l,ext_l,vlt_l;
	int start_percent,cur_percent;
	time_t start_time,cur_time;
	
	out_stat = open(MODPATH "battery_status",O_WRONLY);
	out_bat = open(MODPATH "battery_percentage",O_WRONLY);
	out_etchg = open(MODPATH "et_charge",O_WRONLY);
	out_etdsc = open(MODPATH "et_discharge",O_WRONLY);
	out_ext = open(MODPATH "external_online",O_WRONLY);
	out_vlt = open(MODPATH "output_voltage",O_WRONLY);

	stat_l = bat_l = etchg_l = etdsc_l = ext_l = vlt_l = -1;

	while (errcount) {
		n = rol = 0;
		memset(rbuf,0,sizeof(rbuf));
		while (rol!='\n') read(serial,&rol,sizeof(rol));
		while (rbuf[strlen(rbuf)-1]!='\n') n+=read(serial,rbuf+n,sizeof(rbuf)-n);

		// Update battery status.
		// Check power in
		if (!(ptr=strstr(rbuf,"Vin"))) {
			fprintf(stderr,"UPS: Warning: Corrupted message received.\n");
			errcount--;
			continue;
		}
		if (strstr(ptr,"GOOD")) ext=1;
		else if (strstr(ptr,"NG")) ext=0;
		else {
			fprintf(stderr,"UPS: Warning: Corrupted message received.\n");
			errcount--;
			continue;
		}
		// Check battery percentage
		if (!(ptr=strstr(ptr,"BATCAP"))) {
			fprintf(stderr,"UPS: Warning: Corrupted message received.\n");
			errcount--;
			continue;
		}
		if (EOF==sscanf(ptr+6,"%d",&bat)) {
			fprintf(stderr,"UPS: Warning: Corrupted message received.\n");
			errcount--;
			continue;
		}
		// Check power out
		if (!(ptr=strstr(ptr,"Vout"))) {
			fprintf(stderr,"UPS: Warning: Corrupted message received.\n");
			errcount--;
			continue;
		}
		if (EOF==sscanf(ptr+4,"%d",&vlt)) {
			fprintf(stderr,"UPS: Warning: Corrupted message received.\n");
			errcount--;
			continue;
		}
		// Update battery status
		if (vlt<5200) stat=3;
		else if (ext) {
			if (bat==100) stat=4;
			else stat=1;
		}
		else stat=2;
		// Update charge/discharge time estimation
		if (stat!=stat_l) {
			time(&start_time);
			start_percent=bat;
			etchg=etdsc=-1;
		}
		else if (bat!=bat_l) {
			time(&cur_time);
			cur_percent=bat;
			if (start_time!=cur_time&&abs(start_percent-cur_percent)>1) {
				if (ext) {
					etchg = (100-bat)*(cur_time-start_time)/(cur_percent-start_percent-1);
					etdsc = -1;
				}
				else {
					etchg = -1;
					etdsc = bat*(cur_time-start_time)/(start_percent-cur_percent-1);
				}
			}
		}

		// Move current values to last records.
		if (stat_l!=stat) {
			write(out_stat,BATSTAT[stat],strlen(BATSTAT[stat]));
			stat_l=stat;
		}
		if (bat_l!=bat) {
			sprintf(wbuf,"%d",bat);
			write(out_bat,wbuf,strlen(wbuf));
			bat_l=bat;
		}
		if (etchg_l!=etchg) {
			sprintf(wbuf,"%d",etchg);
			write(out_etchg,wbuf,strlen(wbuf));
			etchg_l=etchg;
		}
		if (etdsc_l!=etdsc) {
			sprintf(wbuf,"%d",etdsc);
			write(out_etdsc,wbuf,strlen(wbuf));
			etdsc_l=etdsc;
		}
		if (ext_l!=ext) {
			sprintf(wbuf,"%d",ext);
			write(out_ext,wbuf,strlen(wbuf));
			ext_l=ext;
		}
		if (vlt_l!=vlt) {
			sprintf(wbuf,"%d",vlt);
			write(out_vlt,wbuf,strlen(wbuf));
			vlt_l=vlt;
		}
		errcount=5;
	}
	close(out_stat);
	close(out_etchg);
	close(out_etdsc);
	close(out_ext);
	close(out_vlt);
	close(serial);
	fprintf(stderr,"UPS: Exited after 5 continuous communication failures.\n");
	return 0;
}
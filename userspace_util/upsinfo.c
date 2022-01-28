#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int set_interface_attribs(int fd,int speed,int parity){
	struct termios tty;
	if (tcgetattr(fd,&tty)!=0){
		printf("Error %d: from tcgetattr.\n",errno);
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
		printf("Error %d: from tcsetattr.\n",errno);
		return -1;
	}
	return 0;
}

void set_blocking(int fd,int should_block){
	struct termios tty;
	memset(&tty,0,sizeof tty);
	if (tcgetattr(fd,&tty)!=0){
		printf("Error %d: from tggetattr.\n",errno);
		return;
	}

	tty.c_cc[VMIN] = should_block?1:0;
	tty.c_cc[VTIME] = 5;// 0.5 seconds read timeout

	if (tcsetattr(fd,TCSANOW,&tty)!=0)
		printf("Error %d: setting term attributes.\n",errno);
}

int main(int argc,char *argv[]){
	if (argc!=3) {
		printf("Invalid arguments!\n");
		return(-1);
	}

	int serial = open(argv[1],O_RDWR|O_NOCTTY|O_SYNC);
	if (serial < 0){
		printf("Error %d opening %s: %s\n",errno,argv[1],strerror(errno));
		return -1;
	}

	set_interface_attribs(serial,B9600,0);  // set speed to 115,200 bps, 8n1 (no parity)
	set_blocking(serial,1);                // set blocking

	//usleep ((7 + 25) * 100);             // sleep enough to transmit the 7 plus receive 25: approx 100 uS per char transmit
	char buf[100];
	int n = 0;

	char *bat,*vout;
	char strout[30];

	FILE *out;
	while (1) {
		n = 0;
		memset(buf,0,sizeof(buf));
		memset(strout,0,sizeof(strout));
		while (buf[0]!='$') read(serial,buf,sizeof(buf));
		while (buf[strlen(buf)-1]!='$' && buf[strlen(buf)-1]!='\n') n += read(serial,buf+n,sizeof(buf)-n);
		out = fopen(argv[2],"w");
		if (!out) {
			printf("Error %d opening %s: %s\n",errno,argv[2],strerror(errno));
			close(serial);
			return -2;
		}

		// Printing raw input.
//		for (int i=0;i<n;i++)
//			printf("%c",buf[i]);

		// Constructing output string.
		if (strstr(buf,"Vin GOOD")) {
			if (strstr(buf,"BATCAP 100")) strcpy(strout,"Charged(");
			else strcpy(strout,"Charging(");
		}
		else strcpy(strout,"Discharging(");
		bat=strstr(buf,"BATCAP")+7;
		vout=strstr(buf,"Vout");
		strncat(strout,bat,vout-bat-1);
		strcat(strout,"%,");
		strncat(strout,vout+5,strstr(buf," $")-vout-5);
		strcat(strout,"mV)\n");
		fprintf(out,"%s",strout);
		fclose(out);
	}
	close(serial);
	return 0;
}
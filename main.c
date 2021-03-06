// VNC server using dispmanx
// TODO: mouse support with inconsistency between fbdev and dispmanx

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/time.h>
#include <unistd.h>
#include "bcm_host.h"
#include <assert.h>

#define BPP      2

#ifndef ALIGN_UP
#define ALIGN_UP(x,y)  ((x + (y)-1) & ~((y)-1))
#endif

/* 15 frames per second (if we can) */
#define MAX_TIMEOUT (1.0/4.0)
#define MIN_TIMEOUT (1.0/15.0)

double adaptiveTimeout = MIN_TIMEOUT;

DISPMANX_DISPLAY_HANDLE_T   display;
DISPMANX_RESOURCE_HANDLE_T  resource;
DISPMANX_MODEINFO_T         info;
void                       *image;
void 			   *back_image;

int padded_width;
int pitch;
int r_x0, r_y0, r_x1, r_y1;

/* for compatibility of non-android systems */
#ifndef ANDROID
# define KEY_SOFT1 KEY_UNKNOWN
# define KEY_SOFT2 KEY_UNKNOWN
# define KEY_CENTER	KEY_UNKNOWN
# define KEY_SHARP KEY_UNKNOWN
# define KEY_STAR KEY_UNKNOWN
#endif


jmp_buf env;
int ufile;
int mouse_last = 0;

int relative_mode = 0;
int last_x;
int last_y;

inline double getTime() {
	static struct timeval now = {0,0};
	gettimeofday(&now, NULL);
	return now.tv_sec + (now.tv_usec/1000000.0);
}

/*
* throttle camera updates
*/
int TimeToTakePicture() {
	static double now = 0, then = 0;
	double elapsed = 0;

	now = getTime();
	elapsed = now - then;

	if (elapsed > adaptiveTimeout) {
		then=now;
	}
	return elapsed > adaptiveTimeout;
}

/*
* simulate grabbing a picture from some device
*/
int TakePicture(unsigned char *buffer)
{
	static int last_line=0, fps=0, fcount=0;
	int line=0;
	int i,j;
	struct timeval now;
	int found;

	VC_IMAGE_TRANSFORM_T	transform = 0;
	VC_RECT_T			rect;

	double start = getTime();
	vc_dispmanx_snapshot(display, resource, transform);

	vc_dispmanx_rect_set(&rect, 0, 0, info.width, info.height);
	vc_dispmanx_resource_read_data(resource, &rect, image, pitch); 
	double stop = getTime();

	
	unsigned short *image_p = (unsigned short *)image;
	unsigned short *buffer_p = (unsigned short *)buffer;
	
	// find y0, y1
	unsigned short *back_image_p = (unsigned short *)back_image;

	unsigned long *image_lp = (unsigned long *)image_p;
	unsigned long *buffer_lp = (unsigned long *)buffer_p;
	unsigned long *back_image_lp = (unsigned long*)back_image_p;

	int lp_padding = padded_width >> 1;
	double a_start = getTime();

	r_y0 = info.height-1;
	r_y1 = 0;
	r_x0 = info.width-1;
	r_x1 = 0;

	for (i=0; i<info.height-1; i++)
	{
		for (j = 0; j<lp_padding; j++) {
			if (back_image_lp[(i*lp_padding) + j] - image_lp[(i*lp_padding) + j])
			{
				r_y0 = i;
				goto y0break;
			}
		}
	}

y0break:

	for (i=info.height-1; i>=r_y0; i--)
	{
		for (j = 0; j<lp_padding; j++) {
			if (back_image_lp[(i*lp_padding) + j] - image_lp[(i*lp_padding) + j])
			{
				r_y1 = i+1;		
				goto y1break;
			}
		}
	}

y1break:
	r_x0 = 0;
	r_x1 = info.width-1;
/*	

	for (i=0; i<lp_padding; i++)
	{
		for (j = r_y0; j<(lp_padding*r_y1); j+=lp_padding) {
			if (back_image_lp[j+i] - image_lp[j+i])
			{
				r_x0 = i<<1;
				break;
			}
		}
		if(r_x0 != 0) {
			break;
		}
	}

	for (i=0; i<lp_padding; i++)
	{
		for (j = r_y1; j>=(lp_padding*r_y0); j-=lp_padding) {
			if (back_image_lp[j+i] - image_lp[j+i])
			{
				r_x1 = i<<1;
				break;
			}
		}
		if(r_x1 != 0) {
			break;
		}
	}
*/
	unsigned long mask = 0x0;
	double a_stop = getTime();
	if(fcount % 2 == 0) {
		mask = 0xaaaaaaaa;
	} else {
		mask = 0x55555555;
	}


	if(r_y0 <= r_y1) {
		for(j=r_y0;j<r_y1;j++) {
			for(i=r_x0>>1;i<r_x1>>1;i++) {
				register unsigned long tbi = image_lp[i + (j*lp_padding)]; 
				buffer_lp[i + (j*lp_padding)] = tbi; // | mask;
			}
		}
	} else {
		r_y1 = r_y0;
	}

	double b_stop = getTime();

	void *tmp_image = back_image;
	back_image = image;
	image = tmp_image;

	//fprintf(stderr, "x0=%d, y0=%d, x1=%d, y1=%d              \n", r_x0, r_y0, r_x1, r_y1); 
	//fprintf(stderr,"%03d fps - lines %3d dispmanx %f bounding box %f blitting %f \r",fps, r_y1 - r_y0, (stop - start)*1000.0,(a_stop - a_start)*1000.0, (b_stop - a_stop)*1000.0);

	/*
	* simulate the passage of time
	*
	* draw a simple black line that moves down the screen. The faster the
	* client, the more updates it will get, the smoother it will look!
	*/
	gettimeofday(&now,NULL);
	line = now.tv_usec / (1000000/info.height);
	if (line>info.height) line=info.height-1;
	//memset(&buffer[(info.width * BPP) * line], 0, (info.width * BPP));
	// frames per second (informational only) 
	fcount++;

	if (last_line > line) {
		fps = fcount;
		fcount = 0;
	}
	last_line = line;
	//fprintf(stderr,"%03d/%03d Picture (%03d fps)\r", line, info.height, fps);

//	fprintf(stderr, "x0=%d, y0=%d, x1=%d, y1=%d              \r", r_x0, r_y0, r_x1, r_y1); 
	/* success!   We have a new picture! */
	return r_y0!=r_y1;
}

void sig_handler(int signo)
{
	longjmp(env, 1);
}

void initUinput()
{
	struct uinput_user_dev   uinp;
	int retcode, i;


	ufile = open("/dev/uinput", O_WRONLY | O_NDELAY );
	printf("open /dev/uinput returned %d.\n", ufile);
	if (ufile == 0) {
		printf("Could not open uinput.\n");
		exit(-1);
	}

	memset(&uinp, 0, sizeof(uinp));
	strncpy(uinp.name, "VNCServer SimKey", 20);
	uinp.id.version = 4;
	uinp.id.bustype = BUS_USB;

	if (!relative_mode) {
		uinp.absmin[ABS_X] = 0;
		uinp.absmax[ABS_X] = info.width;
		uinp.absmin[ABS_Y] = 0;
		uinp.absmax[ABS_Y] = info.height;
	}

	ioctl(ufile, UI_SET_EVBIT, EV_KEY);

	for (i=0; i<KEY_MAX; i++) { //I believe this is to tell UINPUT what keys we can make?
		ioctl(ufile, UI_SET_KEYBIT, i);
	}

	ioctl(ufile, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(ufile, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(ufile, UI_SET_KEYBIT, BTN_MIDDLE);

	if (relative_mode) {
		ioctl(ufile, UI_SET_EVBIT, EV_REL);
		ioctl(ufile, UI_SET_RELBIT, REL_X);
		ioctl(ufile, UI_SET_RELBIT, REL_Y);
	}
	else {
		ioctl(ufile, UI_SET_EVBIT, EV_ABS);
		ioctl(ufile, UI_SET_ABSBIT, ABS_X);
		ioctl(ufile, UI_SET_ABSBIT, ABS_Y);
	}

	retcode = write(ufile, &uinp, sizeof(uinp));
	printf("First write returned %d.\n", retcode);

	retcode = (ioctl(ufile, UI_DEV_CREATE));
	printf("ioctl UI_DEV_CREATE returned %d.\n", retcode);
	if (retcode) {
		printf("Error create uinput device %d.\n", retcode);
		exit(-1);
	}
}

static int keysym2scancode(rfbKeySym key)
{

	int scancode = 0;

	int code = (int) key;
	if (code>='0' && code<='9') {
		printf("keysym: digit");

		scancode = (code & 0xF) - 1;
		if (scancode<0) scancode += 10;
		scancode += KEY_1;
	} else if (code>=0xFF50 && code<=0xFF58) {
		printf("keysym: arrow");

		static const uint16_t map[] =
		{  KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
		KEY_PAGEUP, KEY_PAGEDOWN, KEY_END, 0 };
		scancode = map[code & 0xF];
	} else if (code>=0xFFE1 && code<=0xFFEE) {
		printf("keysym: modifier");

		static const uint16_t map[] =
		{  KEY_LEFTSHIFT, KEY_LEFTSHIFT,
		KEY_LEFTCTRL, KEY_LEFTCTRL,
		KEY_LEFTSHIFT, KEY_LEFTSHIFT,
		0,0,
		KEY_LEFTALT, KEY_RIGHTALT,
		0, 0, 0, 0 };
		scancode = map[code & 0xF];
	} else if ((code>='A' && code<='Z') || (code>='a' && code<='z')) {
		printf("keysym: char");

		static const uint16_t map[] = {
			KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
			KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
			KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
			KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
			KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
		scancode = map[(code & 0x5F) - 'A'];
	} else {
		printf("keysym: special");

		switch (code) {
			case XK_space:    scancode = KEY_SPACE;       break;

			case XK_exclam: scancode = KEY_1; break;
			case XK_at:     scancode         = KEY_2; break;
			case XK_numbersign:    scancode      = KEY_3; break;
			case XK_dollar:    scancode  = KEY_4; break;
			case XK_percent:    scancode = KEY_5; break;
			case XK_asciicircum:    scancode     = KEY_6; break;
			case XK_ampersand:    scancode       = KEY_7; break;
			case XK_asterisk:    scancode        = KEY_8; break;
			case XK_parenleft:    scancode       = KEY_9; break;
			case XK_parenright:    scancode      = KEY_0; break;
			case XK_minus:    scancode   = KEY_MINUS; break;
			case XK_underscore:    scancode      = KEY_MINUS; break;
			case XK_equal:    scancode   = KEY_EQUAL; break;
			case XK_plus:    scancode    = KEY_EQUAL; break;
			case XK_BackSpace:    scancode       = KEY_BACKSPACE; break;
			case XK_Tab:    scancode             = KEY_TAB; break;

			case XK_braceleft:    scancode        = KEY_LEFTBRACE;     break;
			case XK_braceright:    scancode       = KEY_RIGHTBRACE;     break;
			case XK_bracketleft:    scancode      = KEY_LEFTBRACE;     break;
			case XK_bracketright:    scancode     = KEY_RIGHTBRACE;     break;
			case XK_Return:    scancode  = KEY_ENTER;     break;

			case XK_semicolon:    scancode        = KEY_SEMICOLON;     break;
			case XK_colon:    scancode    = KEY_SEMICOLON;     break;
			case XK_apostrophe:    scancode       = KEY_APOSTROPHE;     break;
			case XK_quotedbl:    scancode         = KEY_APOSTROPHE;     break;
			case XK_grave:    scancode    = KEY_GRAVE;     break;    
			case XK_asciitilde:    scancode       = KEY_GRAVE;     break;
			case XK_backslash:    scancode        = KEY_BACKSLASH;     break;
			case XK_bar:    scancode              = KEY_BACKSLASH;     break;

			case XK_comma:    scancode    = KEY_COMMA;      break;
			case XK_less:    scancode     = KEY_COMMA;      break;
			case XK_period:    scancode   = KEY_DOT;      break;
			case XK_greater:    scancode  = KEY_DOT;      break;
			case XK_slash:    scancode    = KEY_SLASH;      break;
			case XK_question:    scancode         = KEY_SLASH;      break;
			case XK_Caps_Lock:    scancode        = KEY_CAPSLOCK;      break;

			case XK_F1:    scancode               = KEY_F1; break;
			case XK_F2:    scancode               = KEY_F2; break;
			case XK_F3:    scancode               = KEY_F3; break;
			case XK_F4:    scancode               = KEY_F4; break;
			case XK_F5:    scancode               = KEY_F5; break;
			case XK_F6:    scancode               = KEY_F6; break;
			case XK_F7:    scancode               = KEY_F7; break;
			case XK_F8:    scancode               = KEY_F8; break;
			case XK_F9:    scancode               = KEY_F9; break;
			case XK_F10:    scancode              = KEY_F10; break;
			case XK_Num_Lock:    scancode         = KEY_NUMLOCK; break;
			case XK_Scroll_Lock:    scancode      = KEY_SCROLLLOCK; break;

			case XK_Page_Down:    scancode        = KEY_PAGEDOWN; break;
			case XK_Insert:    scancode   = KEY_INSERT; break;
			case XK_Delete:    scancode   = KEY_DELETE; break;
			case XK_Page_Up:    scancode  = KEY_PAGEUP; break;
			case XK_Escape:    scancode   = KEY_ESC; break;
			case 0x0003:    scancode = KEY_CENTER;      break;
			default: 
				printf("Unknown key.");
				break;
		}
	}
	printf("keysym: %04X -> %04X\n", key, scancode);

	return scancode;
}

static void doptr(int buttonMask, int x, int y, rfbClientPtr cl)
{
	struct input_event       event;

	//printf("mouse: 0x%x at %d,%d\n", buttonMask, x,y);


	memset(&event, 0, sizeof(event));
	gettimeofday(&event.time, NULL);
	if (relative_mode) {
		event.type = EV_REL;
		event.code = REL_X;
		event.value = x - last_x;
	}
	else {
		event.type = EV_ABS;
		event.code = ABS_X;
		event.value = x;
	}
	write(ufile, &event, sizeof(event));

	memset(&event, 0, sizeof(event));
	gettimeofday(&event.time, NULL);
	if (relative_mode) {
		event.type = EV_REL;
		event.code = REL_Y;
		event.value = y - last_y;
	}
	else {
		event.type = EV_ABS;
		event.code = ABS_Y;
		event.value = y;
	}
	write(ufile, &event, sizeof(event));

	last_x = x;
	last_y = y;

	memset(&event, 0, sizeof(event));
	gettimeofday(&event.time, NULL);
	event.type = EV_SYN;
	event.code = SYN_REPORT; 
	event.value = 0;
	write(ufile, &event, sizeof(event));
	if (mouse_last != buttonMask) {
		int left_l = mouse_last & 0x1;
		int left_w = buttonMask & 0x1;

		if (left_l != left_w) {
			memset(&event, 0, sizeof(event));
			gettimeofday(&event.time, NULL);
			event.type = EV_KEY;
			event.code = BTN_LEFT;
			event.value = left_w;
			write(ufile, &event, sizeof(event));

			memset(&event, 0, sizeof(event));
			gettimeofday(&event.time, NULL);
			event.type = EV_SYN;
			event.code = SYN_REPORT; 
			event.value = 0;
			write(ufile, &event, sizeof(event));
		}

		int middle_l = mouse_last & 0x2;
		int middle_w = buttonMask & 0x2;

		if (middle_l != middle_w) {
			memset(&event, 0, sizeof(event));
			gettimeofday(&event.time, NULL);
			event.type = EV_KEY;
			event.code = BTN_MIDDLE;
			event.value = middle_w >> 1;
			write(ufile, &event, sizeof(event));

			memset(&event, 0, sizeof(event));
			gettimeofday(&event.time, NULL);
			event.type = EV_SYN;
			event.code = SYN_REPORT; 
			event.value = 0;
			write(ufile, &event, sizeof(event));
		}
		int right_l = mouse_last & 0x4;
		int right_w = buttonMask & 0x4;

		if (right_l != right_w) {
			memset(&event, 0, sizeof(event));
			gettimeofday(&event.time, NULL);
			event.type = EV_KEY;
			event.code = BTN_RIGHT;
			event.value = right_w >> 2;
			write(ufile, &event, sizeof(event));

			memset(&event, 0, sizeof(event));
			gettimeofday(&event.time, NULL);
			event.type = EV_SYN;
			event.code = SYN_REPORT; 
			event.value = 0;
			write(ufile, &event, sizeof(event));
		}

		mouse_last = buttonMask;
	}	
}

static void dokey(rfbBool down,rfbKeySym key,rfbClientPtr cl)
{

	struct input_event       event;

	if(down) {

		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_KEY;
		event.code = keysym2scancode(key); //nomodifiers!
		event.value = 1; //key pressed
		write(ufile, &event, sizeof(event));

		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_SYN;
		event.code = SYN_REPORT; //not sure what this is for? i'm guessing its some kind of sync thing?
		event.value = 0;
		write(ufile, &event, sizeof(event));


	} else {
		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_KEY;
		event.code = keysym2scancode(key); //nomodifiers!
		event.value = 0; //key realeased
		write(ufile, &event, sizeof(event));

		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_SYN;
		event.code = SYN_REPORT; //not sure what this is for? i'm guessing its some kind of sync thing?
		event.value = 0;
		write(ufile, &event, sizeof(event));


	}
}

int main(int argc, char *argv[])
{
	VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;

	uint32_t                    vc_image_ptr;

	int             		ret, end, x;
	long usec;

	uint32_t        screen = 0;
 	long throttlectr = 0, submitcounter = 0;

	for (x=1; x<argc; x++) {
		if (strcmp(argv[x], "-r")==0)
			relative_mode = 1;
		if (strcmp(argv[x], "-a")==0)
			relative_mode = 0;
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		fprintf(stderr, "error setting sighandler\n");
		exit(-1);
	}

	bcm_host_init();

	printf("Open display[%i]...\n", screen );
	display = vc_dispmanx_display_open( screen );

	ret = vc_dispmanx_display_get_info(display, &info);
	assert(ret == 0);

	/* DispmanX expects buffer rows to be aligned to a 32 bit boundarys */
	pitch = ALIGN_UP(2 * info.width, 32);
	padded_width = pitch/BPP;

	printf( "Display is %d x %d\n", info.width, info.height );

	image = calloc( 1, pitch * info.height );

	assert(image);

	back_image = calloc( 1, pitch * info.height );

	assert(back_image);

	r_x0 = r_y0 = 0;
	r_x1 = info.width;
	r_y1 = info.height;
	resource = vc_dispmanx_resource_create( type,
		info.width,
		info.height,
		&vc_image_ptr );

	last_x = padded_width / 2;
	last_y = info.height / 2;

	rfbScreenInfoPtr server=rfbGetScreen(&argc,argv,padded_width,info.height,5,3,BPP);
	if(!server)
		return 0;
	server->desktopName = "VNC server via dispmanx";
	server->frameBuffer=(char*)malloc(pitch*info.height);
	server->alwaysShared=(1==1);
	server->kbdAddEvent = dokey;
	server->ptrAddEvent = doptr;
	server->serverFormat.redShift = 11;
	server->serverFormat.blueShift = 0;
	server->serverFormat.greenShift = 6;
	printf("Server bpp:%d\n", server->serverFormat.bitsPerPixel);
	printf("Server bigEndian:%d\n", server->serverFormat.bigEndian);
	printf("Server redShift:%d\n", server->serverFormat.redShift);
	printf("Server blueShift:%d\n", server->serverFormat.blueShift);
	printf("Server greeShift:%d\n", server->serverFormat.greenShift);
	printf("server->deferUpdateTime %d\n", server->deferUpdateTime);
	/* Initialize the server */
	rfbInitServer(server);

	end = setjmp(env);
	if (end != 0) goto end;  


	initUinput();

	/* Loop, processing clients and taking pictures */
	while (rfbIsActive(server)) {
		if (TimeToTakePicture()) {
			if (TakePicture((unsigned char *)server->frameBuffer)) {
				rfbMarkRectAsModified(server,r_x0,r_y0,r_x1, r_y1);
				submitcounter++;
			} else {
				submitcounter--;
				if(submitcounter<0) {
					submitcounter=0;
				}
			}
		} 

		// Nasty experiments... 
	/*	throttlectr++;
		if(submitcounter > 1) {
			if(submitcounter>20) {
				adaptiveTimeout+=MIN_TIMEOUT;
				if(adaptiveTimeout>MAX_TIMEOUT) {
					adaptiveTimeout = MAX_TIMEOUT;
				}
				submitcounter-=20;
				throttlectr=0;
			}
			//submitcounter = 0;
			if(throttlectr > 100) {
				adaptiveTimeout-=MIN_TIMEOUT;
				if(adaptiveTimeout<MIN_TIMEOUT) {
					adaptiveTimeout=MIN_TIMEOUT;
				}
				throttlectr=0;
			}
		} else {
			if(throttlectr>100) {
				adaptiveTimeout+=MIN_TIMEOUT;
				if(adaptiveTimeout>MAX_TIMEOUT) {
					adaptiveTimeout=MAX_TIMEOUT;
				}
				throttlectr=0;
			}
		}*/
	//	fprintf(stderr, " %d timer %f            \r", submitcounter, adaptiveTimeout);

		usec = server->deferUpdateTime*1000;
		rfbProcessEvents(server,usec);
	}

end:

	// destroy the device
	ioctl(ufile, UI_DEV_DESTROY);
	close(ufile);

	ret = vc_dispmanx_resource_delete( resource );
	assert( ret == 0 );
	ret = vc_dispmanx_display_close(display );
	assert( ret == 0 );
	printf("\nDone\n");

	return 0;

}


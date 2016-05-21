#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <stdbool.h>
#include <devtty.h>
#include <device.h>
#include "devfd.h"
#include <vt.h>
#include <tty.h>
#include <devdw.h>
#include <ttydw.h>
#include <graphics.h>

#undef  DEBUG			/* UNdefine to delete debug code sequences */

uint8_t *uart_data = (uint8_t *)0xFF04;		/* ACIA data */
uint8_t *uart_status = (uint8_t *)0xFF05;	/* ACIA status */
uint8_t *uart_command = (uint8_t *)0xFF06;	/* ACIA command */
uint8_t *uart_control = (uint8_t *)0xFF07;	/* ACIA control */

unsigned char tbuf1[TTYSIZ];
unsigned char tbuf2[TTYSIZ];
unsigned char tbuf3[TTYSIZ];   /* drivewire VSER 0 */
unsigned char tbuf4[TTYSIZ];   /* drivewire VWIN 0 */

struct s_queue ttyinq[NUM_DEV_TTY + 1] = {	/* ttyinq[0] is never used */
	{NULL, NULL, NULL, 0, 0, 0},
	{tbuf1, tbuf1, tbuf1, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf2, tbuf2, tbuf2, TTYSIZ, 0, TTYSIZ / 2},
	/* Drivewire Virtual Serial Ports */
	{tbuf3, tbuf3, tbuf3, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf4, tbuf4, tbuf4, TTYSIZ, 0, TTYSIZ / 2},
};

uint8_t vtattr_cap = VTA_INVERSE|VTA_UNDERLINE|VTA_ITALIC|VTA_BOLD|
		     VTA_OVERSTRIKE|VTA_NOCURSOR;

static uint8_t vmode;
static uint8_t kbd_timer;
struct vt_repeat keyrepeat = { 40, 4 };

/* tty1 is the screen tty2 is the serial port */

/* Output for the system console (kprintf etc) */
void kputchar(char c)
{
	if (c == '\n')
		tty_putc(1, '\r');
	tty_putc(1, c);
}

ttyready_t tty_writeready(uint8_t minor)
{
	uint8_t c;
	if (minor == 1)
		return TTY_READY_NOW;
	c = *uart_status;
	return (c & 16) ? TTY_READY_NOW : TTY_READY_SOON; /* TX DATA empty */
}

/* For DragonPlus we should perhaps support both monitors 8) */

void tty_putc(uint8_t minor, unsigned char c)
{
	if (minor > 2 ) {
		dw_putc(minor, c);
		return;
	}
	if (minor == 1) {
		/* We don't do text except in 256x192 resolution modes */
		if (vmode < 2)
			vtoutput(&c, 1);
	} else
		*uart_data = c;	/* Data */
}

void tty_sleeping(uint8_t minor)
{
    used(minor);
}

/* 6551 mode handling */

static uint8_t baudbits[] = {
	0x11,		/* 50 */
	0x12,		/* 75 */
	0x13,		/* 110 */
	0x14,		/* 134 */
	0x15,		/* 150 */
	0x16,		/* 300 */
	0x17,		/* 600 */
	0x18,		/* 1200 */
	0x1A,		/* 2400 */
	0x1C,		/* 4800 */
	0x1E,		/* 9600 */
	0x1F,		/* 19200 */
};

static uint8_t bitbits[] = {
	0x30,
	0x20,
	0x10,
	0x00
};

void tty_setup(uint8_t minor)
{
	uint8_t r;
	if (minor > 2) {
		dw_vopen(minor);
		return;
	}
	if (minor != 2)
		return;
	r = ttydata[2].termios.c_cflag & CBAUD;
	if (r > B19200) {
		r = 0x1F;	/* 19.2 */
		ttydata[2].termios.c_cflag &=~CBAUD;
		ttydata[2].termios.c_cflag |= B19200;
	} else
		r = baudbits[r];
	r |= bitbits[(ttydata[2].termios.c_cflag & CSIZE) >> 4];
	*uart_control = r;
	r = 0x0A;	/* rx and tx on, !rts low, dtr int off, no echo */
	if (ttydata[2].termios.c_cflag & PARENB) {
		if (ttydata[2].termios.c_cflag & PARODD)
			r |= 0x20;	/* Odd parity */
		else
			r |= 0x60;	/* Even parity */
		if (ttydata[2].termios.c_cflag & PARMRK)
			r |= 0x80;	/* Mark/space */
	}
}

int tty_carrier(uint8_t minor)
{
	if (minor > 2) return dw_carrier( minor );
	/* The serial DCD is status bit 5 but not wired */
	return 1;
}

void tty_interrupt(void)
{
	uint8_t r = *uart_status;
	if (r & 0x8) {
		r = *uart_data;
		tty_inproc(2,r);
	}	
}

uint8_t keymap[8];
static uint8_t keyin[8];
static uint8_t keybyte, keybit;
static uint8_t newkey;
static int keysdown = 0;
/* FIXME: shouldn't COCO shiftmask also differ ??? 0x02 not 0x40 ?? */
static uint8_t shiftmask[8] = {
	0, 0x40, 0, 0, 0, 0, 0, 0x40
};

static uint8_t rbit[8] = {
	0xFE,
	0xFD,
	0xFB,
	0xF7,
	0xEF,
	0xDF,
	0xBF,
	0x7F,
};

/* Row inputs: multiplexed with the joystick */
static volatile uint8_t *pia_row = (uint8_t *)0xFF00;
/* Columns for scanning: multiplexed with the printer port */
static volatile uint8_t *pia_col = (uint8_t *)0xFF02;
/* Control */
static volatile uint8_t *pia_ctrl = (uint8_t *)0xFF03;

static void keyproc(void)
{
	int i;
	uint8_t key;

	for (i = 0; i < 8; i++) {
		/* We do the scan in software on the Dragon */
		*pia_col = rbit[i];
		keyin[i] = ~*pia_row;
		key = keyin[i] ^ keymap[i];
		if (key) {
			int n;
			int m = 1;
			for (n = 0; n < 7; n++) {
				if ((key & m) && (keymap[i] & m)) {
					if (!(shiftmask[i] & m))
						keysdown--;
				}
				if ((key & m) && !(keymap[i] & m)) {
					if (!(shiftmask[i] & m)) {
						keysdown++;
						newkey = 1;
						keybyte = i;
						keybit = n;
					}
				}
				m += m;
			}
		}
		keymap[i] = keyin[i];
	}
	if (system_id && keybit != 6) { 	/* COCO series */
	  keybit += 2;
	  if (keybit > 5)
	    keybit -= 6;
        }
}

#ifdef CONFIG_COCO_KBD
uint8_t keyboard[8][7] = {
	{ '@', 'h', 'p', 'x', '0', '8', KEY_ENTER },
	{ 'a', 'i', 'q', 'y', '1', '9', 0 /* clear - used as ctrl*/ },
	{ 'b', 'j', 'r', 'z', '2', ':', KEY_ESC /* break (used for esc) */ },
	{ 'c', 'k', 's', '^' /* up */, '3', ';' , 0 /* NC */ },
	{ 'd', 'l', 't', '|' /* down */, '4', ',', 0 /* NC */ },
	{ 'e', 'm', 'u', KEY_BS /* left */, '5', '-', 0 /* NC */ },
	{ 'f', 'n', 'v', KEY_TAB /* right */, '6', '.', 0 /* NC */ },
	{ 'g', 'o', 'w', ' ', '7', '/', 0 /* shift */ },
};

uint8_t shiftkeyboard[8][7] = {
	{ '\\', 'H', 'P', 'X', '_', '(', KEY_ENTER },
	{ 'A', 'I', 'Q', 'Y', '!', ')', 0 /* clear - used as ctrl */ },
	{ 'B', 'J', 'R', 'Z', '"', '*', CTRL('C') /* break */ },
	{ 'C', 'K', 'S', '[' /* up */, '#', '+', 0 /* NC */ },
	{ 'D', 'L', 'T', ']' /* down */, '$', '<', 0 /* NC */ },
	{ 'E', 'M', 'U', '{' /* left */, '%', '=', 0 /* NC */ },
	{ 'F', 'N', 'V', '}' /* right */, '&', '>', 0 /* NC */ },
	{ 'G', 'O', 'W', ' ', '\'', '?', 0 /* shift */ },
};
#else
uint8_t keyboard[8][7] = {
	{ '0', '8', '@', 'h', 'p', 'x', KEY_ENTER },
	{ '1', '9', 'a', 'i', 'q', 'y', 0 /* clear - used as ctrl*/ },
	{ '2', ':', 'b', 'j', 'r', 'z', KEY_ESC /* break (used for esc) */ },
	{ '3', ';', 'c', 'k', 's', '^' /* up */, 0 /* NC */ },
	{ '4', ',', 'd', 'l', 't', '|' /* down */, 0 /* NC */ },
	{ '5', '-', 'e', 'm', 'u', KEY_BS /* left */, 0 /* NC */ },
	{ '6', '.', 'f', 'n', 'v', KEY_TAB /* right */, 0 /* NC */ },
	{ '7', '/', 'g', 'o', 'w', ' ', 0 /* shift */ },
};

uint8_t shiftkeyboard[8][7] = {
	{ '_', '(', '\\', 'H', 'P', 'X', KEY_ENTER },
	{ '!', ')', 'A', 'I', 'Q', 'Y', 0 /* clear - used as ctrl*/ },
	{ '"', '*', 'B', 'J', 'R', 'Z', CTRL('C') /* break */ },
	{ '#', '+', 'C', 'K', 'S', '[' /* up */, 0 /* NC */ },
	{ '$', '<', 'D', 'L', 'T', ']' /* down */, 0 /* NC */ },
	{ '%', '=', 'E', 'M', 'U', '{' /* left */, 0 /* NC */ },
	{ '&', '>', 'F', 'N', 'V', '}' /* right */, 0 /* NC */ },
	{ '\'', '?', 'G', 'O', 'W', ' ', 0 /* shift */ },
};
#endif /* COCO_KBD */

static void keydecode(void)
{
	uint8_t c;

	if (keymap[7] & 64)	/* shift */
		c = shiftkeyboard[keybyte][keybit];
	else
		c = keyboard[keybyte][keybit];
	if (keymap[1] & 64) {	/* control */
		if (c > 31 && c < 127)
			c &= 31;
	}
	tty_inproc(1, c);
}

void platform_interrupt(void)
{
	uint8_t i = *pia_ctrl;
	if (i & 0x80) {
		*pia_col;
		newkey = 0;
		keyproc();
		if (keysdown && keysdown < 3) {
			if (newkey) {
				keydecode();
				kbd_timer = keyrepeat.first;
			} else if (! --kbd_timer) {
				keydecode();
				kbd_timer = keyrepeat.continual;
			}
		}
                fd_timer_tick();
		timer_interrupt();
		dw_vpoll();
	}
}

/* This is used by the vt asm code, but needs to live at the top of the kernel */
uint16_t cursorpos;

static struct display display[4] = {
	/* Two variants of 256x192 with different palette */
	/* 256 x 192 */
	{
		0,
		256, 192,
		256, 192,
		0xFF, 0xFF,		/* For now */
		FMT_MONO_WB,
		HW_UNACCEL,
		GFX_TEXT|GFX_MAPPABLE|GFX_VBLANK|GFX_MULTIMODE,
		0,
		GFX_DRAW|GFX_READ|GFX_WRITE,
	},
	{
		1,
		256, 192,
		256, 192,
		0xFF, 0xFF,		/* For now */
		FMT_MONO_WB,
		HW_UNACCEL,
		GFX_TEXT|GFX_MAPPABLE|GFX_VBLANK|GFX_MULTIMODE,
		0,
		GFX_DRAW|GFX_READ|GFX_WRITE,
	},
	/* 128 x 192 four colour modes */
	{
		2,
		128, 192,
		128, 192,
		0xFF, 0xFF,		/* For now */
		FMT_COLOUR4,
		HW_UNACCEL,
		GFX_MAPPABLE|GFX_VBLANK,
		0,
		GFX_DRAW|GFX_READ|GFX_WRITE|GFX_MULTIMODE,
	},
	{
		3,
		128, 192,
		128, 192,
		0xFF, 0xFF,		/* For now */
		FMT_COLOUR4,
		HW_UNACCEL,
		GFX_MAPPABLE|GFX_VBLANK,
		0,
		GFX_DRAW|GFX_READ|GFX_WRITE|GFX_MULTIMODE,
	},
	/* Possibly we should also allow for SG6 and SG4 ?? */
};

static struct videomap displaymap = {
	0,
	0,
	VIDEO_BASE,
	6 * 1024,
	0,
	0,
	0,
	MAP_FBMEM|MAP_FBMEM_SIMPLE
};

/* bit 7 - A/G 6 GM2 5 GM1 4 GM0 & INT/EXT 3 CSS */
static uint8_t piabits[] = { 0xF0, 0xF8, 0xE0, 0xE8};
///* V0 V1 V2 */
//static uint8_t sambits[] = { 0x6, 0x6, 0x6, 0x6 };



#define pia1b	((volatile uint8_t *)0xFF22)
#define sam_v	((volatile uint8_t *)0xFFC0)

static int gfx_draw_op(uarg_t arg, char *ptr, uint8_t *buf)
{
  int l;
  int c = 8;	/* 4 x uint16_t */
  uint16_t *p = (uint16_t *)buf;
  l = ugetw(ptr);
  if (l < 6 || l > 512)
    return EINVAL;
  if (arg != GFXIOC_READ)
    c = l;
  if (uget(buf, ptr + 2, c))
    return EFAULT;
  switch(arg) {
  case GFXIOC_DRAW:
    /* TODO
    if (draw_validate(ptr, l, 256, 192))  - or 128!
      return EINVAL */
    video_cmd(buf);
    break;
  case GFXIOC_WRITE:
  case GFXIOC_READ:
    if (l < 8)
      return EINVAL;
    l -= 8;
    if (p[0] > 191 || p[1] > 31 || p[2] > 191 || p[3] > 31 ||
      p[0] + p[2] > 192 || p[1] + p[3] > 32 ||
      (p[2] * p[3]) > l)
      return -EFAULT;
    if (arg == GFXIOC_READ) {
      video_read(buf);
      if (uput(buf + 8, ptr + 10, l - 2))
        return EFAULT;
      return 0;
    }
    video_write(buf);
  }
  return 0;
}

int gfx_ioctl(uint8_t minor, uarg_t arg, char *ptr)
{
	if (minor > 2)	/* remove once DW get its own ioctl() */
		return tty_ioctl(minor, arg, ptr);
	if (arg >> 8 != 0x03)
		return vt_ioctl(minor, arg, ptr);
	switch(arg) {
	case GFXIOC_GETINFO:
		return uput(&display[vmode], ptr, sizeof(struct display));
	case GFXIOC_MAP:
		return uput(&displaymap, ptr, sizeof(displaymap));
	case GFXIOC_UNMAP:
		return 0;
	case GFXIOC_GETMODE:
	case GFXIOC_SETMODE:
	{
		uint8_t m = ugetc(ptr);
//		uint8_t b;
		if (m > 3) {
			udata.u_error = EINVAL;
			return -1;
		}
		if (arg == GFXIOC_GETMODE)
			return uput(&display[m], ptr, sizeof(struct display));
		vmode = m;
		*pia1b = (*pia1b & 0x07) | piabits[m];
//		b = sambits[m];
//		sam_v[b & 1] = 0;
//		sam_v[(b & 2)?3:2] = 0;
//		sam_v[(b & 4)?5:4] = 0;
		return 0;
	}
	case GFXIOC_WAITVB:
		/* Our system clock is our vblank, use the standard timeout
		   to pause for one clock */
		udata.u_ptab->p_timeout = 2;
		psleep(NULL);
		return 0;
	case GFXIOC_DRAW:
	case GFXIOC_READ:
	case GFXIOC_WRITE:
	{
		uint8_t *tmp;
		int err;

		tmp = (uint8_t *)tmpbuf();
		err = gfx_draw_op(arg, ptr, tmp);
		brelse((bufptr) tmp);
		if (err) {
			udata.u_error = err;
			err = -1;
		}
		return err;
	}
	}
	return -1;
}

/* A wrapper for tty_close that closes the DW port properly */
int my_tty_close(uint8_t minor)
{
	if (minor > 2 && ttydata[minor].users == 1)
		dw_vclose(minor);
	return (tty_close(minor));
}

/* based on gui-agent/vmside.c */

#include <stdint.h>
#include <xen/io/fbif.h>
#include <xen/io/kbdif.h>
#include <semaphore.h>
#include <sched.h>
#include <hw/hw.h>
#include <hw/pc.h>
#include <console.h>

#include <mm.h>
#include <hw/xenfb.h>
#include <fbfront.h>
#include <sysemu.h>

#include <qubes_gui_qemu.h>
#include <messages.h>
#include <shm_cmd.h>
#include <libvchan.h>
#include <txrx.h>
#include <double_buffer.h>
#include <qlimits.h>

struct QubesGuiState *qs;

#define QUBES_MAIN_WINDOW 1

static void *vga_vram;

static unsigned char linux2scancode[KEY_MAX + 1];
static DisplayChangeListener *dcl;

extern int double_buffered;

extern uint32_t vga_ram_size;


void process_pv_update(QubesGuiState *qs,
			   int x, int y, int width, int height)
{
	struct msg_shmimage mx;
	struct msghdr hdr;

	hdr.type = MSG_SHMIMAGE;
	hdr.window = QUBES_MAIN_WINDOW;
	mx.x = x;
	mx.y = y;
	mx.width = width;
	mx.height = height;
	write_message(hdr, mx);
}


void qubes_create_window(QubesGuiState *qs, int w, int h)
{
	struct msghdr hdr;
	struct msg_create crt;
	int ret;
	
	// the following hopefully avoids missed damage events
	hdr.type = MSG_CREATE;
	hdr.window = QUBES_MAIN_WINDOW;
	crt.width = w;
	crt.height = h;
	crt.parent = 0;
	crt.x = 0;
	crt.y = 0;
	crt.override_redirect = 0;
	write_struct(hdr);
	write_struct(crt);
}

void send_pixmap_mfns(QubesGuiState *qs)
{
	struct shm_cmd shmcmd;
	struct msghdr hdr;
	int ret, rcvd = 0;
	uint32_t *mfns;
    int n = vga_ram_size / XC_PAGE_SIZE;
	int i;
	void *data;
	int offset,copy_offset;

    if (!(qs->ds->surface->flags & QEMU_ALLOCATED_FLAG)) {
		data = ((void *) ds_get_data(qs->ds));
	} else {
		data = qs->nonshared_vram;
	}

	offset = (long)data & (XC_PAGE_SIZE - 1);

    mfns = malloc(n * sizeof(*mfns));
    for (i = 0; i < n; i++)
        mfns[i] = virtual_to_mfn(data + i * XC_PAGE_SIZE);
	hdr.type = MSG_MFNDUMP;
	hdr.window = QUBES_MAIN_WINDOW;
	shmcmd.width = ds_get_width(qs->ds);
	shmcmd.height = ds_get_height(qs->ds);
	shmcmd.num_mfn = n;
	shmcmd.off = offset;
	shmcmd.bpp = ds_get_bits_per_pixel(qs->ds);
	write_message(hdr, shmcmd);
	write_data((char *) mfns, n  * sizeof(*mfns));
    free(mfns);
}

void send_wmname(QubesGuiState *qs, const char *wmname)
{
	struct msghdr hdr;
	struct msg_wmname msg;
	strncpy(msg.data, wmname, sizeof(msg.data));
	hdr.window = QUBES_MAIN_WINDOW;
	hdr.type = MSG_WMNAME;
	write_message(hdr, msg);
}

void send_wmhints(QubesGuiState *qs)
{
	struct msghdr hdr;
	struct msg_window_hints msg;
	long supplied_hints;

	// pass only some hints
	msg.flags = (PMinSize | PMaxSize);
	msg.min_width = ds_get_width(qs->ds);
	msg.min_height = ds_get_height(qs->ds);
	msg.max_width = ds_get_width(qs->ds);
	msg.max_height = ds_get_height(qs->ds);
	hdr.window = QUBES_MAIN_WINDOW;
	hdr.type = MSG_WINDOW_HINTS;
	write_message(hdr, msg);
}

void send_map(QubesGuiState *qs)
{
	struct msghdr hdr;
	struct msg_map_info map_info;

	map_info.override_redirect = 0;
	map_info.transient_for = 0;
	hdr.type = MSG_MAP;
	hdr.window = QUBES_MAIN_WINDOW;
	write_struct(hdr);
	write_struct(map_info);
}

void process_pv_resize(QubesGuiState *qs, int width, int height, int linesize)
{
	struct msghdr hdr;
	struct msg_configure conf;
	if (qs->log_level > 1)
		fprintf(stderr,
			"handle resize  w=%d h=%d\n", width, height);
	hdr.type = MSG_CONFIGURE;
	hdr.window = QUBES_MAIN_WINDOW;
	conf.x = qs->x;
	conf.y = qs->y;
	conf.width = width;
	conf.height = height;
	conf.override_redirect = 0;
	write_struct(hdr);
	write_struct(conf);
	send_pixmap_mfns(qs);
	send_wmhints(qs);
}

void handle_configure(QubesGuiState *qs)
{
	struct msg_configure r;
	read_data((char *) &r, sizeof(r));
	fprintf(stderr,
		"configure msg, x/y %d %d (was %d %d), w/h %d %d\n",
		r.x, r.y, qs->x, qs->y, r.width, r.height);

	qs->x = r.x;
	qs->y = r.y;
}

/* currently unused */
void send_clipboard_data(char *data, int len)
{
	struct msghdr hdr;
	hdr.type = MSG_CLIPBOARD_DATA;
	if (len > MAX_CLIPBOARD_SIZE)
		hdr.window = MAX_CLIPBOARD_SIZE;
	else
		hdr.window = len;
	write_struct(hdr);
	write_data((char *) data, len);
}

void handle_keypress(QubesGuiState *qs)
{
	struct msg_keypress key;
	int scancode;
	
	read_data((char *) &key, sizeof(key));

	scancode = key.keycode;
	scancode = linux2scancode[key.keycode];
	fprintf(stderr, "Received keycode %d(0x%x), converted to %d(0x%x)\n", key.keycode, key.keycode, scancode, scancode);
	if (!scancode) {
		fprintf(stderr, "Can't convert keycode %x to scancode\n", key.keycode);
		return;
	}
	if (scancode & 0x80) {
		kbd_put_keycode(0xe0);
		scancode &= 0x7f;
	}
	if (key.type != KeyPress)
		scancode |= 0x80;
	kbd_put_keycode(scancode);
}

void handle_button(QubesGuiState *qs)
{
	struct msg_button key;
	int ret;
	int button = 0;

	read_data((char *) &key, sizeof(key));
	if (qs->log_level > 1)
		fprintf(stderr,
			"send buttonevent, type=%d button=%d\n",
			(int) key.type, key.button);

	if (key.button == Button1)
		button = MOUSE_EVENT_LBUTTON;
	else if (key.button == Button3)
		button = MOUSE_EVENT_RBUTTON;
	else if (key.button == Button2)
		button = MOUSE_EVENT_MBUTTON;

	if (button) {
		if (key.type == ButtonPress)
			qs->buttons |=  button;
		else
			qs->buttons &= ~button;
		if (kbd_mouse_is_absolute())
			kbd_mouse_event(
					qs->x * 0x7FFF / (ds_get_width(qs->ds) - 1),
					qs->y * 0x7FFF / (ds_get_height(qs->ds) - 1),
					0,
					qs->buttons);
		else
			kbd_mouse_event(0, 0, 0, qs->buttons);
	} else {
		fprintf(stderr, "send buttonevent: unknown button %d\n", key.button);
	}
}

void handle_motion(QubesGuiState *qs)
{
	struct msg_motion key;
	int ret;
	int new_x, new_y;

	read_data((char *) &key, sizeof(key));
	new_x = key.x;
	new_y = key.y;

	if (new_x >= ds_get_width(qs->ds))
		new_x = ds_get_width(qs->ds) - 1;
	if (new_y >= ds_get_height(qs->ds))
		new_y = ds_get_height(qs->ds) - 1;
	if (kbd_mouse_is_absolute()) {
		kbd_mouse_event(
				new_x * 0x7FFF / (ds_get_width(qs->ds) - 1),
				new_y * 0x7FFF / (ds_get_height(qs->ds) - 1),
				0, /* TODO? */
				qs->buttons);
	} else {
		kbd_mouse_event(
				new_x - qs->x,
				new_y - qs->y,
				0, /* TODO? */
				qs->buttons);
	}
	qs->x = new_x;
	qs->y = new_y;
}



void handle_clipboard_data(QubesGuiState *qs, int len)
{

	if (qs->clipboard_data)
		free(qs->clipboard_data);
	// qubes_guid will not bother to send len==-1, really
	qs->clipboard_data = malloc(len + 1);
	if (!qs->clipboard_data) {
		perror("malloc");
		return;
	}
	qs->clipboard_data_len = len;
	read_data((char *) qs->clipboard_data, len);
	qs->clipboard_data[len] = 0;
}

void send_protocol_version()
{   
	uint32_t version = QUBES_GUID_PROTOCOL_VERSION;
	write_struct(version);
}


/* end of based on gui-agent/vmside.c */

static void qubesgui_pv_update(DisplayState *ds, int x, int y, int w, int h)
{
    QubesGuiState *qs = ds->opaque;
	if (!qs->init_done)
		return;
    process_pv_update(qs, x, y, w, h);
}

static void qubesgui_pv_resize(DisplayState *ds)
{
    QubesGuiState *qs = ds->opaque;

    fprintf(stderr,"resize to %dx%d@%d, %d required\n", ds_get_width(ds), ds_get_height(ds), ds_get_bits_per_pixel(ds), ds_get_linesize(ds));
	if (!qs->init_done)
		return;

    process_pv_resize(qs, ds_get_width(ds), ds_get_height(ds), ds_get_linesize(ds));
}

static void qubesgui_pv_setdata(DisplayState *ds)
{
    QubesGuiState *qs = ds->opaque;

	if (!qs->init_done)
		return;
	process_pv_resize(qs, ds_get_width(ds), ds_get_height(ds), ds_get_linesize(ds));
}

static void qubesgui_pv_refresh(DisplayState *ds)
{
    vga_hw_update();
}

static void qubesgui_message_handler(void *opaque)
{
#define KBD_NUM_BATCH 64
    union xenkbd_in_event buf[KBD_NUM_BATCH];
    int n, i;
    QubesGuiState *qs = opaque;
    DisplayState *s = qs->ds;
    static int buttons;
    static int x, y;
	struct msghdr hdr;
	char discard[256];


	vchan_handler_called();
	if (!qs->init_done) {
		qubesgui_init_connection(qs);
		goto out;
	}
	write_data(NULL, 0);    // trigger write of queued data, if any present
	if (read_ready() == 0) {
		goto out; // no data
	}
	read_data((char *) &hdr, sizeof(hdr));

	switch (hdr.type) {
		case MSG_KEYPRESS:
			handle_keypress(qs);
			break;
		case MSG_MAP:
			//ignore
			read_data(discard, sizeof(struct msg_map_info));
			break;
		case MSG_CLOSE:
			//ignore
			// no additional data
			break;
		case MSG_CROSSING:
			//ignore
			read_data(discard, sizeof(struct msg_crossing));
			break;
		case MSG_FOCUS:
			//ignore
			read_data(discard, sizeof(struct msg_focus));
			break;
		case MSG_EXECUTE:
			//ignore
			read_data(discard, sizeof(struct msg_execute));
			break;
		case MSG_BUTTON:
			handle_button(qs);
			break;
		case MSG_MOTION:
			handle_motion(qs);
			break;
		case MSG_CLIPBOARD_REQ:
			// TODO ?
			break;
		case MSG_CLIPBOARD_DATA:
			handle_clipboard_data(qs, hdr.window);
			break;
		case MSG_KEYMAP_NOTIFY:
			// TODO ?
			//ignore
			read_data(discard, sizeof(struct msg_keymap_notify));
			break;
		case MSG_CONFIGURE:
			handle_configure(qs);
			break;
		default:
			fprintf(stderr, "got unknown msg type %d\n", hdr.type);
			return;
	}
out:
	// allow the handler to be called again
	vchan_unmask_channel();
}

static void kbd_init_scancodes()
{
    int scancode, keycode;
#if 0
	// original version
    for (scancode = 0; scancode < 128; scancode++) {
        keycode = atkbd_set2_keycode[atkbd_unxlate_table[scancode]];
        linux2scancode[keycode] = scancode;
        keycode = atkbd_set2_keycode[atkbd_unxlate_table[scancode] | 0x80];
        linux2scancode[keycode] = scancode | 0x80;
    }
#else
	// magic XXX
    for (keycode = 0; keycode < 128; keycode++) {
		linux2scancode[keycode] = keycode-8;
    }
#endif

}


static DisplaySurface* qubesgui_create_displaysurface(int width, int height)
{
    DisplaySurface *surface = (DisplaySurface*) qemu_mallocz(sizeof(DisplaySurface));
    if (surface == NULL) {
        fprintf(stderr, "qubesgui_create_displaysurface: malloc failed\n");
        exit(1);
    }

    surface->width = width;
    surface->height = height;
    surface->linesize = width * 4;
    surface->pf = qemu_default_pixelformat(32);
#ifdef WORDS_BIGENDIAN
    surface->flags = QEMU_ALLOCATED_FLAG | QEMU_BIG_ENDIAN_FLAG;
#else
    surface->flags = QEMU_ALLOCATED_FLAG;
#endif
    surface->data = qs->nonshared_vram;

    return surface;
}

static DisplaySurface* qubesgui_resize_displaysurface(DisplaySurface *surface,
                                          int width, int height)
{
    surface->width = width;
    surface->height = height;
    surface->linesize = width * 4;
    surface->pf = qemu_default_pixelformat(32);
#ifdef WORDS_BIGENDIAN
    surface->flags = QEMU_ALLOCATED_FLAG | QEMU_BIG_ENDIAN_FLAG;
#else
    surface->flags = QEMU_ALLOCATED_FLAG;
#endif
    surface->data = qs->nonshared_vram;

    return surface;
}

static void qubesgui_free_displaysurface(DisplaySurface *surface)
{
    if (surface == NULL)
        return;
    qemu_free(surface);
}

static void qubesgui_pv_display_allocator(void)
{
    DisplaySurface *ds;
    DisplayAllocator *da = qemu_mallocz(sizeof(DisplayAllocator));
    da->create_displaysurface = qubesgui_create_displaysurface;
    da->resize_displaysurface = qubesgui_resize_displaysurface;
    da->free_displaysurface = qubesgui_free_displaysurface;
    if (register_displayallocator(qs->ds, da) != da) {
        fprintf(stderr, "qubesgui_pv_display_allocator: could not register DisplayAllocator\n");
        exit(1);
    }

    qs->nonshared_vram = qemu_memalign(XC_PAGE_SIZE, vga_ram_size);
    if (!qs->nonshared_vram) {
        fprintf(stderr, "qubesgui_pv_display_allocator: could not allocate nonshared_vram\n");
        exit(1);
    }
    /* Touch the pages before sharing them */
    memset(qs->nonshared_vram, 0xff, vga_ram_size);

    ds = qubesgui_create_displaysurface(ds_get_width(qs->ds), ds_get_height(qs->ds));
    defaultallocator_free_displaysurface(qs->ds->surface);
    qs->ds->surface = ds;
}

int qubesgui_pv_display_init(DisplayState *ds)
{
    int i;

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    qs = qemu_mallocz(sizeof(QubesGuiState));
    if (!qs)
        return -1;

    qs->ds = ds;
	qs->init_done = 0;
	qs->init_state = 0;

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    qubesgui_pv_display_allocator();

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
	kbd_init_scancodes();

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    dcl = qemu_mallocz(sizeof(DisplayChangeListener));
    if (!dcl)
        exit(1);
    ds->opaque = qs;
    dcl->dpy_update = qubesgui_pv_update;
    dcl->dpy_resize = qubesgui_pv_resize;
    dcl->dpy_setdata = qubesgui_pv_setdata;
    dcl->dpy_refresh = qubesgui_pv_refresh;
	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    register_displaychangelistener(ds, dcl);

	libvchan_server_init(6000);
    qemu_set_fd_handler(vchan_fd(), qubesgui_message_handler, NULL, qs);

    return 0;
}

int qubesgui_pv_display_vram(void *data)
{
    vga_vram = data;
	return 0;
}

void qubesgui_init_connection(QubesGuiState *qs)
{
	struct msg_xconf xconf;

	if (qs->init_state == 0) {
		if (vchan_handle_connected()) {
			perror("vchan_handle_connected");
			return;
		}

		send_protocol_version();
		fprintf(stderr, "qubes_gui/init[%d]: version sent, waiting for xorg conf\n", __LINE__);
		// XXX warning - thread unsafe
		qs->init_state++;
	} 
	if  (qs->init_state == 1) {
		if (!read_ready())
			return;

		read_struct(xconf);
		fprintf(stderr, "qubes_gui/init[%d]: got xorg conf, creating window\n", __LINE__);
		qubes_create_window(qs, ds_get_width(qs->ds), ds_get_height(qs->ds));

		send_map(qs);
		send_wmname(qs, qemu_name);

		fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
		/* process_pv_resize will send mfns */
		process_pv_resize(qs, ds_get_width(qs->ds), ds_get_height(qs->ds), ds_get_linesize(qs->ds));

		qs->init_state++;
		qs->init_done = 1;
	}
}

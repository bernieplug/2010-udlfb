#ifndef FBDISPLAYLINK_H
#define FBDISPLAYLINK_H

#define FB_BPP		16

#define STD_CHANNEL	"\x57\xCD\xDC\xA7\x1C\x88\x5E\x15"	\
			"\x60\xFE\xC6\x97\x16\x3D\x47\xF2"

#define DL_CHIP_TYPE_BASE 0xB
#define DL_CHIP_TYPE_ALEX 0xF
#define DL_CHIP_TYPE_OLLIE 0xF1


#define EDID_GET_WIDTH(edid) ( ( ( (uint16_t) edid->data.pixel_data.hactive_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.hactive_lo )
#define EDID_GET_HEIGHT(edid) ( ( ( (uint16_t) edid->data.pixel_data.vactive_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.vactive_lo )

#define EDID_GET_HBLANK(edid) ( ( ( (uint16_t) edid->data.pixel_data.hblank_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.hblank_lo )
#define EDID_GET_VBLANK(edid) ( ( ( (uint16_t) edid->data.pixel_data.vblank_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.vblank_lo )

#define EDID_GET_HSYNC(edid) ( ( ( (uint16_t) edid->data.pixel_data.hsync_offset_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.hsync_offset_lo )
#define EDID_GET_VSYNC(edid) ( ( ( (uint16_t) edid->data.pixel_data.vsync_offset_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.vsync_offset_lo )

#define EDID_GET_HPULSE(edid) ( ( ( (uint16_t) edid->data.pixel_data.hsync_pulse_width_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.hsync_pulse_width_lo )
#define EDID_GET_VPULSE(edid) ( ( ( (uint16_t) edid->data.pixel_data.vsync_pulse_width_hi ) << 8 ) | (uint16_t) edid->data.pixel_data.vsync_pulse_width_lo )

/* as libdlo */
#define BUF_HIGH_WATER_MARK	1024
#define BUF_SIZE		(64*1024)

struct dlfb_orphaned_device_context {
	atomic_t fb_count;
    struct usb_device *udev;
    struct mutex fb_mutex;
    struct fb_info *info;
    int screen_size;
    int line_length;
};

struct dlfb_device_context {
	// first members of structure must match "orphaned" struct below
	atomic_t fb_count;
	struct usb_device *udev;
    struct mutex fb_mutex;
    int screen_size;
    int line_length;
	
	struct usb_interface *interface;
	struct urb *tx_urb, *ctrl_urb;
	struct usb_ctrlrequest dr;
	struct fb_info *info;
	char *buf;
	char *bufend;
	char *backing_buffer;
	struct mutex bulk_mutex;
	char edid[128];
	char chiptype[8];
	char name[64];
	struct completion done;
	int base16;
	int base16d;
	int base8;
	int base8d;
};


struct dlfb_video_mode {
	uint8_t col;
	uint32_t hclock;
	uint32_t vclock;
	uint8_t unknown1[6];
	uint16_t xres;
	uint8_t unknown2[6];
	uint16_t yres;
	uint8_t unknown3[4];
} __attribute__ ((__packed__));

char *dlfb_set_register(char *bufptr, uint8_t reg, uint8_t val);
int dlfb_bulk_msg(struct dlfb_device_context *dev, int len);
void dlfb_destroy_framebuffer(struct dlfb_device_context *dev);
void dlfb_edid(struct dlfb_device_context *dev);
int dlfb_set_video_mode(struct dlfb_device_context *dev, int mode, int width, int height, int freq);
void dlfb_bulk_callback(struct urb *urb);
int dlfb_setup(struct dlfb_device_context *dev);
int dlfb_activate_framebuffer(struct dlfb_device_context *dev, int mode);

#endif

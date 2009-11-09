/*
 * displaylinkfb.c -- FB driver for DisplayLink USB controller
 *
 * Copyright (C) 2009, Jaya Kumar
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of this archive for
 * more details.
 *
 * Layout is based on skeletonfb by James Simmons and Geert Uytterhoeven,
 * usb-skeleton by GregKH, and is also based on libdlo and
 * Roberto De Ioris's udlfb.
 *
 * The intention of this driver is to provide reduced mem support for
 * DisplayLink devices on embedded systems, and to work with regular user
 * space, standard fbdev applications such as Xfbdev.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/usb.h>

#include "edid.h"

#define NR_USB_REQUEST_I2C_SUB_IO 0x02
#define NR_USB_REQUEST_CHANNEL 0x12

struct dlfb_dev {
       struct usb_device *udev;
       struct usb_interface *interface;
       struct mutex io_mutex; /* synchronize I/O with disconnect */
       struct fb_info *info;
       u32 pseudo_palette[256];
};

#define VEND_DISPLAYLINK       0x17e9
#define PROD_K08               0x0141

static struct usb_device_id dlfb_id_table[] = {
       { USB_DEVICE(VEND_DISPLAYLINK, PROD_K08) },
       { }
};
MODULE_DEVICE_TABLE(usb, dlfb_id_table);

static struct fb_fix_screeninfo dlfb_fix = {
       .id =           "displaylinkfb",
       .type =         FB_TYPE_PACKED_PIXELS,
       .visual =       FB_VISUAL_TRUECOLOR,
       .xpanstep =     0,
       .ypanstep =     0,
       .ywrapstep =    0,
       .accel =        FB_ACCEL_NONE,
};

/*
 * LFSR is linear feedback shift register. The reason we have this is
 * because the display controller needs to minimize the clock depth of
 * various counters used in the display path. So this code, taken from
 * libdlo's dlo_mode.c, reverses the provided value into the lfsr16 value
 * by counting backwards to get the value that needs to be set in the
 * hardware comparator to get the same actual count. This makes sense once
 * you read above a couple of times and think about it from a hardware
 * perspective.
 */
static u16 lfsr16(u16 actual_count)
{
       u32 lv = 0xFFFF; /* This is the lfsr value that the hw starts with */

       while (actual_count--) {
               lv =    ((lv << 1) |
                       (((lv >> 15) ^ (lv >> 4) ^ (lv >> 2) ^ (lv >> 1)) & 1))
                       & 0xFFFF;
       }

       return (u16) lv;
}

/*
 * Inserts a specific DisplayLink controller command into the provided
 * buffer.
 */
static char *insert_command(char *buf, u8 reg, u8 val)
{
       *buf++ = 0xAF;
       *buf++ = 0x20;
       *buf++ = reg;
       *buf++ = val;
       return buf;
}

static char *insert_vidreg_lock(char *buf)
{
       return insert_command(buf, 0xFF, 0x00);
}

static char *insert_vidreg_unlock(char *buf)
{
       return insert_command(buf, 0xFF, 0xFF);
}

/*
 * Once you send this command, the DisplayLink framebuffer gets driven to the
 * display.
 */
static char *insert_enable_hvsync(char *buf)
{
       return insert_command(buf, 0x1F, 0x00);
}

static char *insert_set_color_depth(char *buf, u8 selection)
{
       return insert_command(buf, 0x00, selection);
}

static char *insert_set_base16bpp(char *wrptr, u32 base)
{
       /* the base pointer is 16 bits wide, 0x20 is hi byte. */
       wrptr = insert_command(wrptr, 0x20, base >> 16);
       wrptr = insert_command(wrptr, 0x21, base >> 8);
       return insert_command(wrptr, 0x22, base);
}

static char *insert_set_base8bpp(char *wrptr, u32 base)
{
       wrptr = insert_command(wrptr, 0x26, base >> 16);
       wrptr = insert_command(wrptr, 0x27, base >> 8);
       return insert_command(wrptr, 0x28, base);
}

static char *insert_command_16(char *wrptr, u8 reg, u16 value)
{
       wrptr = insert_command(wrptr, reg, value >> 8);
       return insert_command(wrptr, reg+1, value);
}

/*
 * This is kind of weird because the controller takes some
 * register values in a different byte order than other registers.
 */
static char *insert_command_16be(char *wrptr, u8 reg, u16 value)
{
       wrptr = insert_command(wrptr, reg, value);
       return insert_command(wrptr, reg+1, value >> 8);
}

/*
 * This does LFSR conversion on the value that is to be written.
 * See LFSR explanation at top for more detail.
 */
static char *insert_command_lfsr16(char *wrptr, u8 reg, u16 value)
{
       return insert_command_16(wrptr, reg, lfsr16(value));
}

/*
 * This takes a standard fbdev screeninfo struct and all of its monitor mode
 * details and converts them into the DisplayLink equivalent register commands.
 */
static char *insert_set_vid_cmds(char *wrptr, struct fb_var_screeninfo *var)
{
       u16 xds, yds;
       u16 xde, yde;
       u16 yec;


       /* x display start */
       xds = var->left_margin + var->hsync_len;
       wrptr = insert_command_lfsr16(wrptr, 0x01, xds);
       /* x display end */
       xde = xds + var->xres;
       wrptr = insert_command_lfsr16(wrptr, 0x03, xde);

       /* y display start */
       yds = var->upper_margin + var->vsync_len;
       wrptr = insert_command_lfsr16(wrptr, 0x05, yds);
       /* y display end */
       yde = yds + var->yres;
       wrptr = insert_command_lfsr16(wrptr, 0x07, yde);

       /* x end count is active + blanking - 1 */
       wrptr = insert_command_lfsr16(wrptr, 0x09, xde + var->right_margin - 1);

       /* libdlo hardcodes hsync start to 1 */
       wrptr = insert_command_lfsr16(wrptr, 0x0B, 1);

       /* hsync end is width of sync pulse + 1 */
       wrptr = insert_command_lfsr16(wrptr, 0x0D, var->hsync_len + 1);

       /* hpixels is active pixels */
       wrptr = insert_command_16(wrptr, 0x0F, var->xres);

       /* yendcount is vertical active + vertical blanking */
       yec = var->yres + var->upper_margin + var->lower_margin +
                       var->vsync_len;
       wrptr = insert_command_lfsr16(wrptr, 0x11, yec);

       /* libdlo hardcodes vsync start to 0 */
       wrptr = insert_command_lfsr16(wrptr, 0x13, 0);

       /* vsync end is width of vsync pulse */
       wrptr = insert_command_lfsr16(wrptr, 0x15, var->vsync_len);

       /* vpixels is active pixels */
       wrptr = insert_command_16(wrptr, 0x17, var->yres);

       /* convert picoseconds to 5kHz multiple for pclk5k = x * 1E12/5k */
       wrptr = insert_command_16be(wrptr, 0x1B, 200*1000*1000/var->pixclock);

       return wrptr;
}

/*
 * This is to flag any issues with our writes.
 */
static void dlfb_write_bulk_callback(struct urb *urb)
{
       struct dlfb_dev *dev;

       dev = urb->context;

       /* sync/async unlink faults aren't errors */
       if ((urb->status) && (!(urb->status == -ENOENT ||
                   urb->status == -ECONNRESET ||
                   urb->status == -ESHUTDOWN))) {
               dev_err(&dev->udev->dev, "Problem %d with write bulk.\n",
                               urb->status);
       }

       /* free up our allocated buffer */
       usb_buffer_free(urb->dev, urb->transfer_buffer_length,
                       urb->transfer_buffer, urb->transfer_dma);
}

/*
 * This takes a standard fbdev screeninfo struct that was fetched or prepared
 * and then generates the appropriate command sequence that then drives the
 * display controller.
 */
static int dlfb_set_video_mode(struct dlfb_dev *dev,
                               struct fb_var_screeninfo *var)
{
       struct urb *urb;
       char *buf;
       char *wrptr;
       int retval = 0;
       int writesize, bufsize;

       /*
        * Allocate a buffer to store these series of commands that we'll send
        * to the controller.
        */
       urb = usb_alloc_urb(0, GFP_KERNEL);
       if (!urb) {
               retval = -ENOMEM;
               goto error;
       }

       bufsize = 50 * 4; /* anticipated command count * command size */
       buf = usb_buffer_alloc(dev->udev, bufsize, GFP_KERNEL,
                               &urb->transfer_dma);
       if (!buf) {
               retval = -ENOMEM;
               goto error;
       }

       /*
        * This first section has to do with setting the base address on the
        * controller * associated with the display. There are 2 base
        * pointers, currently, we only * use the 16 bpp segment.
        */
       wrptr = insert_vidreg_lock(buf);
       wrptr = insert_set_color_depth(wrptr, 0x00);
       /* set base for 16bpp segment to 0 */
       wrptr = insert_set_base16bpp(wrptr, 0);
       /* set base for 8bpp segment to end of fb */
       wrptr = insert_set_base8bpp(wrptr, dev->info->fix.smem_len);

       wrptr = insert_set_vid_cmds(wrptr, var);
       wrptr = insert_enable_hvsync(wrptr);
       wrptr = insert_vidreg_unlock(wrptr);

       writesize = wrptr - buf;

       mutex_lock(&dev->io_mutex);
       if (!dev->interface) {          /* disconnect() was called */
               mutex_unlock(&dev->io_mutex);
               retval = -ENODEV;
               goto error_afterbuf;
       }

       /* initialize the urb properly */
       usb_fill_bulk_urb(urb, dev->udev, usb_sndbulkpipe(dev->udev, 1),
                         buf, writesize, dlfb_write_bulk_callback, dev);
       urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

       retval = usb_submit_urb(urb, GFP_KERNEL);
       mutex_unlock(&dev->io_mutex);
       if (retval) {
               dev_err(&dev->udev->dev, "Problem %d with submit write bulk.\n",
                                       retval);
               goto error_afterbuf;
       }

       return 0;

error_afterbuf:
       usb_buffer_free(dev->udev, bufsize, buf, urb->transfer_dma);
error:
       usb_free_urb(urb);
       return retval;
}

/*
 * This is necessary before we can communicate with the display controller.
 */
static int dlfb_select_std_channel(struct dlfb_dev *dev)
{
       int ret;
       u8 set_def_chn[] = {    0x57, 0xCD, 0xDC, 0xA7,
                               0x1C, 0x88, 0x5E, 0x15,
                               0x60, 0xFE, 0xC6, 0x97,
                               0x16, 0x3D, 0x47, 0xF2  };

       ret = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
                       NR_USB_REQUEST_CHANNEL,
                       (USB_DIR_OUT | USB_TYPE_VENDOR), 0, 0,
                       set_def_chn, sizeof(set_def_chn), USB_CTRL_SET_TIMEOUT);
       return ret;
}

/*
 * This function is based on read_edid() from libdlo's dlo_usb.c
 * The general idea is to extract the EDID by sending repeated I2C
 * usb control requests. Once we have extracted the EDID, we hand
 * it off to fbdev's edid parse routine which should give us back
 * a filled in screeninfo structure. If there is any trouble, we
 * just break out and clean up. The caller will then have to find
 * another way to determine screen size and parameters.
 */
static int dlfb_get_var_from_edid(struct dlfb_dev *dev,
                                       struct fb_var_screeninfo *var)
{
       int i;
       char *buf;
       int ret;
       char rbuf[2];

       buf = kmalloc(EDID_LENGTH, GFP_KERNEL);
       if (!buf) {
               ret = -ENOMEM;
               goto error;
       }

       /* this makes sure we're in sync with disconnect */
       mutex_lock(&dev->io_mutex);

       for (i = 0; i < EDID_LENGTH; i++) {
               ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                       NR_USB_REQUEST_I2C_SUB_IO,
                       (USB_DIR_IN | USB_TYPE_VENDOR), i << 8, 0xA1,
                       rbuf, 2, USB_CTRL_GET_TIMEOUT);
               /* if we timed-out, then fail out */
               if (ret < 0)
                       goto usb_ctl_err;
               buf[i] = rbuf[1];
       }

       ret = fb_parse_edid(buf, var);

usb_ctl_err:
       kfree(buf);
       mutex_unlock(&dev->io_mutex);
error:
       return ret;
}

/*
 * We're going to use the controller's stripe24 command to push pixels into
 * the framebuffer. The structure of the command is something like
 * 0xAF, 0x68, 3 bytes representing framebuffer address, 1 byte for length
 * in pixels. So only 255 pixels per stripe24 command. This is sent raw.
 * This can of course be improved upon significantly by putting multiple
 * stripe24 into a single usb buffer and also compressing the actual pixel
 * data.
 */
static int dlfb_draw_stripe(struct dlfb_dev *dev, char *data, int pixelcount)
{
       struct urb *urb;
       char *buf;
       int bufsize;
       int startaddress;
       int retval;
       int i;
       u16 *fbbuf, *fbdata;

       urb = usb_alloc_urb(0, GFP_KERNEL);
       if (!urb) {
               retval = -ENOMEM;
               goto error;
       }

       bufsize = pixelcount * (dev->info->var.bits_per_pixel/8);
       bufsize += 6; /* 6 bytes for the header */
       buf = usb_buffer_alloc(dev->udev, bufsize, GFP_KERNEL,
                               &urb->transfer_dma);
       if (!buf) {
               retval = -ENOMEM;
               goto error;
       }

       startaddress = data - dev->info->screen_base;
       /* and now we prepare the stripe */
       buf[0] = 0xAF;
       buf[1] = 0x68;
       buf[2] = startaddress >> 16;
       buf[3] = startaddress >> 8;
       buf[4] = startaddress;
       buf[5] = pixelcount;

       /* and now we copy the pixels into the command buffer */
#if defined(__BIG_ENDIAN)
       /* if we're bigendian then we can use the optimized memcpy */
       memcpy(buf+6, data, bufsize - 6);
#else
       /* if not, we have to swizzle on little endian */
       fbbuf = (u16 *) (buf + 6);
       fbdata = (u16 *) (data);
       for (i = 0; i < pixelcount; i++)
               fbbuf[i] = cpu_to_be16(fbdata[i]);
#endif

       mutex_lock(&dev->io_mutex);
       if (!dev->interface) {
               mutex_unlock(&dev->io_mutex);
               retval = -ENODEV;
               goto error_afterbuf;
       }

       usb_fill_bulk_urb(urb, dev->udev, usb_sndbulkpipe(dev->udev, 1),
                         buf, bufsize, dlfb_write_bulk_callback, dev);
       urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

       retval = usb_submit_urb(urb, GFP_KERNEL);
       mutex_unlock(&dev->io_mutex);
       if (retval) {
               dev_err(&dev->udev->dev, "Problem %d with submit write bulk.\n",
                                       retval);
               goto error_afterbuf;
       }

       return 0;

error_afterbuf:
       usb_buffer_free(dev->udev, bufsize, buf, urb->transfer_dma);
error:
       usb_free_urb(urb);
       return retval;
}

static int dlfb_dpy_update(struct dlfb_dev *dev)
{
       char *data;
       int i = 0, size;
       int ret = 0;

       data = dev->info->screen_base;
       size = dev->info->fix.smem_len;

       while (size > (255*2)) {
               ret = dlfb_draw_stripe(dev, data + i, 255);
               if (ret)
                       return ret;
               i += 255*2;
               size -= 255*2;
       }
       if (size)
               ret = dlfb_draw_stripe(dev, data + i, size/2);

       return ret;
}

static int dlfb_dpy_update_page(struct dlfb_dev *dev, int index)
{
       char *data;
       int i = 0, size;
       int ret = 0;

       data = dev->info->screen_base + index;
       size = PAGE_SIZE;

       while (size > (255*2)) {
               ret = dlfb_draw_stripe(dev, data + i, 255);
               if (ret)
                       return ret;
               i += 255*2;
               size -= 255*2;
       }
       if (size)
               ret = dlfb_draw_stripe(dev, data + i, size/2);

       return ret;
}

static void dlfb_dpy_deferred_io(struct fb_info *info,
                               struct list_head *pagelist)
{
       struct page *cur;
       struct fb_deferred_io *fbdefio = info->fbdefio;
       struct dlfb_dev *dev = info->par;
       int ret;

       /* walk the written page list and push the data */
       list_for_each_entry(cur, &fbdefio->pagelist, lru) {
               ret = dlfb_dpy_update_page(dev, cur->index << PAGE_SHIFT);
               if (ret) {
                       dev_err(&dev->udev->dev, "Problem %d with update.\n",
                                               ret);
                       break;
               }
       }
}

static void dlfb_fillrect(struct fb_info *info,
                                  const struct fb_fillrect *rect)
{
       struct dlfb_dev *dev = info->par;

       sys_fillrect(info, rect);

       dlfb_dpy_update(dev);
}

static void dlfb_copyarea(struct fb_info *info,
                                  const struct fb_copyarea *area)
{
       struct dlfb_dev *dev = info->par;

       sys_copyarea(info, area);

       dlfb_dpy_update(dev);
}

static void dlfb_imageblit(struct fb_info *info,
                               const struct fb_image *image)
{
       struct dlfb_dev *dev = info->par;

       sys_imageblit(info, image);

       dlfb_dpy_update(dev);
}

/*
 * this is the slow path from userspace. they can seek and write to
 * the fb. it's inefficient to do anything less than a full screen draw
 */
static ssize_t dlfb_write(struct fb_info *info, const char __user *buf,
                               size_t count, loff_t *ppos)
{
       struct dlfb_dev *dev = info->par;
       unsigned long p = *ppos;
       void *dst;
       int err = 0;
       unsigned long total_size;

       if (info->state != FBINFO_STATE_RUNNING)
               return -EPERM;

       total_size = info->fix.smem_len;

       if (p > total_size)
               return -EFBIG;

       if (count > total_size) {
               err = -EFBIG;
               count = total_size;
       }

       if (count + p > total_size) {
               if (!err)
                       err = -ENOSPC;

               count = total_size - p;
       }

       dst = (void __force *) (info->screen_base + p);

       if (copy_from_user(dst, buf, count))
               err = -EFAULT;

       if  (!err)
               *ppos += count;

       dlfb_dpy_update(dev);

       return (err) ? err : count;
}

static int dlfb_setcolreg(unsigned regno, unsigned red, unsigned green,
                       unsigned blue, unsigned transp, struct fb_info *info)
{
       struct dlfb_dev *dev = info->par;
       int err = 0;

       if (regno >= info->cmap.len)
               return 1;

       if (regno < 16) {
               if (info->var.red.offset == 10) {
                       /* 1:5:5:5 */
                       ((u32 *) (dev->pseudo_palette))[regno] =
                           ((red & 0xf800) >> 1) |
                           ((green & 0xf800) >> 6) | ((blue & 0xf800) >> 11);
               } else {
                       /* 0:5:6:5 */
                       ((u32 *) (dev->pseudo_palette))[regno] =
                           ((red & 0xf800)) |
                           ((green & 0xfc00) >> 5) | ((blue & 0xf800) >> 11);
               }
       }

       return err;
}


static struct fb_ops dlfb_ops = {
       .owner          = THIS_MODULE,
       .fb_read        = fb_sys_read,
       .fb_write       = dlfb_write,
       .fb_fillrect    = dlfb_fillrect,
       .fb_copyarea    = dlfb_copyarea,
       .fb_imageblit   = dlfb_imageblit,
       .fb_setcolreg   = dlfb_setcolreg,
};

static struct fb_deferred_io dlfb_defio = {
       .delay          = 10,
       .deferred_io    = dlfb_dpy_deferred_io,
};

static int dlfb_probe(struct usb_interface *interface,
                       const struct usb_device_id *id)
{
       struct device *mydev;
       struct dlfb_dev *dev;
       struct fb_info *info;
       int videomemorysize;
       unsigned char *videomemory;
       int retval = -ENOMEM;
       struct fb_var_screeninfo *var;
       struct fb_bitfield red = { 11, 5, 0 };
       struct fb_bitfield green = { 5, 6, 0 };
       struct fb_bitfield blue = { 0, 5, 0 };

       dev = kzalloc(sizeof(*dev), GFP_KERNEL);
       if (!dev) {
               err("Out of memory");
               goto error;
       }

       mutex_init(&dev->io_mutex);
       dev->udev = usb_get_dev(interface_to_usbdev(interface));
       dev->interface = interface;

       usb_set_intfdata(interface, dev);

       mydev = &dev->udev->dev;
       info = framebuffer_alloc(0, mydev);
       if (!info)
               goto err_fballoc;

       dev->info = info;
       info->par = dev;
       info->pseudo_palette = dev->pseudo_palette;

       var = &info->var;
       retval = dlfb_get_var_from_edid(dev, var);
       if (retval) {
               /* had a problem getting edid. so fallback to 1024x768 */
               dev_err(mydev, "Problem %d with EDID.\n", retval);
               var->xres = 1024;
               var->yres = 768;
       }

       /*
        * ok, now that we've got the size info, we can alloc our framebuffer.
        * We are using 16bpp.
        */
       info->var.bits_per_pixel = 16;
       info->fix = dlfb_fix;
       info->fix.line_length = var->xres * (var->bits_per_pixel / 8);
       videomemorysize = info->fix.line_length * var->yres;
       videomemory = vmalloc(videomemorysize);
       info->fix.smem_len = videomemorysize;
       info->flags = FBINFO_FLAG_DEFAULT;

       if (!videomemory)
               goto err_vidmem;

       memset(videomemory, 0, videomemorysize);

       info->screen_base = videomemory;
       info->fbops = &dlfb_ops;

       var->vmode = FB_VMODE_NONINTERLACED;
       var->red = red;
       var->green = green;
       var->blue = blue;

       /* set offset, length, msb right */

       info->fbdefio = &dlfb_defio;
       fb_deferred_io_init(info);

       retval = fb_alloc_cmap(&info->cmap, 256, 0);
       if (retval < 0) {
               dev_err(mydev, "Failed to allocate colormap\n");
               goto err_cmap;
       }

       dlfb_select_std_channel(dev);
       dlfb_set_video_mode(dev, var);
       dlfb_dpy_update(dev);

       retval = register_framebuffer(info);
       if (retval < 0)
               goto err_regfb;

       dev_info(mydev, "DisplayLink USB device %d now attached, "
                       "using %dK of memory\n", info->node,
                                       videomemorysize >> 10);
       return 0;

err_regfb:
       fb_dealloc_cmap(&info->cmap);
err_cmap:
       fb_deferred_io_cleanup(info);
       vfree(videomemory);
err_vidmem:
       framebuffer_release(info);
err_fballoc:
       usb_set_intfdata(interface, NULL);
       usb_put_dev(dev->udev);
error:
       kfree(dev);
       return retval;
}

static void dlfb_disconnect(struct usb_interface *interface)
{
       struct dlfb_dev *dev;
       struct fb_info *info;

       dev = usb_get_intfdata(interface);
       usb_set_intfdata(interface, NULL);
       usb_put_dev(dev->udev);

       mutex_lock(&dev->io_mutex);
       dev->interface = NULL;
       mutex_unlock(&dev->io_mutex);

       info = dev->info;
       if (info) {
               dev_info(&interface->dev, "Detaching DisplayLink device %d.\n",
                                               info->node);
               unregister_framebuffer(info);
               fb_dealloc_cmap(&info->cmap);
               fb_deferred_io_cleanup(info);
               fb_dealloc_cmap(&info->cmap);
               vfree((void __force *)info->screen_base);
               framebuffer_release(info);
       }
       kfree(dev);
}

static struct usb_driver dlfb_driver = {
       .name           = "DisplayLink USB FrameBuffer",
       .probe          = dlfb_probe,
       .disconnect     = dlfb_disconnect,
       .id_table       = dlfb_id_table,
};

static int __init dlfb_init(void)
{
       return usb_register(&dlfb_driver);
}

static void __exit dlfb_exit(void)
{
       usb_deregister(&dlfb_driver);
}

module_init(dlfb_init);
module_exit(dlfb_exit);

MODULE_DESCRIPTION("fbdev driver for DisplayLink USB controller");
MODULE_AUTHOR("Jaya Kumar");
MODULE_LICENSE("GPL");

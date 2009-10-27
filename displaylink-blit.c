#ifndef DISPLAYLINK
        #include "displaylink.h"
#endif

/* 
displaylink_render_rgb565_hline_rlx()

Render a command stream for an encoded horizontal line segment of pixels.
The fundamental building block of rendering for current Displaylink devices.

A command buffer holds several commands.
It always begins with a fresh command header
(the protocol doesn't require this, but we enforce it to allow
multiple buffers to be potentially encoded and sent in parallel).
A single command encodes one contiguous horizontal line of pixels
In the form of spans of raw and RLE-encoded pixels.

The function relies on the client to do all allocation, so that
rendering can be done directly to output buffers (e.g. USB URBs).
The function fills the supplied command buffer, providing information
on where it left off, so the client may call in again with additional
buffers if the line will take several buffers to complete.
*/

//A single command can transmit a maximum number of pixels,
//regardless of the compression ratio (protocol design limit).
//To the hardware, 0 for a size byte means 256
#define MAX_CMD_PIXELS		255
// sync, cmd, 3 address, write len, raw len, 2 raw pixel bytes
#define MIN_RLX_CMD_BYTES	9

#define MIN(a,b) ((a)>(b)?(b):(a))

void displaylink_render_rgb565_hline_rlx(
  const uint16_t*	*pixel_start_ptr,
  const uint16_t*	const pixel_end,
  uint32_t              *device_address_ptr,
  uint8_t*      	*command_buffer_ptr,
  const uint8_t*	const cmd_end)
{
  const uint16_t*       pixel     = *pixel_start_ptr;
  uint32_t		dev_addr  = *device_address_ptr;
  uint8_t*              cmd       = *command_buffer_ptr;

  while ((pixel_end > pixel) && (cmd_end > cmd + MIN_RLX_CMD_BYTES))
  {
    uint8_t *raw_pixels_count_byte = 0;
    uint8_t *cmd_pixels_count_byte = 0;
    enum { raw, rle } render_state;
    const uint16_t *raw_pixel_start = 0;
    const uint16_t *cmd_pixel_start, *cmd_pixel_end = 0;
    uint16_t pixel_repeat = 0;
    uint32_t be_dev_addr = htonl(dev_addr);
 
    *cmd++ = 0xAF;
    *cmd++ = 0x6B;
    *cmd++ = (uint8_t) ((be_dev_addr >> 8) & 0xFF);
    *cmd++ = (uint8_t) ((be_dev_addr >> 16) & 0xFF);
    *cmd++ = (uint8_t) ((be_dev_addr >> 24) & 0xFF);

    cmd_pixels_count_byte = cmd++;       // we'll know this later
    cmd_pixel_start = pixel;
 
    render_state = raw; // alternating raw/rle spans always start raw
    raw_pixels_count_byte = cmd++; // we'll know this later
    raw_pixel_start = pixel;
	
    cmd_pixel_end = pixel + MIN(pixel_end - pixel, MAX_CMD_PIXELS + 1);

    while ((cmd_pixel_end > pixel) && 
      (cmd_end > cmd + 1)) // Worst case of 2 bytes to encode one pixel
    {
      switch (render_state) {
      case raw:
      {
        uint16_t be_pixel = htons(*pixel);
        
        *cmd++ = (be_pixel) & 0xFF;
	*cmd++ = (be_pixel >> 8) & 0xFF;
	
	// Does this pixel repeat? RAW->RLE->RAW overhead is 2 bytes
	// So just 2 duplicate pixels is a wash, 3 is a win
	if (pixel[0] == pixel[1])
        {
          // finalize length of prior span of raw pixels. +1 for pixel[0]
          *raw_pixels_count_byte = (pixel - raw_pixel_start + 1) & 0xFF;

          // start keeping track of how many times to repeat pixel
          pixel_repeat = 0; 

          render_state = rle;
        }

	pixel++;		
        break;
      }
      case rle:
	pixel_repeat++;
	if (pixel[0] != pixel[1])
        {
          // how sad, our run has ended. Write out pixel repeat count
          *cmd++ = pixel_repeat & 0xFF;

          raw_pixel_start = &pixel[1];
	
	  // hardware expects next byte to be number of raw pixels
          // in the next span.  We don't know that yet, fill in later
          raw_pixels_count_byte = cmd++;

	  render_state = raw;
	}

        pixel++;
        break;
      }
    }

    // We're done or hit a command length or command buffer limit. Finalize
    switch(render_state) {
    case raw:
        *raw_pixels_count_byte = (pixel - raw_pixel_start) & 0xFF;
      break;
    case rle:
        // in rle state, we couldn't have exhausted buffer
        *cmd++ = pixel_repeat;
      break;
    }

    *cmd_pixels_count_byte = (pixel - cmd_pixel_start) & 0xFF; 
    dev_addr += (pixel - cmd_pixel_start) * 2;
  }

  if (cmd_end <= MIN_RLX_CMD_BYTES + cmd) 
  {
    // We don't have room for anything useful. Fill last few
    // bytes with no-ops
    if (cmd_end > cmd)  memset(cmd, 0xAF, cmd_end - cmd);
    cmd = (uint8_t*) cmd_end;
  }

  *command_buffer_ptr = cmd;
  *pixel_start_ptr = pixel;
  *device_address_ptr = dev_addr;
 
  return;
}

// Still using single synchronous URB yet
int
displaylink_image_blit(struct displaylink_dev *dev, int x, int y, int width, int height,
	   char *data)
{
  const int host_pixel_size = 2; // this blit only handles 16bpp 565 source data
  const int device_pixel_size = 2; // and displaylink device (target) of same format
  int i, j, k, ret;
  char *cmd, *cmd_end;

  mutex_lock(&dev->bulk_mutex);

  cmd = dev->buf;
  cmd_end = dev->bufend - BUF_HIGH_WATER_MARK;

  if (dev->udev == NULL) {
    return 0;
  }

  if (width <= 0) {
    return -EINVAL;
  }

  if (x + width > dev->fb_info->var.xres) {
    return -EINVAL;
  }

  if (y + height > dev->fb_info->var.yres) {
    return -EINVAL;
  }

  //printk("IMAGE_BLIT, frame buffer %x - %x\n", (unsigned int) data,
  //  (unsigned int) (data + (dev->line_length*dev->fb_info->var.yres)));

  for (i = y; i < y + height ; i++) {
    const uint16_t *line_start, *line_end, *back_start, *next_pixel;
    uint32_t dev_addr = dev->base16 + (dev->line_length * i) + 
		(x * device_pixel_size);

    //printk("WRITING LINE %d, width %d\n", i, width);

    line_start = (uint16_t *) (data + (dev->line_length * i) + 
		(x * host_pixel_size));
    next_pixel = line_start;
    line_end = &line_start[width+1];
    back_start = (uint16_t *) (dev->backing_buffer + (dev->line_length * i) + 
		(x * host_pixel_size));

    // adjust line_start to first pixel that actually changed
    for (j = 0; j < width; j++) 
    {
      if (back_start[j] != line_start[j])
      {	
        next_pixel = &next_pixel[j];
        dev_addr += j * device_pixel_size;
        break;
      }
    }

    if (j == width) 
    {
      // no actual changes in this line
      next_pixel = line_end;

    } else {

      // adjust line_end to last pixel that actually changed
      for (k = (width-1); k > j; k--) 
      {
	if (back_start[k] != line_start[k])
        {
          line_end = &line_start[k+1];
          break;
	}
      }
    }

    while (next_pixel < line_end) {

      //if (i==y) {
      //  printk("rlx width=%x hstart=%x hend=%x daddr=%x cmd=%x cmd_end=%x\n",
      //    (unsigned int) width, (unsigned int) next_pixel, (unsigned int) line_end, 
      //    (unsigned int) dev_addr, (unsigned int) cmd, (unsigned int) cmd_end);
      //}

      displaylink_render_rgb565_hline_rlx(
	&next_pixel,
	line_end,
	&dev_addr,
	(uint8_t**) &cmd,
	(uint8_t*)  cmd_end);

      if (cmd >= cmd_end) {
        //printk("buffer full. Sending bulk message %d bytes\n", cmd - dev->buf);
	ret = displaylink_bulk_msg(dev, cmd - dev->buf);
	cmd = dev->buf;
      }
    }

    memcpy((char*)back_start, (char*) line_start, width * host_pixel_size);
  }

  if (cmd > dev->buf)
  {
    //printk("blit complete. Sending partial buffer, %d bytes\n", cmd - dev->buf);
    // Send partial buffer remaining before exiting
    ret = displaylink_bulk_msg(dev, cmd - dev->buf);		
  }

  mutex_unlock(&dev->bulk_mutex);

  return 0;
}

/**
 * Hunter Adams (vha3@cornell.edu)
 * 
 * Modified by:
 * Sander Groen (sandergroen@gmail.com)
 * 
 * VGA driver using PIO assembler
 *
 * HARDWARE CONNECTIONS
 *  - GPIO 0 ---> 390 ohm resistor ---> VGA Red
 *  - GPIO 1 ---> 1K ohm resistor ---> VGA Red
 *  - GPIO 2 ---> 390 ohm resistor ---> VGA Green
 *  - GPIO 3 ---> 1K ohm resistor ---> VGA Green
 *  - GPIO 4 ---> 390 ohm resistor ---> VGA Blue
 *  - GPIO 5 ---> 1K ohm resistor ---> VGA Blue
 *  - GPIO 6 ---> VGA Hsync
 *  - GPIO 7 ---> VGA Vsync
 *  - RP2040 GND ---> VGA GND
 *
 * RESOURCES USED
 *  - PIO state machines 0, 1, and 2 on PIO instance 0
 *  - DMA channels 0 and 1
 *  - 614.4 kBytes of RAM (for pixel color data)
 *
 * HOW TO USE THIS CODE
 *  This code uses one DMA channel to send pixel data to a PIO state machine
 *  that is driving the VGA display, and a second DMA channel to reconfigure
 *  and restart the first. As such, changing any value in the pixel color
 *  array will be automatically reflected on the VGA display screen.
 *  
 *  To help with this, I have included a function called drawPixel which takes,
 *  as arguments, a VGA x-coordinate (int), a VGA y-coordinate (int), and a
 *  pixel color (char). Only 6 bits are used for RGB, so there are only 64 possible
 *  colors. If you keep all of the code above line 124, this interface will work.
 *
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hsync.pio.h"
#include "vsync.pio.h"
#include "rgb.pio.h"

#define H_ACTIVE   655 // 640+16-1
#define V_ACTIVE   479 // 480-1
#define RGB_ACTIVE 127 // 640/5-1
#define RED_PIN   0
#define HSYNC     6
#define VSYNC     7
#define TXCOUNT 61440

uint32_t vga_data_array[TXCOUNT];
uint32_t* address_pointer = &vga_data_array[0];

void drawPixel(int x, int y, char color) {
    if (x > 639) x = 639;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (y > 479) y = 479;

    int pixel = ((640 * y) + x);
    // Put 5 pixel values into a single 32-bit integer
    vga_data_array[pixel / 5] |= (color << (24 - ((pixel % 5) * 6)));
}

int main() {
    stdio_init_all();
    PIO pio = pio0;

    uint hsync_offset = pio_add_program(pio, &hsync_program);
    uint vsync_offset = pio_add_program(pio, &vsync_program);
    uint rgb_offset = pio_add_program(pio, &rgb_program);

    uint hsync_sm = 0;
    uint vsync_sm = 1;
    uint rgb_sm = 2;

    hsync_program_init(pio, hsync_sm, hsync_offset, HSYNC);
    vsync_program_init(pio, vsync_sm, vsync_offset, VSYNC);
    rgb_program_init(pio, rgb_sm, rgb_offset, RED_PIN);

    int rgb_chan_0 = 0;
    int rgb_chan_1 = 1;

    dma_channel_config c0 = dma_channel_get_default_config(rgb_chan_0);  // default configs
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);              // 8-bit txfers
    channel_config_set_read_increment(&c0, true);                        // yes read incrementing
    channel_config_set_write_increment(&c0, false);                      // no write incrementing
    channel_config_set_dreq(&c0, DREQ_PIO0_TX2) ;                        // DREQ_PIO0_TX2 pacing (FIFO)
    channel_config_set_chain_to(&c0, rgb_chan_1);                        // chain to other channel

    dma_channel_configure(
        rgb_chan_0,                 // Channel to be configured
        &c0,                        // The configuration we just created
        &pio->txf[rgb_sm],          // write address (RGB PIO TX FIFO)
        &vga_data_array,            // The initial read address (pixel color array)
        TXCOUNT,                    // Number of transfers; in this case each is 1 byte.
        false                       // Don't start immediately.
    );

    // Channel One (reconfigures the first channel)
    dma_channel_config c1 = dma_channel_get_default_config(rgb_chan_1);   // default configs
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);              // 32-bit txfers
    channel_config_set_read_increment(&c1, false);                        // no read incrementing
    channel_config_set_write_increment(&c1, false);                       // no write incrementing
    channel_config_set_chain_to(&c1, rgb_chan_0);                         // chain to other channel

    dma_channel_configure(
        rgb_chan_1,                         // Channel to be configured
        &c1,                                // The configuration we just created
        &dma_hw->ch[rgb_chan_0].read_addr,  // Write address (channel 0 read address)
        &address_pointer,                   // Read address (POINTER TO AN ADDRESS)
        1,                                  // Number of transfers, in this case each is 4 byte
        false                               // Don't start immediately.
    );

    pio_sm_put_blocking(pio, hsync_sm, H_ACTIVE);
    pio_sm_put_blocking(pio, vsync_sm, V_ACTIVE);
    pio_sm_put_blocking(pio, rgb_sm, RGB_ACTIVE);
    pio_enable_sm_mask_in_sync(pio, ((1u << hsync_sm) | (1u << vsync_sm) | (1u << rgb_sm)));
    dma_start_channel_mask((1u << rgb_chan_0)) ;

    while (true) {
        int x = 0 ;
        int y = 0 ;
        int index = 0;
        int xcounter = 0;
        int ycounter = 0;

        for (y=0; y<480; y++) {                   
            if (ycounter==8) {                    
                ycounter = 0 ;                    
                index = (index+1)%64;             
            }                                     
            ycounter += 1;                        
            for (x=0; x<640; x++) {               
                if (xcounter == 10) {             
                    xcounter = 0;                 
                    index = (index + 1) % 64;       
                }                                 
                xcounter += 1;                    
                drawPixel(x, y, index);
            }
        }
    }
}

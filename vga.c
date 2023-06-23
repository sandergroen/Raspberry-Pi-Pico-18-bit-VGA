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
 *  - 224 kBytes of RAM (for pixel color data)
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
// Our assembled programs:
// Each gets the name <pio_filename.pio.h>
#include "hsync.pio.h"
#include "vsync.pio.h"
#include "rgb.pio.h"

// VGA timing constants
#define H_ACTIVE   655    // (active + frontporch - 1) - one cycle delay for mov
#define V_ACTIVE   349    // (active - 1)
#define RGB_ACTIVE 639    // (horizontal active - 1)

// Length of the pixel array, and number of DMA transfers
#define TXCOUNT 224000 // Total pixels/2 (since we have 2 pixels per byte)
// 128*480
// Pixel color array that is DMA's to the PIO machines and
// a pointer to the ADDRESS of this color array.
// Note that this array is automatically initialized to all 0's (black)
unsigned char vga_data_array[TXCOUNT];
char* address_pointer = &vga_data_array[0];

// Give the I/O pins that we're using some names that make sense
#define RED_PIN   0
#define HSYNC     6
#define VSYNC     7

// A function for drawing a pixel with a specified color.
// Note that because information is passed to the PIO state machines through
// a DMA channel, we only need to modify the contents of the array and the
// pixels will be automatically updated on the screen.
void drawPixel(int x, int y, char color) {
    // Range checks
    if (x > 639) x = 639;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (y > 349) y = 349;

    // Which pixel is it?
    int pixel = ((640 * y) + x) ;
    vga_data_array[pixel] = color;
}

int main() {
    // Initialize stdio
    stdio_init_all();

    // Choose which PIO instance to use (there are two instances, each with 4 state machines)
    PIO pio = pio0;

    // Our assembled program needs to be loaded into this PIO's instruction
    // memory. This SDK function will find a location (offset) in the
    // instruction memory where there is enough space for our program. We need
    // to remember these locations!
    //
    // We only have 32 instructions to spend! If the PIO programs contain more than
    // 32 instructions, then an error message will get thrown at these lines of code.
    //
    // The program name comes from the .program part of the pio file
    // and is of the form <program name_program>
    uint hsync_offset = pio_add_program(pio, &hsync_program);
    uint vsync_offset = pio_add_program(pio, &vsync_program);
    uint rgb_offset = pio_add_program(pio, &rgb_program);

    // Manually select a few state machines from pio instance pio0.
    uint hsync_sm = 0;
    uint vsync_sm = 1;
    uint rgb_sm = 2;

    // Call the initialization functions that are defined within each PIO file.
    // Why not create these programs here? By putting the initialization function in
    // the pio file, then all information about how to use/setup that state machine
    // is consolidated in one place. Here in the C, we then just import and use it.
    hsync_program_init(pio, hsync_sm, hsync_offset, HSYNC);
    vsync_program_init(pio, vsync_sm, vsync_offset, VSYNC);
    rgb_program_init(pio, rgb_sm, rgb_offset, RED_PIN);


    /////////////////////////////////////////////////////////////////////////////////////////////////////
    // ===========================-== DMA Data Channels =================================================
    /////////////////////////////////////////////////////////////////////////////////////////////////////

    // DMA channels - 0 sends color data, 1 reconfigures and restarts 0
    int rgb_chan_0 = 0;
    int rgb_chan_1 = 1;

    // Channel Zero (sends color data to PIO VGA machine)
    dma_channel_config c0 = dma_channel_get_default_config(rgb_chan_0);  // default configs
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_8);              // 8-bit txfers
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

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////

    // Initialize PIO state machine counters. This passes the information to the state machines
    // that they retrieve in the first 'pull' instructions, before the .wrap_target directive
    // in the assembly. Each uses these values to initialize some counting registers.
    pio_sm_put_blocking(pio, hsync_sm, H_ACTIVE);
    pio_sm_put_blocking(pio, vsync_sm, V_ACTIVE);
    pio_sm_put_blocking(pio, rgb_sm, RGB_ACTIVE);


    // Start the two pio machine IN SYNC
    // Note that the RGB state machine is running at full speed,
    // so synchronization doesn't matter for that one. But, we'll
    // start them all simultaneously anyway.
    pio_enable_sm_mask_in_sync(pio, ((1u << hsync_sm) | (1u << vsync_sm) | (1u << rgb_sm)));

    // Start DMA channel 0. Once started, the contents of the pixel color array
    // will be continously DMA's to the PIO machines that are driving the screen.
    // To change the contents of the screen, we need only change the contents
    // of that array.
    dma_start_channel_mask((1u << rgb_chan_0)) ;


    /////////////////////////////////////////////////////////////////////////////////////////////////////
    // ===================================== An Example =================================================
    /////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // The remainder of this program is simply an example to show how to use the VGA system.
    // This particular example just produces a diagonal array of colors on the VGA screen.
    
    while (true) {
        int x = 0 ; // VGA x coordinate
        int y = 0 ; // VGA y coordinate

        // Array of colors, and a variable that we'll use to index into the array
        
        int index = 0;

        // A couple of counters
        int xcounter = 0;
        int ycounter = 0;

        for (y=0; y<350; y++) {                     // For each y-coordinate . . .
            if (ycounter==6) {                     //   If the y-counter is 60 . . .
                ycounter = 0 ;                      //     Zero the counter
                index = (index+1)%64;               //     Increment the color index
            }                                       //
            ycounter += 1;                          //   Increment the y-counter
            for (x=0; x<640; x++) {                 //   For each x-coordinate . . .
                if (xcounter == 10) {               //     If the x-counter is 80 . . .
                    xcounter = 0;                   //        Zero the x-counter
                    index = (index + 1)%64;         //        Increment the color index
                }                                   //
                xcounter += 1;                     //     Increment the x-counter
                drawPixel(x, y, index) ;    //     Draw a pixel to the screen
            }
        }
    }
}

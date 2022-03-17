/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef AVBTP_AEM_H
#define AVBTP_AEM_H

#include "packets.h"

#define AVBTP_AEM_DESC_ENTITY			0x0000
#define AVBTP_AEM_DESC_CONFIGURATION		0x0001
#define AVBTP_AEM_DESC_AUDIO_UNIT		0x0002
#define AVBTP_AEM_DESC_VIDEO_UNIT		0x0003
#define AVBTP_AEM_DESC_SENSOR_UNIT		0x0004
#define AVBTP_AEM_DESC_STREAM_INPUT		0x0005
#define AVBTP_AEM_DESC_STREAM_OUTPUT		0x0006
#define AVBTP_AEM_DESC_JACK_INPUT		0x0007
#define AVBTP_AEM_DESC_JACK_OUTPUT		0x0008
#define AVBTP_AEM_DESC_AVB_INTERFACE		0x0009
#define AVBTP_AEM_DESC_CLOCK_SOURCE		0x000a
#define AVBTP_AEM_DESC_MEMORY_OBJECT		0x000b
#define AVBTP_AEM_DESC_LOCALE			0x000c
#define AVBTP_AEM_DESC_STRINGS			0x000d
#define AVBTP_AEM_DESC_STREAM_PORT_INPUT	0x000e
#define AVBTP_AEM_DESC_STREAM_PORT_OUTPUT	0x000f
#define AVBTP_AEM_DESC_EXTERNAL_PORT_INPUT	0x0010
#define AVBTP_AEM_DESC_EXTERNAL_PORT_OUTPUT	0x0011
#define AVBTP_AEM_DESC_INTERNAL_PORT_INPUT	0x0012
#define AVBTP_AEM_DESC_INTERNAL_PORT_OUTPUT	0x0013
#define AVBTP_AEM_DESC_AUDIO_CLUSTER		0x0014
#define AVBTP_AEM_DESC_VIDEO_CLUSTER		0x0015
#define AVBTP_AEM_DESC_SENSOR_CLUSTER		0x0016
#define AVBTP_AEM_DESC_AUDIO_MAP		0x0017
#define AVBTP_AEM_DESC_VIDEO_MAP		0x0018
#define AVBTP_AEM_DESC_SENSOR_MAP		0x0019
#define AVBTP_AEM_DESC_CONTROL			0x001a
#define AVBTP_AEM_DESC_SIGNAL_SELECTOR		0x001b
#define AVBTP_AEM_DESC_MIXER			0x001c
#define AVBTP_AEM_DESC_MATRIX			0x001d
#define AVBTP_AEM_DESC_MATRIX_SIGNAL		0x001e
#define AVBTP_AEM_SIGNAL_SPLITTER		0x001f
#define AVBTP_AEM_SIGNAL_COMBINER		0x0020
#define AVBTP_AEM_SIGNAL_DEMULTIPLEXER		0x0021
#define AVBTP_AEM_SIGNAL_MULTIPLEXER		0x0022
#define AVBTP_AEM_SIGNAL_TRANSCODER		0x0023
#define AVBTP_AEM_CLOCK_DOMAIN			0x0024
#define AVBTP_AEM_CONTROL_BLOCK			0x0025
#define AVBTP_AEM_INVALID			0xffff

#endif /* AVBTP_AEM_H */

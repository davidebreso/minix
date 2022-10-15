/*
ne1000.h

Created:	October 15, 2022 by Davide Bresolin <bresolin.davide@gmail.com>
*/

#ifndef NE1000_H
#define NE1000_H

#define NE_DP8390	0x00
#define NE_DATA		0x10
#define NE_RESET	0x1F

#define NE1000_START	0x2000
#define NE1000_SIZE	0x2000

#define inb_ne(dep, reg) (in_byte(dep->de_base_port+reg))
#define outb_ne(dep, reg, data) (out_byte(dep->de_base_port+reg, data))

#endif /* NE1000_H */


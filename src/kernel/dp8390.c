/*
 * dp8390.c
 *
 * This file contains a ethernet device driver for NS dp8390 based ethernet
 * cards.
 *
 * The valid messages and their parameters are:
 *
 *   m_type	  DL_PORT    DL_PROC   DL_COUNT   DL_MODE   DL_ADDR
 * |------------+----------+---------+----------+---------+---------|
 * | HARDINT	|          |         |          |         |         |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_WRITE	| port nr  | proc nr | count    | mode    | address |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_WRITEV	| port nr  | proc nr | count    | mode    | address |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_READ	| port nr  | proc nr | count    |         | address |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_READV	| port nr  | proc nr | count    |         | address |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_INIT	| port nr  | proc nr | mode     |         | address |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_GETSTAT	| port nr  | proc nr |          |         | address |
 * |------------|----------|---------|----------|---------|---------|
 * | DL_STOP	| port_nr  |         |          |         |	    |
 * |------------|----------|---------|----------|---------|---------|
 *
 * The messages sent are:
 *
 *   m-type	  DL_POR T   DL_PROC   DL_COUNT   DL_STAT   DL_CLCK
 * |------------|----------|---------|----------|---------|---------|
 * |DL_TASK_REPL| port nr  | proc nr | rd-count | err|stat| clock   |
 * |------------|----------|---------|----------|---------|---------|
 *
 *   m_type	  m3_i1     m3_i2       m3_ca1
 * |------------+---------+-----------+---------------|
 * |DL_INIT_REPL| port nr | last port | ethernet addr |
 * |------------|---------|-----------|---------------|
 *
 * Created:	before Dec 28, 1992 by Philip Homburg <philip@cs.vu.nl>
 *
 * Modified Mar 10 1994 by Philip Homburg
 *	Become a generic dp8390 driver.
 *
 * Modified Dec 20 1996 by G. Falzoni <falzoni@marina.scn.de>
 *	Added support for 3c503 boards.
 */

#include "kernel.h"
#include <stdlib.h>
#include <minix/com.h>
#include <net/hton.h>
#include <net/gen/ether.h>
#include <net/gen/eth_io.h>
#include "assert.h"
INIT_ASSERT
#if __minix_vmd
#include "i386/protect.h"
#else
#include "protect.h"
#endif
#include "dp8390.h"
#include "proc.h"
#if __minix_vmd
#include "config.h"
#endif

#if ENABLE_NETWORKING || __minix_vmd

#define DE_PORT_NR	2

static dpeth_t de_table[DE_PORT_NR];
static int int_pending[NR_IRQ_VECTORS];
static int dpeth_tasknr= ANY;
static u16_t eth_ign_proto;

/* Configuration */
typedef struct dp_conf
{
	port_t dpc_port;
	int dpc_irq;
	phys_bytes dpc_mem;
	char *dpc_envvar;
	segm_t dpc_prot_sel;
} dp_conf_t;

dp_conf_t dp_conf[]=	/* Card addresses */
{
	/* I/O port, IRQ,  Buffer address,  Env. var,   Buf selector. */
	{  0x280,     3,    0xD0000,        "DPETH0",  DP_ETH0_SELECTOR },
	{  0x300,     5,    0xCC000,        "DPETH1",  DP_ETH1_SELECTOR },
};

/* Test if dp_conf has exactly DE_PORT_NR entries.  If not then you will see
 * the error: "array size is negative".
 */
extern int ___dummy[DE_PORT_NR == sizeof(dp_conf)/sizeof(dp_conf[0]) ? 1 : -1];


/* Card inits configured out? */
#if !ENABLE_WDETH
#define wdeth_probe(dep)	(0)
#endif
#if !ENABLE_NE2000
#define ne2k_probe(dep)		(0)
#endif
#if !ENABLE_NE1000
#define ne1k_probe(dep)		(0)
#endif
#if !ENABLE_3C503
#define el2_probe(dep)		(0)
#endif


_PROTOTYPE( static void do_vwrite, (message *mp, int from_int,
							int vectored)	);
_PROTOTYPE( static void do_vread, (message *mp, int vectored)		);
_PROTOTYPE( static void do_init, (message *mp)				);
_PROTOTYPE( static void do_int, (dpeth_t *dep)				);
_PROTOTYPE( static void do_getstat, (message *mp)			);
_PROTOTYPE( static void do_stop, (message *mp)				);
_PROTOTYPE( static void dp_init, (dpeth_t *dep)				);
_PROTOTYPE( static void dp_confaddr, (dpeth_t *dep)			);
_PROTOTYPE( static void dp_reinit, (dpeth_t *dep)			);
_PROTOTYPE( static void dp_reset, (dpeth_t *dep)			);
_PROTOTYPE( static void dp_check_ints, (dpeth_t *dep)			);
_PROTOTYPE( static void dp_recv, (dpeth_t *dep)				);
_PROTOTYPE( static void dp_send, (dpeth_t *dep)				);
_PROTOTYPE( static void dp_getblock, (dpeth_t *dep, int page,
				size_t offset, size_t size, void *dst)	);
_PROTOTYPE( static void dp_pio8_getblock, (dpeth_t *dep, int page,
				size_t offset, size_t size, void *dst)	);
_PROTOTYPE( static void dp_pio16_getblock, (dpeth_t *dep, int page,
				size_t offset, size_t size, void *dst)	);
_PROTOTYPE( static int dp_pkt2user, (dpeth_t *dep, int page,
							int length)	);
_PROTOTYPE( static void dp_user2nic, (dpeth_t *dep, iovec_dat_t *iovp, 
		vir_bytes offset, int nic_addr, vir_bytes count)	);
_PROTOTYPE( static void dp_pio8_user2nic, (dpeth_t *dep,
				iovec_dat_t *iovp, vir_bytes offset,
				int nic_addr, vir_bytes count)		);
_PROTOTYPE( static void dp_pio16_user2nic, (dpeth_t *dep,
				iovec_dat_t *iovp, vir_bytes offset,
				int nic_addr, vir_bytes count)		);
_PROTOTYPE( static void dp_nic2user, (dpeth_t *dep, int nic_addr, 
		iovec_dat_t *iovp, vir_bytes offset, vir_bytes count)	);
_PROTOTYPE( static void dp_pio8_nic2user, (dpeth_t *dep, int nic_addr, 
		iovec_dat_t *iovp, vir_bytes offset, vir_bytes count)	);
_PROTOTYPE( static void dp_pio16_nic2user, (dpeth_t *dep, int nic_addr, 
		iovec_dat_t *iovp, vir_bytes offset, vir_bytes count)	);
_PROTOTYPE( static void dp_next_iovec, (iovec_dat_t *iovp)		);
_PROTOTYPE( static int dp_handler, (int irq)				);
_PROTOTYPE( static void conf_hw, (dpeth_t *dep)				);
_PROTOTYPE( static void update_conf, (dpeth_t *dep, dp_conf_t *dcp)	);
_PROTOTYPE( static int calc_iovec_size, (iovec_dat_t *iovp)		);
_PROTOTYPE( static void reply, (dpeth_t *dep, int err, int may_block)	);
_PROTOTYPE( static void mess_reply, (message *req, message *reply)	);
_PROTOTYPE( static void get_userdata, (int user_proc,
		vir_bytes user_addr, vir_bytes count, void *loc_addr)	);
_PROTOTYPE( static void put_userdata, (int user_proc,
		vir_bytes user_addr, vir_bytes count, void *loc_addr)	);

/*===========================================================================*
 *				dpeth_task				     *
 *===========================================================================*/
void dp8390_task()
{
	message m;
	int i, irq, r;
	dpeth_t *dep;
	long v;

	dpeth_tasknr= proc_number(proc_ptr);

	v= 0;
	(void) env_parse("ETH_IGN_PROTO", "x", 0, &v, 0x0000L, 0xFFFFL);
	eth_ign_proto= htons((u16_t) v);

	while (TRUE)
	{
#ifdef SHOW_EVENT
		if (debug)
			show_event(16, ' ');
#endif

		if ((r= receive(ANY, &m)) != OK)
			panic("dp8390: receive failed", r);

#ifdef SHOW_EVENT
		if (debug)
			show_event(16, 'E');
#endif

		switch (m.m_type)
		{
		case DL_WRITE:	do_vwrite(&m, FALSE, FALSE);	break;
		case DL_WRITEV:	do_vwrite(&m, FALSE, TRUE);	break;
		case DL_READ:	do_vread(&m, FALSE);		break;
		case DL_READV:	do_vread(&m, TRUE);		break;
		case DL_INIT:	do_init(&m);			break;
		case DL_GETSTAT: do_getstat(&m);		break;
		case DL_STOP:	do_stop(&m);			break;
		case HARD_INT:
			for (i= 0, dep= &de_table[0]; i<DE_PORT_NR; i++, dep++)
			{
				if (dep->de_mode != DEM_ENABLED)
					continue;
				assert(dep->de_flags & DEF_ENABLED);
				irq= dep->de_irq;
				assert(irq >= 0 && irq < NR_IRQ_VECTORS);
				if (int_pending[irq])
				{
					int_pending[irq]= 0;
					dp_check_ints(dep);
					do_int(dep);
				}
			}
			break;
		default:
			panic("dp8390: illegal message", m.m_type);
		}
	}
}


/*===========================================================================*
 *				dp_dump					     *
 *===========================================================================*/
void dp_dump()
{
	dpeth_t *dep;
	int i, isr;

	printf("\n");
	for (i= 0, dep = &de_table[0]; i<DE_PORT_NR; i++, dep++)
	{
		if (dep->de_mode == DEM_DISABLED)
			printf("dp8390 port %d is disabled\n", i);
		else if (dep->de_mode == DEM_SINK)
			printf("dp8390 port %d is in sink mode\n", i);

		if (dep->de_mode != DEM_ENABLED)
			continue;

		printf("dp8390 statistics of port %d:\n", i);

		printf("recvErr    :%8ld\t", dep->de_stat.ets_recvErr);
		printf("sendErr    :%8ld\t", dep->de_stat.ets_sendErr);
		printf("OVW        :%8ld\n", dep->de_stat.ets_OVW);

		printf("CRCerr     :%8ld\t", dep->de_stat.ets_CRCerr);
		printf("frameAll   :%8ld\t", dep->de_stat.ets_frameAll);
		printf("missedP    :%8ld\n", dep->de_stat.ets_missedP);

		printf("packetR    :%8ld\t", dep->de_stat.ets_packetR);
		printf("packetT    :%8ld\t", dep->de_stat.ets_packetT);
		printf("transDef   :%8ld\n", dep->de_stat.ets_transDef);

		printf("collision  :%8ld\t", dep->de_stat.ets_collision);
		printf("transAb    :%8ld\t", dep->de_stat.ets_transAb);
		printf("carrSense  :%8ld\n", dep->de_stat.ets_carrSense);

		printf("fifoUnder  :%8ld\t", dep->de_stat.ets_fifoUnder);
		printf("fifoOver   :%8ld\t", dep->de_stat.ets_fifoOver);
		printf("CDheartbeat:%8ld\n", dep->de_stat.ets_CDheartbeat);

		printf("OWC        :%8ld\t", dep->de_stat.ets_OWC);

		isr= inb_reg0(dep, DP_ISR);
		printf("dp_isr = 0x%x + 0x%x, de_flags = 0x%x\n", isr,
					inb_reg0(dep, DP_ISR), dep->de_flags);
	}
}


/*===========================================================================*
 *				dp8390_stop				     *
 *===========================================================================*/
void dp8390_stop()
{
	message mess;
	int i;

	for (i= 0; i<DE_PORT_NR; i++)
	{
		if (de_table[i].de_mode != DEM_ENABLED)
			continue;
		mess.m_type= DL_STOP;
		mess.DL_PORT= i;
		do_stop(&mess);
	}
}


/*===========================================================================*
 *				do_vwrite				     *
 *===========================================================================*/
static void do_vwrite(mp, from_int, vectored)
message *mp;
int from_int;
int vectored;
{
	int port, count, size;
	int sendq_head;
	dpeth_t *dep;

	port = mp->DL_PORT;
	count = mp->DL_COUNT;
	if (port < 0 || port >= DE_PORT_NR)
		panic("dp8390: illegal port", port);
	dep= &de_table[port];
	dep->de_client= mp->DL_PROC;

	if (dep->de_mode == DEM_SINK)
	{
		assert(!from_int);
		dep->de_flags |= DEF_PACK_SEND;
		reply(dep, OK, FALSE);
		return;
	}
	assert(dep->de_mode == DEM_ENABLED);
	assert(dep->de_flags & DEF_ENABLED);
	if (dep->de_flags & DEF_SEND_AVAIL)
		panic("dp8390: send already in progress", NO_NUM);

	sendq_head= dep->de_sendq_head;
	if (dep->de_sendq[sendq_head].sq_filled)
	{
		if (from_int)
			panic("dp8390: should not be sending\n", NO_NUM);
		dep->de_sendmsg= *mp;
		dep->de_flags |= DEF_SEND_AVAIL;
		reply(dep, OK, FALSE);
		return;
	}
	assert(!(dep->de_flags & DEF_PACK_SEND));

	if (vectored)
	{
		get_userdata(mp->DL_PROC, (vir_bytes) mp->DL_ADDR,
			(count > IOVEC_NR ? IOVEC_NR : count) *
			sizeof(iovec_t), dep->de_write_iovec.iod_iovec);
		dep->de_write_iovec.iod_iovec_s = count;
		dep->de_write_iovec.iod_proc_nr = mp->DL_PROC;
		dep->de_write_iovec.iod_iovec_addr = (vir_bytes) mp->DL_ADDR;

		dep->de_tmp_iovec = dep->de_write_iovec;
		size = calc_iovec_size(&dep->de_tmp_iovec);
	}
	else
	{  
		dep->de_write_iovec.iod_iovec[0].iov_addr =
			(vir_bytes) mp->DL_ADDR;
		dep->de_write_iovec.iod_iovec[0].iov_size =
			mp->DL_COUNT;
		dep->de_write_iovec.iod_iovec_s = 1;
		dep->de_write_iovec.iod_proc_nr = mp->DL_PROC;
		dep->de_write_iovec.iod_iovec_addr = 0;
		size= mp->DL_COUNT;
	}
	if (size < ETH_MIN_PACK_SIZE || size > ETH_MAX_PACK_SIZE)
	{
		panic("dp8390: invalid packet size", size);
	}
	(dep->de_user2nicf)(dep, &dep->de_write_iovec, 0,
		dep->de_sendq[sendq_head].sq_sendpage * DP_PAGESIZE,
		size);
	dep->de_sendq[sendq_head].sq_filled= TRUE;
	if (dep->de_sendq_tail == sendq_head)
	{
		outb_reg0(dep, DP_TPSR, dep->de_sendq[sendq_head].sq_sendpage);
		outb_reg0(dep, DP_TBCR1, size >> 8);
		outb_reg0(dep, DP_TBCR0, size & 0xff);
		outb_reg0(dep, DP_CR, CR_TXP);	/* there it goes.. */
	}
	else
		dep->de_sendq[sendq_head].sq_size= size;
	
	if (++sendq_head == dep->de_sendq_nr)
		sendq_head= 0;
	assert(sendq_head < SENDQ_NR);
	dep->de_sendq_head= sendq_head;

	dep->de_flags |= DEF_PACK_SEND;

	/* If the interrupt handler called, don't send a reply. The reply
	 * will be sent after all interrupts are handled. 
	 */
	if (from_int)
		return;
	reply(dep, OK, FALSE);

	assert(dep->de_mode == DEM_ENABLED);
	assert(dep->de_flags & DEF_ENABLED);
}


/*===========================================================================*
 *				do_vread				     *
 *===========================================================================*/
static void do_vread(mp, vectored)
message *mp;
int vectored;
{
	int port, count;
	int size;
	dpeth_t *dep;

	port = mp->DL_PORT;
	count = mp->DL_COUNT;
	if (port < 0 || port >= DE_PORT_NR)
		panic("dp8390: illegal port", port);
	dep= &de_table[port];
	dep->de_client= mp->DL_PROC;
	if (dep->de_mode == DEM_SINK)
	{
		reply(dep, OK, FALSE);
		return;
	}
	assert(dep->de_mode == DEM_ENABLED);
	assert(dep->de_flags & DEF_ENABLED);

	if(dep->de_flags & DEF_READING)
		panic("dp8390: read already in progress", NO_NUM);

	if (vectored)
	{
		get_userdata(mp->DL_PROC, (vir_bytes) mp->DL_ADDR,
			(count > IOVEC_NR ? IOVEC_NR : count) *
			sizeof(iovec_t), dep->de_read_iovec.iod_iovec);
		dep->de_read_iovec.iod_iovec_s = count;
		dep->de_read_iovec.iod_proc_nr = mp->DL_PROC;
		dep->de_read_iovec.iod_iovec_addr = (vir_bytes) mp->DL_ADDR;

		dep->de_tmp_iovec = dep->de_read_iovec;
		size= calc_iovec_size(&dep->de_tmp_iovec);
	}
	else
	{
		dep->de_read_iovec.iod_iovec[0].iov_addr =
			(vir_bytes) mp->DL_ADDR;
		dep->de_read_iovec.iod_iovec[0].iov_size =
			mp->DL_COUNT;
		dep->de_read_iovec.iod_iovec_s = 1;
		dep->de_read_iovec.iod_proc_nr = mp->DL_PROC;
		dep->de_read_iovec.iod_iovec_addr = 0;
		size= count;
	}
	if (size < ETH_MAX_PACK_SIZE)
		panic("dp8390: wrong packet size", size);
	dep->de_flags |= DEF_READING;

	dp_recv(dep);

	if ((dep->de_flags & (DEF_READING|DEF_STOPPED)) ==
		(DEF_READING|DEF_STOPPED))
	{
		/* The chip is stopped, and all arrived packets are 
		 * delivered.
		 */
		dp_reset(dep);
	}
	reply(dep, OK, FALSE);
}


/*===========================================================================*
 *				do_init					     *
 *===========================================================================*/
static void do_init(mp)
message *mp;
{
	int port;
	dpeth_t *dep;
	message reply_mess;

	port = mp->DL_PORT;
	if (port < 0 || port >= DE_PORT_NR)
	{
		reply_mess.m_type= DL_INIT_REPLY;
		reply_mess.m3_i1= ENXIO;
		mess_reply(mp, &reply_mess);
		return;
	}
	dep= &de_table[port];
	strcpy(dep->de_name, "dp8390#0");
	dep->de_name[7] += port;
	if (dep->de_mode == DEM_DISABLED)
	{
		/* This is the default, try to (re)locate the device. */
		conf_hw(dep);
		if (dep->de_mode == DEM_DISABLED)
		{
			/* Probe failed, or the device is configured off. */
			reply_mess.m_type= DL_INIT_REPLY;
			reply_mess.m3_i1= ENXIO;
			mess_reply(mp, &reply_mess);
			return;
		}
		if (dep->de_mode == DEM_ENABLED)
			dp_init(dep);
	}

	if (dep->de_mode == DEM_SINK)
	{
		dep->de_address.ea_addr[0] = 
			dep->de_address.ea_addr[1] = 
			dep->de_address.ea_addr[2] = 
			dep->de_address.ea_addr[3] = 
			dep->de_address.ea_addr[4] = 
			dep->de_address.ea_addr[5] = 0;
		dp_confaddr(dep);
		reply_mess.m_type = DL_INIT_REPLY;
		reply_mess.m3_i1 = mp->DL_PORT;
		reply_mess.m3_i2 = DE_PORT_NR;
		*(ether_addr_t *) reply_mess.m3_ca1 = dep->de_address;
		mess_reply(mp, &reply_mess);
		return;
	}
	assert(dep->de_mode == DEM_ENABLED);
	assert(dep->de_flags & DEF_ENABLED);

	dep->de_flags &= ~(DEF_PROMISC | DEF_MULTI | DEF_BROAD);

	if (mp->DL_MODE & DL_PROMISC_REQ)
		dep->de_flags |= DEF_PROMISC | DEF_MULTI | DEF_BROAD;
	if (mp->DL_MODE & DL_MULTI_REQ)
		dep->de_flags |= DEF_MULTI;
	if (mp->DL_MODE & DL_BROAD_REQ)
		dep->de_flags |= DEF_BROAD;

	dep->de_client = mp->m_source;
	dp_reinit(dep);

	reply_mess.m_type = DL_INIT_REPLY;
	reply_mess.m3_i1 = mp->DL_PORT;
	reply_mess.m3_i2 = DE_PORT_NR;
	*(ether_addr_t *) reply_mess.m3_ca1 = dep->de_address;

	mess_reply(mp, &reply_mess);
}

/*===========================================================================*
 *				do_int					     *
 *===========================================================================*/
static void do_int(dep)
dpeth_t *dep;
{
	if (dep->de_flags & (DEF_PACK_SEND | DEF_PACK_RECV))
		reply(dep, OK, TRUE);
}


/*===========================================================================*
 *				do_getstat				     *
 *===========================================================================*/
static void do_getstat(mp)
message *mp;
{
	int port;
	dpeth_t *dep;

	port = mp->DL_PORT;
	if (port < 0 || port >= DE_PORT_NR)
		panic("dp8390: illegal port", port);
	dep= &de_table[port];
	dep->de_client= mp->DL_PROC;
	if (dep->de_mode == DEM_SINK)
	{
		put_userdata(mp->DL_PROC, (vir_bytes) mp->DL_ADDR,
			(vir_bytes) sizeof(dep->de_stat), &dep->de_stat);
		reply(dep, OK, FALSE);
		return;
	}
	assert(dep->de_mode == DEM_ENABLED);
	assert(dep->de_flags & DEF_ENABLED);

	dep->de_stat.ets_CRCerr += inb_reg0(dep, DP_CNTR0);
	dep->de_stat.ets_frameAll += inb_reg0(dep, DP_CNTR1);
	dep->de_stat.ets_missedP += inb_reg0(dep, DP_CNTR2);

	put_userdata(mp->DL_PROC, (vir_bytes) mp->DL_ADDR,
		(vir_bytes) sizeof(dep->de_stat), &dep->de_stat);
	reply(dep, OK, FALSE);
}


/*===========================================================================*
 *				do_stop					     *
 *===========================================================================*/
static void do_stop(mp)
message *mp;
{
	int port;
	dpeth_t *dep;

	port = mp->DL_PORT;

	if (port < 0 || port >= DE_PORT_NR)
		panic("dp8390: illegal port", port);
	dep= &de_table[port];
	if (dep->de_mode == DEM_SINK)
		return;
	assert(dep->de_mode == DEM_ENABLED);

	if (!(dep->de_flags & DEF_ENABLED))
		return;

	outb_reg0(dep, DP_CR, CR_STP | CR_DM_ABORT);
	(dep->de_stopf)(dep);

	dep->de_flags= DEF_EMPTY;
}


/*===========================================================================*
 *				dp_init					     *
 *===========================================================================*/
static void dp_init(dep)
dpeth_t *dep;
{
	int dp_rcr_reg;
	int i;

	/* General initialization */
	dep->de_flags = DEF_EMPTY;
	(*dep->de_initf)(dep);

	dp_confaddr(dep);

	if (debug)
	{
		printf("%s: Ethernet address ", dep->de_name);
		for (i= 0; i < 6; i++)
			printf("%x%c", dep->de_address.ea_addr[i],
							i < 5 ? ':' : '\n');
	}

	/* Initialization of the dp8390 */
	outb_reg0(dep, DP_CR, CR_PS_P0 | CR_STP | CR_DM_ABORT);
	outb_reg0(dep, DP_IMR, 0);
	outb_reg0(dep, DP_PSTART, dep->de_startpage);
	outb_reg0(dep, DP_PSTOP, dep->de_stoppage);
	outb_reg0(dep, DP_BNRY, dep->de_startpage);
	outb_reg0(dep, DP_RCR, RCR_MON);
	outb_reg0(dep, DP_TCR, TCR_NORMAL);
	if (dep->de_16bit)
		outb_reg0(dep, DP_DCR, DCR_WORDWIDE | DCR_8BYTES | DCR_BMS);
	else
		outb_reg0(dep, DP_DCR, DCR_BYTEWIDE | DCR_8BYTES | DCR_BMS);
	outb_reg0(dep, DP_RBCR0, 0);
	outb_reg0(dep, DP_RBCR1, 0);
	outb_reg0(dep, DP_ISR, 0xFF);
	outb_reg0(dep, DP_CR, CR_PS_P1 | CR_DM_ABORT);

	outb_reg1(dep, DP_PAR0, dep->de_address.ea_addr[0]);
	outb_reg1(dep, DP_PAR1, dep->de_address.ea_addr[1]);
	outb_reg1(dep, DP_PAR2, dep->de_address.ea_addr[2]);
	outb_reg1(dep, DP_PAR3, dep->de_address.ea_addr[3]);
	outb_reg1(dep, DP_PAR4, dep->de_address.ea_addr[4]);
	outb_reg1(dep, DP_PAR5, dep->de_address.ea_addr[5]);

	outb_reg1(dep, DP_MAR0, 0xff);
	outb_reg1(dep, DP_MAR1, 0xff);
	outb_reg1(dep, DP_MAR2, 0xff);
	outb_reg1(dep, DP_MAR3, 0xff);
	outb_reg1(dep, DP_MAR4, 0xff);
	outb_reg1(dep, DP_MAR5, 0xff);
	outb_reg1(dep, DP_MAR6, 0xff);
	outb_reg1(dep, DP_MAR7, 0xff);

	outb_reg1(dep, DP_CURR, dep->de_startpage + 1);
	outb_reg1(dep, DP_CR, CR_PS_P0 | CR_DM_ABORT);

	dp_rcr_reg = 0;
	if (dep->de_flags & DEF_PROMISC)
		dp_rcr_reg |= RCR_AB | RCR_PRO | RCR_AM;
	if (dep->de_flags & DEF_BROAD)
		dp_rcr_reg |= RCR_AB;
	if (dep->de_flags & DEF_MULTI)
		dp_rcr_reg |= RCR_AM;
	outb_reg0(dep, DP_RCR, dp_rcr_reg);
	inb_reg0(dep, DP_CNTR0);		/* reset counters by reading */
	inb_reg0(dep, DP_CNTR1);
	inb_reg0(dep, DP_CNTR2);

	outb_reg0(dep, DP_IMR, IMR_PRXE | IMR_PTXE | IMR_RXEE | IMR_TXEE |
		IMR_OVWE | IMR_CNTE);
	outb_reg0(dep, DP_CR, CR_STA | CR_DM_ABORT);

	/* Finish the initialization. */
	dep->de_flags |= DEF_ENABLED;
	for (i= 0; i<dep->de_sendq_nr; i++)
		dep->de_sendq[i].sq_filled= 0;
	dep->de_sendq_head= 0;
	dep->de_sendq_tail= 0;
	if (!dep->de_prog_IO)
	{
		dep->de_user2nicf= dp_user2nic;
		dep->de_nic2userf= dp_nic2user;
		dep->de_getblockf= dp_getblock;
	}
	else if (dep->de_16bit)
	{
		dep->de_user2nicf= dp_pio16_user2nic;
		dep->de_nic2userf= dp_pio16_nic2user;
		dep->de_getblockf= dp_pio16_getblock;
	}
	else
	{
		dep->de_user2nicf= dp_pio8_user2nic;
		dep->de_nic2userf= dp_pio8_nic2user;
		dep->de_getblockf= dp_pio8_getblock;
	}

	/* set the interrupt handler */
	put_irq_handler(dep->de_irq, dp_handler);
	enable_irq(dep->de_irq);
}


/*===========================================================================*
 *				dp_confaddr				     *
 *===========================================================================*/
static void dp_confaddr(dep)
dpeth_t *dep;
{
	int i;
	char eakey[16];
	static char eafmt[]= "x:x:x:x:x:x";
	long v;

	/* User defined ethernet address? */
	strcpy(eakey, dp_conf[dep-de_table].dpc_envvar);
	strcat(eakey, "_EA");

	for (i= 0; i < 6; i++)
	{
		v= dep->de_address.ea_addr[i];
		if (env_parse(eakey, eafmt, i, &v, 0x00L, 0xFFL) != EP_SET)
			break;
		dep->de_address.ea_addr[i]= v;
	}

	if (i != 0 && i != 6)
	{
		/* It's all or nothing; force a panic. */
		(void) env_parse(eakey, "?", 0, &v, 0L, 0L);
	}
}


/*===========================================================================*
 *				dp_reinit				     *
 *===========================================================================*/
static void dp_reinit(dep)
dpeth_t *dep;
{
	int dp_rcr_reg;

	outb_reg0(dep, DP_CR, CR_PS_P0);

	dp_rcr_reg = 0;
	if (dep->de_flags & DEF_PROMISC)
		dp_rcr_reg |= RCR_AB | RCR_PRO | RCR_AM;
	if (dep->de_flags & DEF_BROAD)
		dp_rcr_reg |= RCR_AB;
	if (dep->de_flags & DEF_MULTI)
		dp_rcr_reg |= RCR_AM;
	outb_reg0(dep, DP_RCR, dp_rcr_reg);
}


/*===========================================================================*
 *				dp_reset				     *
 *===========================================================================*/
static void dp_reset(dep)
dpeth_t *dep;
{
	int i;

	/* Stop chip */
	outb_reg0(dep, DP_CR, CR_STP | CR_DM_ABORT);
	outb_reg0(dep, DP_RBCR0, 0);
	outb_reg0(dep, DP_RBCR1, 0);
	for (i= 0; i < 0x1000 && ((inb_reg0(dep, DP_ISR) & ISR_RST) == 0); i++)
		; /* Do nothing */
	outb_reg0(dep, DP_TCR, TCR_1EXTERNAL|TCR_OFST);
	outb_reg0(dep, DP_CR, CR_STA|CR_DM_ABORT);
	outb_reg0(dep, DP_TCR, TCR_NORMAL|TCR_OFST);

	/* Acknowledge the ISR_RDC (remote dma) interrupt. */
	for (i= 0; i < 0x1000 && ((inb_reg0(dep, DP_ISR) & ISR_RDC) == 0); i++)
		; /* Do nothing */
	outb_reg0(dep, DP_ISR, inb_reg0(dep, DP_ISR) & ~ISR_RDC);

	/* Reset the transmit ring. If we were transmitting a packet, we
	 * pretend that the packet is processed. Higher layers will
	 * retransmit if the packet wasn't actually sent.
	 */
	dep->de_sendq_head= dep->de_sendq_tail= 0;
	for (i= 0; i<dep->de_sendq_nr; i++)
		dep->de_sendq[i].sq_filled= 0;
	dp_send(dep);
	dep->de_flags &= ~DEF_STOPPED;
}


/*===========================================================================*
 *				dp_check_ints				     *
 *===========================================================================*/
static void dp_check_ints(dep)
dpeth_t *dep;
{
	int isr, tsr;
	int size, sendq_tail;

	if (!(dep->de_flags & DEF_ENABLED))
		panic("dp8390: got premature interrupt", NO_NUM);

	for(;;)
	{
		isr = inb_reg0(dep, DP_ISR);
		if (!isr)
			break;
		outb_reg0(dep, DP_ISR, isr);
		if (isr & (ISR_PTX|ISR_TXE))
		{
			if (isr & ISR_TXE)
			{
#if DEBUG
 { printf("%s: got send Error\n", dep->de_name); }
#endif
				dep->de_stat.ets_sendErr++;
			}
			else
			{
				tsr = inb_reg0(dep, DP_TSR);

				if (tsr & TSR_PTX) dep->de_stat.ets_packetT++;
				if (tsr & TSR_DFR) dep->de_stat.ets_transDef++;
				if (tsr & TSR_COL) dep->de_stat.ets_collision++;
				if (tsr & TSR_ABT) dep->de_stat.ets_transAb++;
				if (tsr & TSR_CRS) dep->de_stat.ets_carrSense++;
				if (tsr & TSR_FU
					&& ++dep->de_stat.ets_fifoUnder <= 10)
				{
					printf("%s: fifo underrun\n",
						dep->de_name);
				}
				if (tsr & TSR_CDH
					&& ++dep->de_stat.ets_CDheartbeat <= 10)
				{
					printf("%s: CD heart beat failure\n",
						dep->de_name);
				}
				if (tsr & TSR_OWC) dep->de_stat.ets_OWC++;
			}
			sendq_tail= dep->de_sendq_tail;

			if (!(dep->de_sendq[sendq_tail].sq_filled))
			{
				/* Software bug? */
				assert(!debug);

				/* Or hardware bug? */
				printf(
				"%s: transmit interrupt, but not sending\n",
					dep->de_name);
				continue;
			}
			dep->de_sendq[sendq_tail].sq_filled= 0;
			if (++sendq_tail == dep->de_sendq_nr)
				sendq_tail= 0;
			dep->de_sendq_tail= sendq_tail;
			if (dep->de_sendq[sendq_tail].sq_filled)
			{
				size= dep->de_sendq[sendq_tail].sq_size;
				outb_reg0(dep, DP_TPSR,
					dep->de_sendq[sendq_tail].sq_sendpage);
				outb_reg0(dep, DP_TBCR1, size >> 8);
				outb_reg0(dep, DP_TBCR0, size & 0xff);
				outb_reg0(dep, DP_CR, CR_TXP);	/* there is goes.. */
			}
			if (dep->de_flags & DEF_SEND_AVAIL)
				dp_send(dep);
		}

		if (isr & ISR_PRX)
			dp_recv(dep);
		
		if (isr & ISR_RXE) dep->de_stat.ets_recvErr++;
		if (isr & ISR_CNT)
		{
			dep->de_stat.ets_CRCerr += inb_reg0(dep, DP_CNTR0);
			dep->de_stat.ets_frameAll += inb_reg0(dep, DP_CNTR1);
			dep->de_stat.ets_missedP += inb_reg0(dep, DP_CNTR2);
		}
		if (isr & ISR_OVW)
		{
#if DEBUG
 { printW(); printf("%s: got overwrite warning\n", dep->de_name); }
#endif
		}
		if (isr & ISR_RDC)
		{
			/* Nothing to do */
		}
		if (isr & ISR_RST)
		{
			/* this means we got an interrupt but the ethernet 
			 * chip is shutdown. We set the flag DEF_STOPPED,
			 * and continue processing arrived packets. When the
			 * receive buffer is empty, we reset the dp8390.
			 */
#if DEBUG
 { printW(); printf("%s: NIC stopped\n", dep->de_name); }
#endif
			dep->de_flags |= DEF_STOPPED;
			break;
		}
	}
	if ((dep->de_flags & (DEF_READING|DEF_STOPPED)) == 
						(DEF_READING|DEF_STOPPED))
	{
		/* The chip is stopped, and all arrived packets are 
		 * delivered.
		 */
		dp_reset(dep);
	}
}


/*===========================================================================*
 *				dp_recv					     *
 *===========================================================================*/
static void dp_recv(dep)
dpeth_t *dep;
{
	dp_rcvhdr_t header;
	unsigned pageno, curr, next;
	vir_bytes length;
	int packet_processed, r;
	u16_t eth_type;

	packet_processed = FALSE;
	pageno = inb_reg0(dep, DP_BNRY) + 1;
	if (pageno == dep->de_stoppage) pageno = dep->de_startpage;

	do
	{
		outb_reg0(dep, DP_CR, CR_PS_P1);
		curr = inb_reg1(dep, DP_CURR);
		outb_reg0(dep, DP_CR, CR_PS_P0);

		if (curr == pageno) break;

		(dep->de_getblockf)(dep, pageno, (size_t)0, sizeof(header),
			&header);
		(dep->de_getblockf)(dep, pageno, sizeof(header) +
			2*sizeof(ether_addr_t), sizeof(eth_type), &eth_type);

		length = (header.dr_rbcl | (header.dr_rbch << 8)) -
			sizeof(dp_rcvhdr_t);
		next = header.dr_next;
		if (!(header.dr_status & RSR_PRX))
		{
			printf("%s: receive error %02x, resetting receive buffer\n",
				dep->de_name, header.dr_status);
			dep->de_stat.ets_recvErr++;
			next = curr;
		}
		else if (header.dr_status & ~(RSR_PHY | RSR_PRX))
		{
			/* This is very serious, so we issue a warning and
			 * reset the buffers */
			printf("%s: bad status %02x, resetting receive buffer\n",
				dep->de_name, header.dr_status);
			dep->de_stat.ets_fifoOver++;
			next = curr;
		}
		else if (length < 60 || length > 1514)
		{
			printf("%s: packet with strange length arrived: %d\n",
				dep->de_name, (int) length);
			next= curr;
		}
		else if (next < dep->de_startpage || next >= dep->de_stoppage)
		{
			printf("%s: strange next page\n", dep->de_name);
			next= curr;
		}
		else if (eth_type == eth_ign_proto)
		{
			/* Hack: ignore packets of a given protocol, useful
			 * if you share a net with 80 computers sending
			 * Amoeba FLIP broadcasts.  (Protocol 0x8146.)
			 */
			static int first= 1;
			if (first)
			{
				first= 0;
				printf("%s: dropping proto 0x%04x packets\n",
					dep->de_name,
					ntohs(eth_ign_proto));
			}
			dep->de_stat.ets_packetR++;
			next = curr;
		}
		else if (dep->de_flags & DEF_ENABLED)
		{
			r = dp_pkt2user(dep, pageno, length);
			if (r != OK)
				return;

			packet_processed = TRUE;
			dep->de_stat.ets_packetR++;
		}
		if (next == dep->de_startpage)
			outb_reg0(dep, DP_BNRY, dep->de_stoppage - 1);
		else
			outb_reg0(dep, DP_BNRY, next - 1);

		pageno = next;
	}
	while (!packet_processed);
}


/*===========================================================================*
 *				dp_send					     *
 *===========================================================================*/
static void dp_send(dep)
dpeth_t *dep;
{
	if (!(dep->de_flags & DEF_SEND_AVAIL))
		return;

	dep->de_flags &= ~DEF_SEND_AVAIL;
	switch(dep->de_sendmsg.m_type)
	{
	case DL_WRITE:	do_vwrite(&dep->de_sendmsg, TRUE, FALSE);	break;
	case DL_WRITEV:	do_vwrite(&dep->de_sendmsg, TRUE, TRUE);	break;
	default:
		panic("dp8390: wrong type:", dep->de_sendmsg.m_type);
		break;
	}
}


/*===========================================================================*
 *				dp_getblock				     *
 *===========================================================================*/
static void dp_getblock(dep, page, offset, size, dst)
dpeth_t *dep;
int page;
size_t offset;
size_t size;
void *dst;
{
	u16_t *ha;
	int i;

	ha = (u16_t *) dst;
	offset = page * DP_PAGESIZE + offset;
	assert(!(size & 1));
	for (i= 0; i<size; i+=2)
	{
		*ha = mem_rdw(dep->de_memsegm, offset+i);
		ha++;
	}
}


/*===========================================================================*
 *				dp_pio8_getblock			     *
 *===========================================================================*/
static void dp_pio8_getblock(dep, page, offset, size, dst)
dpeth_t *dep;
int page;
size_t offset;
size_t size;
void *dst;
{
	u8_t *ha;
	int i;

	ha = (u8_t *) dst;
	offset = page * DP_PAGESIZE + offset;
	outb_reg0(dep, DP_RBCR0, size & 0xFF);
	outb_reg0(dep, DP_RBCR1, size >> 8);
	outb_reg0(dep, DP_RSAR0, offset & 0xFF);
	outb_reg0(dep, DP_RSAR1, offset >> 8);
	outb_reg0(dep, DP_CR, CR_DM_RR | CR_PS_P0 | CR_STA);

	rep_inb(dep->de_data_port, ha, size);
}


/*===========================================================================*
 *				dp_pio16_getblock			     *
 *===========================================================================*/
static void dp_pio16_getblock(dep, page, offset, size, dst)
dpeth_t *dep;
int page;
size_t offset;
size_t size;
void *dst;
{
	u16_t *ha;
	int i;

	ha = (u16_t *) dst;
	offset = page * DP_PAGESIZE + offset;
	outb_reg0(dep, DP_RBCR0, size & 0xFF);
	outb_reg0(dep, DP_RBCR1, size >> 8);
	outb_reg0(dep, DP_RSAR0, offset & 0xFF);
	outb_reg0(dep, DP_RSAR1, offset >> 8);
	outb_reg0(dep, DP_CR, CR_DM_RR | CR_PS_P0 | CR_STA);

	rep_inw(dep->de_data_port, ha, size);
}


/*===========================================================================*
 *				dp_pkt2user				     *
 *===========================================================================*/
static int dp_pkt2user(dep, page, length)
dpeth_t *dep;
int page, length;
{
	int last, count;

	if (!(dep->de_flags & DEF_READING))
		return EGENERIC;

	last = page + (length - 1) / DP_PAGESIZE;
	if (last >= dep->de_stoppage)
	{
		count = (dep->de_stoppage - page) * DP_PAGESIZE -
			sizeof(dp_rcvhdr_t);

		/* Save read_iovec since we need it twice. */
		dep->de_tmp_iovec = dep->de_read_iovec;
		(dep->de_nic2userf)(dep, page * DP_PAGESIZE +
			sizeof(dp_rcvhdr_t), &dep->de_tmp_iovec, 0, count);
		(dep->de_nic2userf)(dep, dep->de_startpage * DP_PAGESIZE, 
				&dep->de_read_iovec, count, length - count);
	}
	else
	{
		(dep->de_nic2userf)(dep, page * DP_PAGESIZE +
			sizeof(dp_rcvhdr_t), &dep->de_read_iovec, 0, length);
	}

	dep->de_read_s = length;
	dep->de_flags |= DEF_PACK_RECV;
	dep->de_flags &= ~DEF_READING;

	return OK;
}


/*===========================================================================*
 *				dp_user2nic				     *
 *===========================================================================*/
static void dp_user2nic(dep, iovp, offset, nic_addr, count)
dpeth_t *dep;
iovec_dat_t *iovp;
vir_bytes offset;
int nic_addr;
vir_bytes count;
{
	phys_bytes phys_hw, phys_user;
	int bytes, i;

	phys_hw = dep->de_linmem + nic_addr;

	i= 0;
	while (count > 0)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		assert(i < iovp->iod_iovec_s);
		if (offset >= iovp->iod_iovec[i].iov_size)
		{
			offset -= iovp->iod_iovec[i].iov_size;
			i++;
			continue;
		}
		bytes = iovp->iod_iovec[i].iov_size - offset;
		if (bytes > count)
			bytes = count;

		phys_user = numap(iovp->iod_proc_nr,
			iovp->iod_iovec[i].iov_addr + offset, bytes);
		if (!phys_user)
			panic("dp8390: umap failed\n", NO_NUM);
		phys_copy(phys_user, phys_hw, (phys_bytes) bytes);
		count -= bytes;
		phys_hw += bytes;
		offset += bytes;
	}
	assert(count == 0);
}


/*===========================================================================*
 *				dp_pio8_user2nic			     *
 *===========================================================================*/
static void dp_pio8_user2nic(dep, iovp, offset, nic_addr, count)
dpeth_t *dep;
iovec_dat_t *iovp;
vir_bytes offset;
int nic_addr;
vir_bytes count;
{
	phys_bytes phys_user;
	int bytes, i;

	outb_reg0(dep, DP_ISR, ISR_RDC);

	outb_reg0(dep, DP_RBCR0, count & 0xFF);
	outb_reg0(dep, DP_RBCR1, count >> 8);
	outb_reg0(dep, DP_RSAR0, nic_addr & 0xFF);
	outb_reg0(dep, DP_RSAR1, nic_addr >> 8);
	outb_reg0(dep, DP_CR, CR_DM_RW | CR_PS_P0 | CR_STA);

	i= 0;
	while (count > 0)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		assert(i < iovp->iod_iovec_s);
		if (offset >= iovp->iod_iovec[i].iov_size)
		{
			offset -= iovp->iod_iovec[i].iov_size;
			i++;
			continue;
		}
		bytes = iovp->iod_iovec[i].iov_size - offset;
		if (bytes > count)
			bytes = count;

		phys_user = numap(iovp->iod_proc_nr,
			iovp->iod_iovec[i].iov_addr + offset, bytes);
		if (!phys_user)
			panic("dp8390: umap failed\n", NO_NUM);
		port_write_byte(dep->de_data_port, phys_user, bytes);
		count -= bytes;
		offset += bytes;
	}
	assert(count == 0);

	for (i= 0; i<100; i++)
	{
		if (inb_reg0(dep, DP_ISR) & ISR_RDC)
			break;
	}
	if (i == 100)
	{
		panic("dp8390: remote dma failed to complete", NO_NUM);
	}
}


/*===========================================================================*
 *				dp_pio16_user2nic			     *
 *===========================================================================*/
static void dp_pio16_user2nic(dep, iovp, offset, nic_addr, count)
dpeth_t *dep;
iovec_dat_t *iovp;
vir_bytes offset;
int nic_addr;
vir_bytes count;
{
	phys_bytes phys_user;
	vir_bytes ecount;
	int bytes, i;
	u8_t two_bytes[2];
	phys_bytes phys_2bytes;
	int odd_byte;

	ecount= (count+1) & ~1;
	phys_2bytes = vir2phys(two_bytes);
	odd_byte= 0;

	outb_reg0(dep, DP_ISR, ISR_RDC);
	outb_reg0(dep, DP_RBCR0, ecount & 0xFF);
	outb_reg0(dep, DP_RBCR1, ecount >> 8);
	outb_reg0(dep, DP_RSAR0, nic_addr & 0xFF);
	outb_reg0(dep, DP_RSAR1, nic_addr >> 8);
	outb_reg0(dep, DP_CR, CR_DM_RW | CR_PS_P0 | CR_STA);

	i= 0;
	while (count > 0)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		assert(i < iovp->iod_iovec_s);
		if (offset >= iovp->iod_iovec[i].iov_size)
		{
			offset -= iovp->iod_iovec[i].iov_size;
			i++;
			continue;
		}
		bytes = iovp->iod_iovec[i].iov_size - offset;
		if (bytes > count)
			bytes = count;

		phys_user = numap(iovp->iod_proc_nr,
			iovp->iod_iovec[i].iov_addr + offset, bytes);
		if (!phys_user)
			panic("dp8390: umap failed\n", NO_NUM);
		if (odd_byte)
		{
			phys_copy(phys_user, phys_2bytes+1,
				(phys_bytes) 1);
			out_word(dep->de_data_port, *(u16_t *)two_bytes);
			count--;
			offset++;
			bytes--;
			phys_user++;
			odd_byte= 0;
			if (!bytes)
				continue;
		}
		ecount= bytes & ~1;
		if (ecount != 0)
		{
			port_write(dep->de_data_port, phys_user, ecount);
			count -= ecount;
			offset += ecount;
			bytes -= ecount;
			phys_user += ecount;
		}
		if (bytes)
		{
			assert(bytes == 1);
			phys_copy(phys_user, phys_2bytes, (phys_bytes) 1);
			count--;
			offset++;
			bytes--;
			phys_user++;
			odd_byte= 1;
		}
	}
	assert(count == 0);

	if (odd_byte)
		out_word(dep->de_data_port, *(u16_t *)two_bytes);

	for (i= 0; i<100; i++)
	{
		if (inb_reg0(dep, DP_ISR) & ISR_RDC)
			break;
	}
	if (i == 100)
	{
		panic("dp8390: remote dma failed to complete", NO_NUM);
	}
}


/*===========================================================================*
 *				dp_nic2user				     *
 *===========================================================================*/
static void dp_nic2user(dep, nic_addr, iovp, offset, count)
dpeth_t *dep;
int nic_addr;
iovec_dat_t *iovp;
vir_bytes offset;
vir_bytes count;
{
	phys_bytes phys_hw, phys_user;
	int bytes, i;

	phys_hw = dep->de_linmem + nic_addr;

	i= 0;
	while (count > 0)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		assert(i < iovp->iod_iovec_s);
		if (offset >= iovp->iod_iovec[i].iov_size)
		{
			offset -= iovp->iod_iovec[i].iov_size;
			i++;
			continue;
		}
		bytes = iovp->iod_iovec[i].iov_size - offset;
		if (bytes > count)
			bytes = count;

		phys_user = numap(iovp->iod_proc_nr,
			iovp->iod_iovec[i].iov_addr + offset, bytes);
		if (!phys_user)
			panic("dp8390: umap failed\n", NO_NUM);
		phys_copy(phys_hw, phys_user, (phys_bytes) bytes);
		count -= bytes;
		phys_hw += bytes;
		offset += bytes;
	}
	assert(count == 0);
}


/*===========================================================================*
 *				dp_pio8_nic2user			     *
 *===========================================================================*/
static void dp_pio8_nic2user(dep, nic_addr, iovp, offset, count)
dpeth_t *dep;
int nic_addr;
iovec_dat_t *iovp;
vir_bytes offset;
vir_bytes count;
{
	phys_bytes phys_user;
	int bytes, i;

	outb_reg0(dep, DP_RBCR0, count & 0xFF);
	outb_reg0(dep, DP_RBCR1, count >> 8);
	outb_reg0(dep, DP_RSAR0, nic_addr & 0xFF);
	outb_reg0(dep, DP_RSAR1, nic_addr >> 8);
	outb_reg0(dep, DP_CR, CR_DM_RR | CR_PS_P0 | CR_STA);

	i= 0;
	while (count > 0)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		assert(i < iovp->iod_iovec_s);
		if (offset >= iovp->iod_iovec[i].iov_size)
		{
			offset -= iovp->iod_iovec[i].iov_size;
			i++;
			continue;
		}
		bytes = iovp->iod_iovec[i].iov_size - offset;
		if (bytes > count)
			bytes = count;

		phys_user = numap(iovp->iod_proc_nr,
			iovp->iod_iovec[i].iov_addr + offset, bytes);
		if (!phys_user)
			panic("dp8390: umap failed\n", NO_NUM);
		port_read_byte(dep->de_data_port, phys_user, bytes);
		count -= bytes;
		offset += bytes;
	}
	assert(count == 0);
}


/*===========================================================================*
 *				dp_pio16_nic2user			     *
 *===========================================================================*/
static void dp_pio16_nic2user(dep, nic_addr, iovp, offset, count)
dpeth_t *dep;
int nic_addr;
iovec_dat_t *iovp;
vir_bytes offset;
vir_bytes count;
{
	phys_bytes phys_user;
	vir_bytes ecount;
	int bytes, i;
	u8_t two_bytes[2];
	phys_bytes phys_2bytes;
	int odd_byte;

	ecount= (count+1) & ~1;
	phys_2bytes = vir2phys(two_bytes);
	odd_byte= 0;

	outb_reg0(dep, DP_RBCR0, ecount & 0xFF);
	outb_reg0(dep, DP_RBCR1, ecount >> 8);
	outb_reg0(dep, DP_RSAR0, nic_addr & 0xFF);
	outb_reg0(dep, DP_RSAR1, nic_addr >> 8);
	outb_reg0(dep, DP_CR, CR_DM_RR | CR_PS_P0 | CR_STA);

	i= 0;
	while (count > 0)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		assert(i < iovp->iod_iovec_s);
		if (offset >= iovp->iod_iovec[i].iov_size)
		{
			offset -= iovp->iod_iovec[i].iov_size;
			i++;
			continue;
		}
		bytes = iovp->iod_iovec[i].iov_size - offset;
		if (bytes > count)
			bytes = count;

		phys_user = numap(iovp->iod_proc_nr,
			iovp->iod_iovec[i].iov_addr + offset, bytes);
		if (!phys_user)
			panic("dp8390: umap failed\n", NO_NUM);
		if (odd_byte)
		{
			phys_copy(phys_2bytes+1, phys_user, (phys_bytes) 1);
			count--;
			offset++;
			bytes--;
			phys_user++;
			odd_byte= 0;
			if (!bytes)
				continue;
		}
		ecount= bytes & ~1;
		if (ecount != 0)
		{
			port_read(dep->de_data_port, phys_user, ecount);
			count -= ecount;
			offset += ecount;
			bytes -= ecount;
			phys_user += ecount;
		}
		if (bytes)
		{
			assert(bytes == 1);
			*(u16_t *)two_bytes= in_word(dep->de_data_port);
			phys_copy(phys_2bytes, phys_user, (phys_bytes) 1);
			count--;
			offset++;
			bytes--;
			phys_user++;
			odd_byte= 1;
		}
	}
	assert(count == 0);
}


/*===========================================================================*
 *				dp_next_iovec					     *
 *===========================================================================*/
static void dp_next_iovec(iovp)
iovec_dat_t *iovp;
{
	assert(iovp->iod_iovec_s > IOVEC_NR);

	iovp->iod_iovec_s -= IOVEC_NR;

	iovp->iod_iovec_addr += IOVEC_NR * sizeof(iovec_t);

	get_userdata(iovp->iod_proc_nr, iovp->iod_iovec_addr, 
		(iovp->iod_iovec_s > IOVEC_NR ? IOVEC_NR : iovp->iod_iovec_s) *
		sizeof(iovec_t), iovp->iod_iovec); 
}


/*===========================================================================*
 *				dp_handler				     *
 *===========================================================================*/
static int dp_handler(irq)
int irq;
{
/* DP8390 interrupt, send message and reenable interrupts. */

#ifdef SHOW_EVENT
	if (debug)
		show_event(irq, 'E');
#endif

	assert(irq >= 0 && irq < NR_IRQ_VECTORS);
	int_pending[irq]= 1;
	interrupt(dpeth_tasknr);

#ifdef SHOW_EVENT
	if (debug)
		show_event(irq, ' ');
#endif

	return 1;
}

/*===========================================================================*
 *				conf_hw					     *
 *===========================================================================*/
static void conf_hw(dep)
dpeth_t *dep;
{
	static eth_stat_t empty_stat = {0, 0, 0, 0, 0, 0 	/* ,... */ };

	int ifnr;
	dp_conf_t *dcp;

	dep->de_mode= DEM_DISABLED;	/* Superfluous */
	ifnr= dep-de_table;

	dcp= &dp_conf[ifnr];
	update_conf(dep, dcp);
	if (dep->de_mode != DEM_ENABLED)
			return;
	if (!wdeth_probe(dep) && !ne2k_probe(dep) && !ne1k_probe(dep) && !el2_probe(dep))
	{
		printf("%s: No ethernet card found at 0x%x\n", 
			dep->de_name, dep->de_base_port);
		dep->de_mode= DEM_DISABLED;
		return;
	}

	/* Allocate a memory segment, programmed I/O should set the
	 * memory segment (linmem) to zero.
	 */
	if (dep->de_linmem != 0)
	{
		if (protected_mode)
		{
			init_dataseg(&gdt[dcp->dpc_prot_sel / DESC_SIZE],
				dep->de_linmem, dep->de_ramsize,
				TASK_PRIVILEGE);
			dep->de_memsegm= dcp->dpc_prot_sel;
		}
		else
		{
			dep->de_memsegm= physb_to_hclick(dep->de_linmem);
		}
	}

/* XXX */ if (dep->de_linmem == 0) dep->de_linmem= 0xFFFF0000;

	dep->de_flags = DEF_EMPTY;
	dep->de_stat = empty_stat;
}


/*===========================================================================*
 *				update_conf				     *
 *===========================================================================*/
static void update_conf(dep, dcp)
dpeth_t *dep;
dp_conf_t *dcp;
{
	long v;
	static char dpc_fmt[] = "x:d:x:x";

	/* Get the default settings and modify them from the environment. */
	dep->de_mode= DEM_SINK;
	v= dcp->dpc_port;
	switch (env_parse(dcp->dpc_envvar, dpc_fmt, 0, &v, 0x0000L, 0x3FFL)) {
	case EP_OFF:
		dep->de_mode= DEM_DISABLED;
		break;
	case EP_ON:
	case EP_SET:
		dep->de_mode= DEM_ENABLED;	/* Might become disabled if 
						 * all probes fail */
		break;
	}
	dep->de_base_port= v;

	v= dcp->dpc_irq | DEI_DEFAULT;
	(void) env_parse(dcp->dpc_envvar, dpc_fmt, 1, &v, 0L,
						(long) NR_IRQ_VECTORS - 1);
	dep->de_irq= v;

	v= dcp->dpc_mem;
	(void) env_parse(dcp->dpc_envvar, dpc_fmt, 2, &v, 0L, 0xFFFFFL);
	dep->de_linmem= v;

	v= 0;
	(void) env_parse(dcp->dpc_envvar, dpc_fmt, 3, &v, 0x2000L, 0x8000L);
	dep->de_ramsize= v;
}


/*===========================================================================*
 *				calc_iovec_size				     *
 *===========================================================================*/
static int calc_iovec_size(iovp)
iovec_dat_t *iovp;
{
	/* Calculate the size of a request. Note that the iovec_dat
	 * structure will be unusable after calc_iovec_size.
	 */
	int size;
	int i;

	size= 0;
	i= 0;
	while (i < iovp->iod_iovec_s)
	{
		if (i >= IOVEC_NR)
		{
			dp_next_iovec(iovp);
			i= 0;
			continue;
		}
		size += iovp->iod_iovec[i].iov_size;
		i++;
	}
	return size;
}


/*===========================================================================*
 *				reply					     *
 *===========================================================================*/
static void reply(dep, err, may_block)
dpeth_t *dep;
int err;
int may_block;
{
	message reply;
	int status;
	int r;

	status = 0;
	if (dep->de_flags & DEF_PACK_SEND)
		status |= DL_PACK_SEND;
	if (dep->de_flags & DEF_PACK_RECV)
		status |= DL_PACK_RECV;

	reply.m_type = DL_TASK_REPLY;
	reply.DL_PORT = dep - de_table;
	reply.DL_PROC = dep->de_client;
	reply.DL_STAT = status | ((u32_t) err << 16);
	reply.DL_COUNT = dep->de_read_s;
	reply.DL_CLCK = get_uptime();
	r= send(dep->de_client, &reply);
#if 0
	/* XXX  What is the reason that this doesn't happen? */
	if (result == ELOCKED && may_block)
		return;
#endif
	if (r < 0)
		panic("dp8390: send failed:", r);
	
	dep->de_read_s = 0;
	dep->de_flags &= ~(DEF_PACK_SEND | DEF_PACK_RECV);
}


/*===========================================================================*
 *				mess_reply				     *
 *===========================================================================*/
static void mess_reply(req, reply_mess)
message *req;
message *reply_mess;
{
	if (send(req->m_source, reply_mess) != OK)
		panic("dp8390: unable to mess_reply", NO_NUM);
}


/*===========================================================================*
 *				get_userdata				     *
 *===========================================================================*/
static void get_userdata(user_proc, user_addr, count, loc_addr)
int user_proc;
vir_bytes user_addr;
vir_bytes count;
void *loc_addr;
{
	phys_bytes src;

	src = numap(user_proc, user_addr, count);
	if (!src)
		panic("dp8390: umap failed", NO_NUM);

	phys_copy(src, vir2phys(loc_addr), (phys_bytes) count);
}


/*===========================================================================*
 *				put_userdata				     *
 *===========================================================================*/
static void put_userdata(user_proc, user_addr, count, loc_addr)
int user_proc;
vir_bytes user_addr;
vir_bytes count;
void *loc_addr;
{
	phys_bytes dst;

	dst = numap(user_proc, user_addr, count);
	if (!dst)
		panic("dp8390: umap failed", NO_NUM);

	phys_copy(vir2phys(loc_addr), dst, (phys_bytes) count);
}

#endif /* ENABLE_NETWORKING */

/*
 * $PchId: dp8390.c,v 1.5 1996/01/19 22:56:35 philip Exp $
 */

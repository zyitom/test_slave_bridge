/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the SOEM project root for full license information.
 *
 * AF_XDP variant of SOEM v1.4.0 oshw/linux/nicdrv.c.
 *
 * This is a DROP-IN replacement for the raw AF_PACKET driver: every public
 * function keeps the exact same signature and buffer-management contract, so
 * all of SOEM's upper layers (ecx_inframe index matching, ecx_srconfirm,
 * process-data build/parse) and the librmcs transport compile and run
 * unchanged. Only the wire I/O is swapped:
 *
 *   ecx_setupnic  : AF_PACKET SOCK_RAW  ->  AF_XDP UMEM + rings, zero-copy
 *   ecx_outframe  : send()              ->  TX ring reserve + kick
 *   ecx_recvpkt   : recv()              ->  RX ring peek, busy-poll driven
 *   ecx_closenic  : close()             ->  xsk/UMEM teardown
 *
 * Why this cuts EtherCAT cycle latency: the AF_PACKET path DMAs into an skb,
 * waits for the NIC RX interrupt + NAPI softirq, then copies to the socket
 * queue -- tens of microseconds per cycle, and the master loop is stop-and-
 * wait so that lands on every cycle. AF_XDP zero-copy lets the NIC DMA
 * straight into our UMEM, and SO_PREFER_BUSY_POLL + a recvfrom() kick runs
 * the driver's NAPI poll inline on the cycle thread, so the return frame is
 * pulled without ever waiting for an interrupt. Everything below the SOEM API
 * stays on one core with no softirq hand-off.
 *
 * Requirements (target machine, i225/i226 igc):
 *   - kernel >= 5.11 for SO_PREFER_BUSY_POLL/SO_BUSY_POLL_BUDGET, >= 5.14 for
 *     igc XDP, ZC a bit later; ZC falls back to copy mode automatically.
 *   - libxdp + libbpf (build: -lxdp -lbpf).
 *   - the EtherCAT RX must land on the bound queue: reduce the NIC to a single
 *     combined queue first, e.g.  ethtool -L <iface> combined 1  (see README).
 *   - run as root / with CAP_NET_RAW + CAP_BPF (sudo is fine).
 *
 * Tunables via environment (read once at setup):
 *   RMCS_XDP_QUEUE          RX/TX queue id to bind (default 0)
 *   RMCS_XDP_COPY=1         force XDP_COPY instead of trying zero-copy first
 *   RMCS_XDP_NO_BUSYPOLL=1  do not drive NAPI from recvfrom (rely on softirq)
 *   RMCS_XDP_BUSYPOLL_US    SO_BUSY_POLL value in us (default 20)
 *   RMCS_XDP_BUSYPOLL_BUDGET SO_BUSY_POLL_BUDGET (default 8)
 */

/* AF_XDP, posix_memalign, getpagesize, PTHREAD_PRIO_INHERIT, struct ifreq and
 * the IFF_* flags are all glibc feature-test gated; stock SOEM builds without
 * -std=c11 so they are exposed by default. Request them explicitly so this
 * overlay compiles regardless of the flags SOEM is built with. */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if_xdp.h>
#include <xdp/xsk.h>

#include "oshw.h"
#include "osal.h"

/* Busy-poll socket options may be missing from older <sys/socket.h>. */
#ifndef SO_BUSY_POLL
#  define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#  define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#  define SO_BUSY_POLL_BUDGET 70
#endif

/** Redundancy modes (identical to stock nicdrv). */
enum
{
   ECT_RED_NONE,
   ECT_RED_DOUBLE
};

/* Same identification MACs as stock SOEM: EtherCAT ignores MAC addressing but
 * SOEM uses the middle source word to route redundant frames. */
const uint16 priMAC[3] = { 0x0101, 0x0101, 0x0101 };
const uint16 secMAC[3] = { 0x0404, 0x0404, 0x0404 };

#define RX_PRIM priMAC[1]
#define RX_SEC secMAC[1]

/* ---------------------------------------------------------------------------
 * AF_XDP context. A single instance backs the one non-redundant EtherCAT port
 * the librmcs transport opens; the whole context is touched only by the thread
 * that owns the SOEM port (bring-up on the constructor thread, then the cycle
 * thread), never concurrently, so the fast path takes no locks.
 * ------------------------------------------------------------------------- */

#define XSK_FRAME_SIZE 2048u
#define XSK_NUM_FRAMES 4096u
#define XSK_RX_FRAMES  (XSK_NUM_FRAMES / 2u) /* posted to the fill ring       */
#define XSK_TX_FRAMES  (XSK_NUM_FRAMES - XSK_RX_FRAMES)
#define XSK_FILL_SIZE  XSK_RING_PROD__DEFAULT_NUM_DESCS
#define XSK_COMP_SIZE  XSK_RING_CONS__DEFAULT_NUM_DESCS
#define XSK_RX_SIZE    XSK_RING_CONS__DEFAULT_NUM_DESCS
#define XSK_TX_SIZE    XSK_RING_PROD__DEFAULT_NUM_DESCS
#define ETH_MIN_FRAME  60 /* AF_PACKET pads runts; the HW/we must do the same */

typedef struct
{
   void *umem_area;
   struct xsk_umem *umem;
   struct xsk_socket *xsk;
   struct xsk_ring_prod fill;
   struct xsk_ring_cons comp;
   struct xsk_ring_prod tx;
   struct xsk_ring_cons rx;
   int fd;
   int busy_poll; /* drive NAPI inline from recvfrom() */
   uint64 tx_free[XSK_TX_FRAMES];
   unsigned tx_free_top;   /* number of reusable TX chunks on the stack */
   uint32 outstanding_tx;  /* submitted, not yet completed */
   int active;
} xsk_ctx_t;

static xsk_ctx_t g_xsk;

static int env_int(const char *name, int fallback)
{
   const char *v = getenv(name);
   if (v == NULL || *v == '\0')
      return fallback;
   return (int)strtol(v, NULL, 0);
}

/* Bring the interface into promiscuous mode so return frames are accepted even
 * if a setup ever uses a unicast destination (stock nicdrv did this too). The
 * default EtherCAT destination is broadcast, so this is belt-and-suspenders. */
static void xsk_set_promisc(const char *ifname)
{
   int s = socket(AF_INET, SOCK_DGRAM, 0);
   struct ifreq ifr;
   if (s < 0)
      return;
   memset(&ifr, 0, sizeof(ifr));
   strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
   if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
   {
      ifr.ifr_flags |= IFF_PROMISC | IFF_BROADCAST;
      (void)ioctl(s, SIOCSIFFLAGS, &ifr);
   }
   close(s);
}

static void xsk_teardown(xsk_ctx_t *c)
{
   if (c->xsk != NULL)
      xsk_socket__delete(c->xsk);
   if (c->umem != NULL)
      (void)xsk_umem__delete(c->umem);
   if (c->umem_area != NULL)
      free(c->umem_area);
   memset(c, 0, sizeof(*c));
}

static int xsk_setup(xsk_ctx_t *c, const char *ifname)
{
   struct xsk_umem_config ucfg;
   struct xsk_socket_config scfg;
   size_t umem_size;
   uint32 queue;
   int copy_mode;
   int err;
   int one;
   int busy_us;
   int budget;
   uint32 idx;
   unsigned i;
   int uerr;

   memset(c, 0, sizeof(*c));
   queue = (uint32)env_int("RMCS_XDP_QUEUE", 0);
   copy_mode = env_int("RMCS_XDP_COPY", 0);
   busy_us = env_int("RMCS_XDP_BUSYPOLL_US", 20);
   budget = env_int("RMCS_XDP_BUSYPOLL_BUDGET", 8);

   xsk_set_promisc(ifname);

   umem_size = (size_t)XSK_NUM_FRAMES * XSK_FRAME_SIZE;
   if (posix_memalign(&c->umem_area, (size_t)getpagesize(), umem_size) != 0)
   {
      fprintf(stderr, "[rmcs-afxdp] UMEM alloc failed\n");
      return 0;
   }

   memset(&ucfg, 0, sizeof(ucfg));
   ucfg.fill_size = XSK_FILL_SIZE;
   ucfg.comp_size = XSK_COMP_SIZE;
   ucfg.frame_size = XSK_FRAME_SIZE;
   ucfg.frame_headroom = 0;
   ucfg.flags = 0;
   uerr = xsk_umem__create(&c->umem, c->umem_area, umem_size, &c->fill, &c->comp, &ucfg);
   if (uerr != 0)
   {
      fprintf(stderr, "[rmcs-afxdp] xsk_umem__create failed: %s\n", strerror(-uerr));
      free(c->umem_area);
      c->umem_area = NULL;
      return 0;
   }

   memset(&scfg, 0, sizeof(scfg));
   scfg.rx_size = XSK_RX_SIZE;
   scfg.tx_size = XSK_TX_SIZE;
   scfg.libbpf_flags = 0;
   scfg.xdp_flags = 0; /* let libxdp choose native/skb mode */
   scfg.bind_flags = XDP_USE_NEED_WAKEUP | (copy_mode ? XDP_COPY : XDP_ZEROCOPY);

   err = xsk_socket__create(&c->xsk, ifname, queue, c->umem, &c->rx, &c->tx, &scfg);
   if (err != 0 && !copy_mode)
   {
      /* Zero-copy not supported on this NIC/driver/queue: fall back to copy. */
      fprintf(stderr, "[rmcs-afxdp] zero-copy bind failed (%s), retrying copy mode\n",
              strerror(-err));
      scfg.bind_flags = XDP_USE_NEED_WAKEUP | XDP_COPY;
      copy_mode = 1;
      err = xsk_socket__create(&c->xsk, ifname, queue, c->umem, &c->rx, &c->tx, &scfg);
   }
   if (err != 0)
   {
      fprintf(stderr, "[rmcs-afxdp] xsk_socket__create(%s, q%u) failed: %s\n", ifname, queue,
              strerror(-err));
      xsk_teardown(c);
      return 0;
   }
   c->fd = xsk_socket__fd(c->xsk);

   /* Busy poll: recvfrom()/sendto() run the driver NAPI inline instead of
    * waiting for an interrupt. Best-effort -- failures just leave the socket
    * interrupt/softirq driven. */
   c->busy_poll = !env_int("RMCS_XDP_NO_BUSYPOLL", 0);
   if (c->busy_poll)
   {
      one = 1;
      (void)setsockopt(c->fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one));
      (void)setsockopt(c->fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
      (void)setsockopt(c->fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
   }

   /* Post the RX half of the UMEM to the fill ring. */
   if (xsk_ring_prod__reserve(&c->fill, XSK_RX_FRAMES, &idx) != XSK_RX_FRAMES)
   {
      fprintf(stderr, "[rmcs-afxdp] initial fill reserve failed\n");
      xsk_teardown(c);
      return 0;
   }
   for (i = 0; i < XSK_RX_FRAMES; i++)
      *xsk_ring_prod__fill_addr(&c->fill, idx + i) = (uint64)i * XSK_FRAME_SIZE;
   xsk_ring_prod__submit(&c->fill, XSK_RX_FRAMES);

   /* The TX half is a simple free stack of chunk-aligned addresses. */
   for (i = 0; i < XSK_TX_FRAMES; i++)
      c->tx_free[i] = (uint64)(XSK_RX_FRAMES + i) * XSK_FRAME_SIZE;
   c->tx_free_top = XSK_TX_FRAMES;

   c->active = 1;
   fprintf(stderr, "[rmcs-afxdp] up on %s queue %u: %s, busy-poll %s (%d us, budget %d)\n", ifname,
           queue, copy_mode ? "COPY" : "ZERO-COPY", c->busy_poll ? "on" : "off", busy_us, budget);
   return 1;
}

/* Reclaim completed TX chunks back onto the free stack. */
static void xsk_reap_completions(xsk_ctx_t *c)
{
   uint32 idx;
   unsigned n;
   unsigned i;

   if (c->outstanding_tx == 0)
      return;
   n = xsk_ring_cons__peek(&c->comp, 64, &idx);
   if (n == 0)
      return;
   for (i = 0; i < n; i++)
      c->tx_free[c->tx_free_top++] = *xsk_ring_cons__comp_addr(&c->comp, idx + i);
   xsk_ring_cons__release(&c->comp, n);
   c->outstanding_tx -= n;
}

/* Send one raw L2 frame. Returns bytes queued (>0) or -1, matching send(). */
static int xsk_send(xsk_ctx_t *c, const uint8 *buf, int len)
{
   uint64 addr;
   uint8 *pkt;
   uint32 idx;
   int wire;

   xsk_reap_completions(c);
   if (c->tx_free_top == 0)
   {
      xsk_reap_completions(c);
      if (c->tx_free_top == 0)
         return -1;
   }

   addr = c->tx_free[c->tx_free_top - 1];
   pkt = xsk_umem__get_data(c->umem_area, addr);
   wire = len;
   memcpy(pkt, buf, (size_t)len);
   if (wire < ETH_MIN_FRAME)
   {
      memset(pkt + len, 0, (size_t)(ETH_MIN_FRAME - len));
      wire = ETH_MIN_FRAME;
   }

   if (xsk_ring_prod__reserve(&c->tx, 1, &idx) != 1)
      return -1; /* TX ring full; leave the chunk on the free stack */
   c->tx_free_top--;

   {
      struct xdp_desc *d = xsk_ring_prod__tx_desc(&c->tx, idx);
      d->addr = addr;
      d->len = (uint32)wire;
   }
   xsk_ring_prod__submit(&c->tx, 1);
   c->outstanding_tx++;

   if (xsk_ring_prod__needs_wakeup(&c->tx))
      (void)sendto(c->fd, NULL, 0, MSG_DONTWAIT, NULL, 0);

   return len;
}

/* Non-blocking receive of one raw L2 frame into dst. Returns bytes read (>0)
 * or 0 when nothing is ready, matching the old recv(MSG_DONTWAIT). */
static int xsk_recv(xsk_ctx_t *c, uint8 *dst, int cap)
{
   uint32 idx;
   unsigned n;
   const struct xdp_desc *d;
   uint64 raw;
   uint64 chunk;
   const uint8 *pkt;
   int len;

   /* Drive NAPI inline (busy poll) so a returning frame is pulled without
    * waiting for the RX interrupt. Even without busy poll, a wakeup is needed
    * when the fill ring asks for one. */
   if (c->busy_poll)
      (void)recvfrom(c->fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
   else if (xsk_ring_prod__needs_wakeup(&c->fill))
      (void)recvfrom(c->fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);

   n = xsk_ring_cons__peek(&c->rx, 1, &idx);
   if (n == 0)
      return 0;

   d = xsk_ring_cons__rx_desc(&c->rx, idx);
   raw = d->addr;
   len = (int)d->len;
   if (len > cap)
      len = cap;
   pkt = xsk_umem__get_data(c->umem_area, xsk_umem__add_offset_to_addr(raw));
   memcpy(dst, pkt, (size_t)len);
   xsk_ring_cons__release(&c->rx, n);

   /* Recycle the chunk back to the fill ring. Aligned UMEM: strip the RX
    * headroom offset by rounding down to the chunk boundary. */
   chunk = xsk_umem__extract_addr(raw) & ~((uint64)XSK_FRAME_SIZE - 1);
   if (xsk_ring_prod__reserve(&c->fill, 1, &idx) == 1)
   {
      *xsk_ring_prod__fill_addr(&c->fill, idx) = chunk;
      xsk_ring_prod__submit(&c->fill, 1);
   }

   return len;
}

/* ---------------------------------------------------------------------------
 * SOEM nicdrv API. Buffer/index management below is byte-for-byte the stock
 * driver; only the socket I/O is redirected to the AF_XDP context above.
 * ------------------------------------------------------------------------- */

static void ecx_clear_rxbufstat(int *rxbufstat)
{
   int i;
   for (i = 0; i < EC_MAXBUF; i++)
      rxbufstat[i] = EC_BUF_EMPTY;
}

int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
   int i;
   pthread_mutexattr_t mutexattr;

   if (secondary)
   {
      /* AF_XDP redundancy is not implemented; librmcs uses a single port. */
      fprintf(stderr, "[rmcs-afxdp] redundant/secondary NIC not supported\n");
      return 0;
   }

   pthread_mutexattr_init(&mutexattr);
   pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
   pthread_mutex_init(&(port->getindex_mutex), &mutexattr);
   pthread_mutex_init(&(port->tx_mutex), &mutexattr);
   pthread_mutex_init(&(port->rx_mutex), &mutexattr);
   port->sockhandle = -1;
   port->lastidx = 0;
   port->redstate = ECT_RED_NONE;
   port->stack.sock = &(port->sockhandle);
   port->stack.txbuf = &(port->txbuf);
   port->stack.txbuflength = &(port->txbuflength);
   port->stack.tempbuf = &(port->tempinbuf);
   port->stack.rxbuf = &(port->rxbuf);
   port->stack.rxbufstat = &(port->rxbufstat);
   port->stack.rxsa = &(port->rxsa);
   ecx_clear_rxbufstat(&(port->rxbufstat[0]));

   if (!xsk_setup(&g_xsk, ifname))
      return 0;
   /* Expose the xsk fd through the usual field; teardown happens via g_xsk. */
   port->sockhandle = g_xsk.fd;

   /* Pre-fill the ethernet header of every TX buffer, exactly as stock. */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));

   return 1;
}

int ecx_closenic(ecx_portt *port)
{
   if (g_xsk.active)
      xsk_teardown(&g_xsk);
   port->sockhandle = -1;
   return 0;
}

void ec_setupheader(void *p)
{
   ec_etherheadert *bp;
   bp = p;
   bp->da0 = htons(0xffff);
   bp->da1 = htons(0xffff);
   bp->da2 = htons(0xffff);
   bp->sa0 = htons(priMAC[0]);
   bp->sa1 = htons(priMAC[1]);
   bp->sa2 = htons(priMAC[2]);
   bp->etype = htons(ETH_P_ECAT);
}

int ecx_getindex(ecx_portt *port)
{
   int idx;
   int cnt;

   pthread_mutex_lock(&(port->getindex_mutex));

   idx = port->lastidx + 1;
   if (idx >= EC_MAXBUF)
      idx = 0;
   cnt = 0;
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF)
         idx = 0;
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

   pthread_mutex_unlock(&(port->getindex_mutex));

   return idx;
}

void ecx_setbufstat(ecx_portt *port, int idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = bufstat;
}

int ecx_outframe(ecx_portt *port, int idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;

   if (!stacknumber)
      stack = &(port->stack);
   else
      stack = &(port->redport->stack);
   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;
   rval = xsk_send(&g_xsk, (const uint8 *)(*stack->txbuf)[idx], lp);
   if (rval == -1)
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;

   return rval;
}

int ecx_outframe_red(ecx_portt *port, int idx)
{
   ec_comt *datagramP;
   ec_etherheadert *ehp;
   int rval;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   /* rewrite MAC source address 1 to primary */
   ehp->sa1 = htons(priMAC[1]);
   /* transmit over primary */
   rval = ecx_outframe(port, idx, 0);
   if (port->redstate != ECT_RED_NONE)
   {
      pthread_mutex_lock(&(port->tx_mutex));
      ehp = (ec_etherheadert *)&(port->txbuf2);
      datagramP = (ec_comt *)&(port->txbuf2[ETH_HEADERSIZE]);
      datagramP->index = idx;
      ehp->sa1 = htons(secMAC[1]);
      port->redport->rxbufstat[idx] = EC_BUF_TX;
      if (xsk_send(&g_xsk, (const uint8 *)&(port->txbuf2), port->txbuflength2) == -1)
         port->redport->rxbufstat[idx] = EC_BUF_EMPTY;
      pthread_mutex_unlock(&(port->tx_mutex));
   }

   return rval;
}

static int ecx_recvpkt(ecx_portt *port, int stacknumber)
{
   int bytesrx;
   ec_stackT *stack;

   if (!stacknumber)
      stack = &(port->stack);
   else
      stack = &(port->redport->stack);
   bytesrx = xsk_recv(&g_xsk, (uint8 *)(*stack->tempbuf), (int)sizeof(port->tempinbuf));
   port->tempinbufs = bytesrx;

   return (bytesrx > 0);
}

int ecx_inframe(ecx_portt *port, int idx, int stacknumber)
{
   uint16 l;
   int rval;
   int idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   if (!stacknumber)
      stack = &(port->stack);
   else
      stack = &(port->redport->stack);
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];
   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else
   {
      pthread_mutex_lock(&(port->rx_mutex));
      if (ecx_recvpkt(port, stacknumber))
      {
         rval = EC_OTHERFRAME;
         ehp = (ec_etherheadert *)(stack->tempbuf);
         if (ehp->etype == htons(ETH_P_ECAT))
         {
            ecp = (ec_comt *)(&(*stack->tempbuf)[ETH_HEADERSIZE]);
            l = etohs(ecp->elength) & 0x0fff;
            idxf = ecp->index;
            if (idxf == idx)
            {
               memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE],
                      (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
               rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
               (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
               (*stack->rxsa)[idx] = ntohs(ehp->sa1);
            }
            else
            {
               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE],
                         (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = ntohs(ehp->sa1);
               }
            }
         }
      }
      pthread_mutex_unlock(&(port->rx_mutex));
   }

   return rval;
}

static int ecx_waitinframe_red(ecx_portt *port, int idx, osal_timert *timer)
{
   osal_timert timer2;
   int wkc = EC_NOFRAME;
   int wkc2 = EC_NOFRAME;
   int primrx, secrx;

   if (port->redstate == ECT_RED_NONE)
      wkc2 = 0;
   do
   {
      if (wkc <= EC_NOFRAME)
         wkc = ecx_inframe(port, idx, 0);
      if (port->redstate != ECT_RED_NONE)
      {
         if (wkc2 <= EC_NOFRAME)
            wkc2 = ecx_inframe(port, idx, 1);
      }
   } while (((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME)) && !osal_timer_is_expired(timer));

   if (port->redstate != ECT_RED_NONE)
   {
      primrx = 0;
      if (wkc > EC_NOFRAME)
         primrx = port->rxsa[idx];
      secrx = 0;
      if (wkc2 > EC_NOFRAME)
         secrx = port->redport->rxsa[idx];

      if (((primrx == RX_SEC) && (secrx == RX_PRIM)))
      {
         memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]),
                port->txbuflength[idx] - ETH_HEADERSIZE);
         wkc = wkc2;
      }
      if (((primrx == 0) && (secrx == RX_SEC)) || ((primrx == RX_PRIM) && (secrx == RX_SEC)))
      {
         if ((primrx == RX_PRIM) && (secrx == RX_SEC))
         {
            memcpy(&(port->txbuf[idx][ETH_HEADERSIZE]), &(port->rxbuf[idx]),
                   port->txbuflength[idx] - ETH_HEADERSIZE);
         }
         osal_timer_start(&timer2, EC_TIMEOUTRET);
         ecx_outframe(port, idx, 1);
         do
         {
            wkc2 = ecx_inframe(port, idx, 1);
         } while ((wkc2 <= EC_NOFRAME) && !osal_timer_is_expired(&timer2));
         if (wkc2 > EC_NOFRAME)
         {
            memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]),
                   port->txbuflength[idx] - ETH_HEADERSIZE);
            wkc = wkc2;
         }
      }
   }

   return wkc;
}

int ecx_waitinframe(ecx_portt *port, int idx, int timeout)
{
   int wkc;
   osal_timert timer;

   osal_timer_start(&timer, timeout);
   wkc = ecx_waitinframe_red(port, idx, &timer);

   return wkc;
}

int ecx_srconfirm(ecx_portt *port, int idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer1, timer2;

   osal_timer_start(&timer1, timeout);
   do
   {
      ecx_outframe_red(port, idx);
      if (timeout < EC_TIMEOUTRET)
         osal_timer_start(&timer2, timeout);
      else
         osal_timer_start(&timer2, EC_TIMEOUTRET);
      wkc = ecx_waitinframe_red(port, idx, &timer2);
   } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(&timer1));

   return wkc;
}

#ifdef EC_VER1
int ec_setupnic(const char *ifname, int secondary)
{
   return ecx_setupnic(&ecx_port, ifname, secondary);
}

int ec_closenic(void)
{
   return ecx_closenic(&ecx_port);
}

int ec_getindex(void)
{
   return ecx_getindex(&ecx_port);
}

void ec_setbufstat(int idx, int bufstat)
{
   ecx_setbufstat(&ecx_port, idx, bufstat);
}

int ec_outframe(int idx, int stacknumber)
{
   return ecx_outframe(&ecx_port, idx, stacknumber);
}

int ec_outframe_red(int idx)
{
   return ecx_outframe_red(&ecx_port, idx);
}

int ec_inframe(int idx, int stacknumber)
{
   return ecx_inframe(&ecx_port, idx, stacknumber);
}

int ec_waitinframe(int idx, int timeout)
{
   return ecx_waitinframe(&ecx_port, idx, timeout);
}

int ec_srconfirm(int idx, int timeout)
{
   return ecx_srconfirm(&ecx_port, idx, timeout);
}
#endif

#include "config.h" // CONFIG_*
#include "output.h" // dprintf
#include "string.h" // memcpy_fl
#include "util.h" // timer_calc
#include "x86.h" // readl
#include "malloc.h" // memalign_low
#include "pci.h" // pci_bdf_to_bus
#include "pci_regs.h" // PCI_BASE_ADDRESS_0
#include "usb.h" // struct usb_s
#include "usb-xhci.h" // struct ehci_qh
#include "biosvar.h" // GET_LOWFLAT

// --------------------------------------------------------------
// configuration

#define XHCI_RING_ITEMS          16
#define XHCI_RING_SIZE           (XHCI_RING_ITEMS*sizeof(struct xhci_trb))

/*
 *  xhci_ring structs are allocated with XHCI_RING_SIZE alignment,
 *  then we can get it from a trb pointer (provided by evt ring).
 */
#define XHCI_RING(_trb)          \
    ((struct xhci_ring*)((u32)(_trb) & ~(XHCI_RING_SIZE-1)))

// --------------------------------------------------------------
// bit definitions

#define XHCI_CMD_RS              (1<<0)
#define XHCI_CMD_HCRST           (1<<1)
#define XHCI_CMD_INTE            (1<<2)
#define XHCI_CMD_HSEE            (1<<3)
#define XHCI_CMD_LHCRST          (1<<7)
#define XHCI_CMD_CSS             (1<<8)
#define XHCI_CMD_CRS             (1<<9)
#define XHCI_CMD_EWE             (1<<10)
#define XHCI_CMD_EU3S            (1<<11)

#define XHCI_STS_HCH             (1<<0)
#define XHCI_STS_HSE             (1<<2)
#define XHCI_STS_EINT            (1<<3)
#define XHCI_STS_PCD             (1<<4)
#define XHCI_STS_SSS             (1<<8)
#define XHCI_STS_RSS             (1<<9)
#define XHCI_STS_SRE             (1<<10)
#define XHCI_STS_CNR             (1<<11)
#define XHCI_STS_HCE             (1<<12)

#define XHCI_PORTSC_CCS          (1<<0)
#define XHCI_PORTSC_PED          (1<<1)
#define XHCI_PORTSC_OCA          (1<<3)
#define XHCI_PORTSC_PR           (1<<4)
#define XHCI_PORTSC_PLS_SHIFT        5
#define XHCI_PORTSC_PLS_MASK     0xf
#define XHCI_PORTSC_PP           (1<<9)
#define XHCI_PORTSC_SPEED_SHIFT      10
#define XHCI_PORTSC_SPEED_MASK   0xf
#define XHCI_PORTSC_SPEED_FULL   (1<<10)
#define XHCI_PORTSC_SPEED_LOW    (2<<10)
#define XHCI_PORTSC_SPEED_HIGH   (3<<10)
#define XHCI_PORTSC_SPEED_SUPER  (4<<10)
#define XHCI_PORTSC_PIC_SHIFT        14
#define XHCI_PORTSC_PIC_MASK     0x3
#define XHCI_PORTSC_LWS          (1<<16)
#define XHCI_PORTSC_CSC          (1<<17)
#define XHCI_PORTSC_PEC          (1<<18)
#define XHCI_PORTSC_WRC          (1<<19)
#define XHCI_PORTSC_OCC          (1<<20)
#define XHCI_PORTSC_PRC          (1<<21)
#define XHCI_PORTSC_PLC          (1<<22)
#define XHCI_PORTSC_CEC          (1<<23)
#define XHCI_PORTSC_CAS          (1<<24)
#define XHCI_PORTSC_WCE          (1<<25)
#define XHCI_PORTSC_WDE          (1<<26)
#define XHCI_PORTSC_WOE          (1<<27)
#define XHCI_PORTSC_DR           (1<<30)
#define XHCI_PORTSC_WPR          (1<<31)

#define TRB_C               (1<<0)
#define TRB_TYPE_SHIFT          10
#define TRB_TYPE_MASK       0x3f
#define TRB_TYPE(t)         (((t) >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK)

#define TRB_EV_ED           (1<<2)

#define TRB_TR_ENT          (1<<1)
#define TRB_TR_ISP          (1<<2)
#define TRB_TR_NS           (1<<3)
#define TRB_TR_CH           (1<<4)
#define TRB_TR_IOC          (1<<5)
#define TRB_TR_IDT          (1<<6)
#define TRB_TR_TBC_SHIFT        7
#define TRB_TR_TBC_MASK     0x3
#define TRB_TR_BEI          (1<<9)
#define TRB_TR_TLBPC_SHIFT      16
#define TRB_TR_TLBPC_MASK   0xf
#define TRB_TR_FRAMEID_SHIFT    20
#define TRB_TR_FRAMEID_MASK 0x7ff
#define TRB_TR_SIA          (1<<31)

#define TRB_TR_DIR          (1<<16)

#define TRB_CR_SLOTID_SHIFT     24
#define TRB_CR_SLOTID_MASK  0xff
#define TRB_CR_EPID_SHIFT       16
#define TRB_CR_EPID_MASK    0x1f

#define TRB_CR_BSR          (1<<9)
#define TRB_CR_DC           (1<<9)

#define TRB_LK_TC           (1<<1)

#define TRB_INTR_SHIFT          22
#define TRB_INTR_MASK       0x3ff
#define TRB_INTR(t)         (((t).status >> TRB_INTR_SHIFT) & TRB_INTR_MASK)

typedef enum TRBType {
    TRB_RESERVED = 0,
    TR_NORMAL,
    TR_SETUP,
    TR_DATA,
    TR_STATUS,
    TR_ISOCH,
    TR_LINK,
    TR_EVDATA,
    TR_NOOP,
    CR_ENABLE_SLOT,
    CR_DISABLE_SLOT,
    CR_ADDRESS_DEVICE,
    CR_CONFIGURE_ENDPOINT,
    CR_EVALUATE_CONTEXT,
    CR_RESET_ENDPOINT,
    CR_STOP_ENDPOINT,
    CR_SET_TR_DEQUEUE,
    CR_RESET_DEVICE,
    CR_FORCE_EVENT,
    CR_NEGOTIATE_BW,
    CR_SET_LATENCY_TOLERANCE,
    CR_GET_PORT_BANDWIDTH,
    CR_FORCE_HEADER,
    CR_NOOP,
    ER_TRANSFER = 32,
    ER_COMMAND_COMPLETE,
    ER_PORT_STATUS_CHANGE,
    ER_BANDWIDTH_REQUEST,
    ER_DOORBELL,
    ER_HOST_CONTROLLER,
    ER_DEVICE_NOTIFICATION,
    ER_MFINDEX_WRAP,
} TRBType;

typedef enum TRBCCode {
    CC_INVALID = 0,
    CC_SUCCESS,
    CC_DATA_BUFFER_ERROR,
    CC_BABBLE_DETECTED,
    CC_USB_TRANSACTION_ERROR,
    CC_TRB_ERROR,
    CC_STALL_ERROR,
    CC_RESOURCE_ERROR,
    CC_BANDWIDTH_ERROR,
    CC_NO_SLOTS_ERROR,
    CC_INVALID_STREAM_TYPE_ERROR,
    CC_SLOT_NOT_ENABLED_ERROR,
    CC_EP_NOT_ENABLED_ERROR,
    CC_SHORT_PACKET,
    CC_RING_UNDERRUN,
    CC_RING_OVERRUN,
    CC_VF_ER_FULL,
    CC_PARAMETER_ERROR,
    CC_BANDWIDTH_OVERRUN,
    CC_CONTEXT_STATE_ERROR,
    CC_NO_PING_RESPONSE_ERROR,
    CC_EVENT_RING_FULL_ERROR,
    CC_INCOMPATIBLE_DEVICE_ERROR,
    CC_MISSED_SERVICE_ERROR,
    CC_COMMAND_RING_STOPPED,
    CC_COMMAND_ABORTED,
    CC_STOPPED,
    CC_STOPPED_LENGTH_INVALID,
    CC_MAX_EXIT_LATENCY_TOO_LARGE_ERROR = 29,
    CC_ISOCH_BUFFER_OVERRUN = 31,
    CC_EVENT_LOST_ERROR,
    CC_UNDEFINED_ERROR,
    CC_INVALID_STREAM_ID_ERROR,
    CC_SECONDARY_BANDWIDTH_ERROR,
    CC_SPLIT_TRANSACTION_ERROR
} TRBCCode;

enum {
    PLS_U0              =  0,
    PLS_U1              =  1,
    PLS_U2              =  2,
    PLS_U3              =  3,
    PLS_DISABLED        =  4,
    PLS_RX_DETECT       =  5,
    PLS_INACTIVE        =  6,
    PLS_POLLING         =  7,
    PLS_RECOVERY        =  8,
    PLS_HOT_RESET       =  9,
    PLS_COMPILANCE_MODE = 10,
    PLS_TEST_MODE       = 11,
    PLS_RESUME          = 15,
};

#define xhci_get_field(data, field)             \
    (((data) >> field##_SHIFT) & field##_MASK)

// --------------------------------------------------------------
// state structs

struct xhci_ring {
    struct xhci_trb      ring[XHCI_RING_ITEMS];
    struct xhci_trb      evt;
    u32                  eidx;
    u32                  nidx;
    u32                  cs;
    struct mutex_s       lock;
};

struct usb_xhci_s {
    struct usb_s         usb;
    struct usbhub_s      hub;

    /* devinfo */
    u32                  baseaddr;
    u32                  xcap;
    u32                  ports;
    u32                  slots;

    /* xhci registers */
    struct xhci_caps     *caps;
    struct xhci_op       *op;
    struct xhci_pr       *pr;
    struct xhci_ir       *ir;
    struct xhci_db       *db;

    /* xhci data structures */
    struct xhci_devlist  *devs;
    struct xhci_ring     *cmds;
    struct xhci_ring     *evts;
    struct xhci_er_seg   *eseg;

    /* usb devices */
    struct hlist_head    list;
};

struct xhci_device {
    struct xhci_devctx   devctx;
    struct xhci_inctx    inctx;

    struct usbdevice_s   *usbdev;
    struct usb_xhci_s    *xhci;
    u32                  slotid;
    struct hlist_node    next;
};

struct xhci_pipe {
    struct xhci_ring     reqs;

    struct usb_pipe      pipe;
    struct xhci_device   *dev;
    u32                  epid;
    void                 *buf;
    int                  bufused;
};

// --------------------------------------------------------------
// tables

static const char *speed_name[16] = {
    [ 0 ] = " - ",
    [ 1 ] = "Full",
    [ 2 ] = "Low",
    [ 3 ] = "High",
    [ 4 ] = "Super",
};

static const int speed_from_xhci[16] = {
    [ 0 ] = -1,
    [ 1 ] = USB_FULLSPEED,
    [ 2 ] = USB_LOWSPEED,
    [ 3 ] = USB_HIGHSPEED,
    [ 4 ] = USB_SUPERSPEED,
    [ 5 ... 15 ] = -1,
};

static const int speed_to_xhci[] = {
    [ USB_FULLSPEED  ] = 1,
    [ USB_LOWSPEED   ] = 2,
    [ USB_HIGHSPEED  ] = 3,
    [ USB_SUPERSPEED ] = 4,
};

static const int speed_to_ctlsize[] = {
    [ USB_FULLSPEED  ] = 8,
    [ USB_LOWSPEED   ] = 8,
    [ USB_HIGHSPEED  ] = 64,
    [ USB_SUPERSPEED ] = 256,
};

static const int eptype_to_xhci_in[] = {
    [ USB_ENDPOINT_XFER_CONTROL] = 4,
    [ USB_ENDPOINT_XFER_ISOC   ] = 5,
    [ USB_ENDPOINT_XFER_BULK   ] = 6,
    [ USB_ENDPOINT_XFER_INT    ] = 7,
};

static const int eptype_to_xhci_out[] = {
    [ USB_ENDPOINT_XFER_CONTROL] = 4,
    [ USB_ENDPOINT_XFER_ISOC   ] = 1,
    [ USB_ENDPOINT_XFER_BULK   ] = 2,
    [ USB_ENDPOINT_XFER_INT    ] = 3,
};

// --------------------------------------------------------------
// internal functions, 16bit + 32bit

static void xhci_doorbell(struct usb_xhci_s *xhci, u32 slotid, u32 value)
{
    struct xhci_db *db = GET_LOWFLAT(xhci->db);
    u32 addr = (u32)(&db[slotid].doorbell);
    pci_writel(addr, value);
}

static void xhci_process_events(struct usb_xhci_s *xhci)
{
    struct xhci_ring *evts = GET_LOWFLAT(xhci->evts);

    for (;;) {
        /* check for event */
        u32 nidx = GET_LOWFLAT(evts->nidx);
        u32 cs = GET_LOWFLAT(evts->cs);
        struct xhci_trb *etrb = evts->ring + nidx;
        u32 control = GET_LOWFLAT(etrb->control);
        if ((control & TRB_C) != (cs ? 1 : 0))
            return;

        /* process event */
        u32 evt_type = TRB_TYPE(control);
        u32 evt_cc = (GET_LOWFLAT(etrb->status) >> 24) & 0xff;
        switch (evt_type) {
        case ER_TRANSFER:
        case ER_COMMAND_COMPLETE:
        {
            struct xhci_trb  *rtrb = (void*)GET_LOWFLAT(etrb->ptr_low);
            struct xhci_ring *ring = XHCI_RING(rtrb);
            struct xhci_trb  *evt = &ring->evt;
            u32 eidx = rtrb - ring->ring + 1;
            dprintf(5, "%s: ring %p [trb %p, evt %p, type %d, eidx %d, cc %d]\n",
                    __func__, ring, rtrb, evt, evt_type, eidx, evt_cc);
            memcpy_fl(evt, etrb, sizeof(*etrb));
            SET_LOWFLAT(ring->eidx, eidx);
            break;
        }
        case ER_PORT_STATUS_CHANGE:
        {
            u32 portid = (GET_LOWFLAT(etrb->ptr_low) >> 24) & 0xff;
            dprintf(3, "%s: status change port #%d\n",
                    __func__, portid);
            break;
        }
        default:
            dprintf(1, "%s: unknown event, type %d, cc %d\n",
                    __func__, evt_type, evt_cc);
            break;
        }

        /* move ring index, notify xhci */
        nidx++;
        if (nidx == XHCI_RING_ITEMS) {
            nidx = 0;
            cs = cs ? 0 : 1;
            SET_LOWFLAT(evts->cs, cs);
        }
        SET_LOWFLAT(evts->nidx, nidx);
        struct xhci_ir *ir = GET_LOWFLAT(xhci->ir);
        u32 addr = (u32)(&ir->erdp_low);
        u32 erdp = (u32)(evts->ring + nidx);
        pci_writel(addr, erdp);
    }
}

static int xhci_ring_busy(struct xhci_ring *ring)
{
    u32 eidx = GET_LOWFLAT(ring->eidx);
    u32 nidx = GET_LOWFLAT(ring->nidx);
    return (eidx != nidx);
}

static int xhci_event_wait(struct usb_xhci_s *xhci,
                           struct xhci_ring *ring,
                           u32 timeout)
{
    u32 end = timer_calc(timeout);

    for (;;) {
        xhci_process_events(xhci);
        if (!xhci_ring_busy(ring)) {
            u32 status = GET_LOWFLAT(ring->evt.status);
            return (status >> 24) & 0xff;
        }
        if (timer_check(end)) {
            warn_timeout();
            return -1;
        }
        yield();
    }
}

static void xhci_trb_queue(struct xhci_ring *ring,
                           struct xhci_trb *trb)
{
    u32 nidx = GET_LOWFLAT(ring->nidx);
    u32 cs   = GET_LOWFLAT(ring->cs);
    struct xhci_trb *dst;
    u32 control;

    if (nidx == XHCI_RING_ITEMS-1) {
        dst = ring->ring + nidx;
        control  = (TR_LINK << 10); // trb type
        control |= TRB_LK_TC;
        control |= (cs ? TRB_C : 0);
        SET_LOWFLAT(dst->ptr_low,  (u32)&ring[0]);
        SET_LOWFLAT(dst->ptr_high, 0);
        SET_LOWFLAT(dst->status,   0);
        SET_LOWFLAT(dst->control,  control);
        nidx = 0;
        cs = cs ? 0 : 1;
        SET_LOWFLAT(ring->nidx, nidx);
        SET_LOWFLAT(ring->cs,   cs);

        dprintf(5, "%s: ring %p [linked]\n", __func__, ring);
    }

    dst = ring->ring + nidx;
    control = GET_LOWFLAT(trb->control) | (cs ? TRB_C : 0);

    SET_LOWFLAT(dst->ptr_low,  GET_LOWFLAT(trb->ptr_low));
    SET_LOWFLAT(dst->ptr_high, GET_LOWFLAT(trb->ptr_high));
    SET_LOWFLAT(dst->status,   GET_LOWFLAT(trb->status));
    SET_LOWFLAT(dst->control,  control);
    nidx++;
    SET_LOWFLAT(ring->nidx, nidx);

    dprintf(5, "%s: ring %p [nidx %d, len %d]\n",
            __func__, ring, nidx,
            GET_LOWFLAT(trb->status) & 0xffff);
}

static void xhci_xfer_queue(struct xhci_pipe *pipe,
                            struct xhci_trb *trb)
{
    xhci_trb_queue(&pipe->reqs, trb);
}

static void xhci_xfer_kick(struct xhci_pipe *pipe)
{
    struct xhci_device *dev = GET_LOWFLAT(pipe->dev);
    struct usb_xhci_s *xhci = GET_LOWFLAT(dev->xhci);
    u32 slotid = GET_LOWFLAT(dev->slotid);
    u32 epid = GET_LOWFLAT(pipe->epid);

    dprintf(5, "%s: ring %p, slotid %d, epid %d\n",
            __func__, &pipe->reqs, slotid, epid);
    xhci_doorbell(xhci, slotid, epid);
}

static void xhci_xfer_normal(struct xhci_pipe *pipe,
                             void *data, int datalen)
{
    struct xhci_trb trb;

    memset(&trb, 0, sizeof(trb));
    trb.ptr_low  = (u32)data;
    trb.status   = datalen;
    trb.control  |= (TR_NORMAL << 10); // trb type
    trb.control  |= TRB_TR_IOC;

    xhci_xfer_queue(pipe, MAKE_FLATPTR(GET_SEG(SS), &trb));
    xhci_xfer_kick(pipe);
}

// --------------------------------------------------------------
// internal functions, pure 32bit

static int wait_bit(u32 *reg, u32 mask, int value, u32 timeout)
{
    ASSERT32FLAT();
    u32 end = timer_calc(timeout);

    while ((readl(reg) & mask) != value) {
        if (timer_check(end)) {
            warn_timeout();
            return -1;
        }
        yield();
    }
    return 0;
}

static int xhci_cmd_submit(struct usb_xhci_s *xhci,
                           struct xhci_trb *cmd)
{
    ASSERT32FLAT();
    int rc;

    mutex_lock(&xhci->cmds->lock);
    xhci_trb_queue(xhci->cmds, cmd);
    xhci_doorbell(xhci, 0, 0);
    rc = xhci_event_wait(xhci, xhci->cmds, 1000);
    mutex_unlock(&xhci->cmds->lock);
    return rc;
}

static int xhci_cmd_enable_slot(struct usb_xhci_s *xhci)
{
    ASSERT32FLAT();
    struct xhci_trb cmd = {
        .ptr_low  = 0,
        .ptr_high = 0,
        .status   = 0,
        .control  = (CR_ENABLE_SLOT << 10)
    };
    dprintf(3, "%s:\n", __func__);
    int cc = xhci_cmd_submit(xhci, &cmd);
    if (cc != CC_SUCCESS)
        return -1;
    return (xhci->cmds->evt.control >> 24) & 0xff;
}

static int xhci_cmd_disable_slot(struct xhci_device *dev)
{
    ASSERT32FLAT();
    struct xhci_trb cmd = {
        .ptr_low  = 0,
        .ptr_high = 0,
        .status   = 0,
        .control  = (dev->slotid << 24) | (CR_DISABLE_SLOT << 10)
    };
    dprintf(3, "%s: slotid %d\n", __func__, dev->slotid);
    return xhci_cmd_submit(dev->xhci, &cmd);
}

static int xhci_cmd_address_device(struct xhci_device *dev)
{
    ASSERT32FLAT();
    struct xhci_trb cmd = {
        .ptr_low  = (u32)&dev->inctx,
        .ptr_high = 0,
        .status   = 0,
        .control  = (dev->slotid << 24) | (CR_ADDRESS_DEVICE << 10)
    };
    dprintf(3, "%s: slotid %d\n", __func__, dev->slotid);
    return xhci_cmd_submit(dev->xhci, &cmd);
}

static int xhci_cmd_configure_endpoint(struct xhci_device *dev)
{
    ASSERT32FLAT();
    struct xhci_trb cmd = {
        .ptr_low  = (u32)&dev->inctx,
        .ptr_high = 0,
        .status   = 0,
        .control  = (dev->slotid << 24) | (CR_CONFIGURE_ENDPOINT << 10)
    };
    dprintf(3, "%s: slotid %d, add 0x%x, del 0x%x\n", __func__,
            dev->slotid, dev->inctx.add, dev->inctx.del);
    return xhci_cmd_submit(dev->xhci, &cmd);
}

static int xhci_cmd_evaluate_context(struct xhci_device *dev)
{
    ASSERT32FLAT();
    struct xhci_trb cmd = {
        .ptr_low  = (u32)&dev->inctx,
        .ptr_high = 0,
        .status   = 0,
        .control  = (dev->slotid << 24) | (CR_EVALUATE_CONTEXT << 10)
    };
    dprintf(3, "%s: slotid %d, add 0x%x, del 0x%x\n", __func__,
            dev->slotid, dev->inctx.add, dev->inctx.del);
    return xhci_cmd_submit(dev->xhci, &cmd);
}

static void xhci_xfer_setup(struct xhci_pipe *pipe,
                            const struct usb_ctrlrequest *req,
                            int dir, int datalen)
{
    ASSERT32FLAT();
    struct xhci_trb trb;

    memset(&trb, 0, sizeof(trb));
    trb.ptr_low  |= req->bRequestType;
    trb.ptr_low  |= (req->bRequest) << 8;
    trb.ptr_low  |= (req->wValue) << 16;
    trb.ptr_high |= req->wIndex;
    trb.ptr_high |= (req->wLength) << 16;
    trb.status   |= 8;                // length
    trb.control  |= (TR_SETUP << 10); // trb type
    trb.control  |= TRB_TR_IDT;
    if (datalen)
        trb.control |= (dir ? 3 : 2) << 16; // transfer type
    xhci_xfer_queue(pipe, &trb);
}

static void xhci_xfer_data(struct xhci_pipe *pipe,
                           int dir, void *data, int datalen)
{
    ASSERT32FLAT();
    struct xhci_trb trb;

    memset(&trb, 0, sizeof(trb));
    trb.ptr_low  = (u32)data;
    trb.status   = datalen;
    trb.control  |= (TR_DATA << 10); // trb type
    if (dir)
        trb.control |= (1 << 16);
    xhci_xfer_queue(pipe, &trb);
}

static void xhci_xfer_status(struct xhci_pipe *pipe, int dir)
{
    ASSERT32FLAT();
    struct xhci_trb trb;

    memset(&trb, 0, sizeof(trb));
    trb.control  |= (TR_STATUS << 10); // trb type
    trb.control  |= TRB_TR_IOC;
    if (dir)
        trb.control |= (1 << 16);

    xhci_xfer_queue(pipe, &trb);
    xhci_xfer_kick(pipe);
}

static struct xhci_device *xhci_find_alloc_device(struct usb_xhci_s *xhci,
                                                  struct usbdevice_s *usbdev)
{
    ASSERT32FLAT();
    struct xhci_device *dev;

    hlist_for_each_entry(dev, &xhci->list, next) {
        if (dev->usbdev == usbdev) {
            return dev;
        }
    }

    dev = memalign_low(64, sizeof(*dev));
    if (!dev) {
        warn_noalloc();
        return NULL;
    }
    memset(dev, 0, sizeof(*dev));
    dev->usbdev = usbdev;
    dev->xhci = xhci;
    hlist_add_head(&dev->next, &xhci->list);
    return dev;
}

static void
configure_xhci(void *data)
{
    ASSERT32FLAT();
    struct usb_xhci_s *xhci = data;
    u32 reg;

    xhci->devs = memalign_high(64, sizeof(*xhci->devs) * (xhci->slots + 1));
    xhci->eseg = memalign_high(64, sizeof(*xhci->eseg));
    xhci->cmds = memalign_high(XHCI_RING_SIZE, sizeof(*xhci->cmds));
    xhci->evts = memalign_low(XHCI_RING_SIZE, sizeof(*xhci->evts));
    if (!xhci->devs || !xhci->cmds || !xhci->evts || !xhci->eseg) {
        warn_noalloc();
        goto fail;
    }
    memset(xhci->devs, 0, sizeof(*xhci->devs) * (xhci->slots + 1));
    memset(xhci->cmds, 0, sizeof(*xhci->cmds));
    memset(xhci->evts, 0, sizeof(*xhci->evts));
    memset(xhci->eseg, 0, sizeof(*xhci->eseg));

    reg = readl(&xhci->op->usbcmd);
    if (reg & XHCI_CMD_RS) {
        reg &= ~XHCI_CMD_RS;
        writel(&xhci->op->usbcmd, reg);
        if (wait_bit(&xhci->op->usbsts, XHCI_STS_HCH, XHCI_STS_HCH, 32) != 0)
            goto fail;
    }

    dprintf(3, "%s: resetting\n", __func__);
    writel(&xhci->op->usbcmd, XHCI_CMD_HCRST);
    if (wait_bit(&xhci->op->usbcmd, XHCI_CMD_HCRST, 0, 100) != 0)
        goto fail;
    if (wait_bit(&xhci->op->usbsts, XHCI_STS_CNR, 0, 100) != 0)
        goto fail;

    writel(&xhci->op->config, xhci->slots);
    writel(&xhci->op->dcbaap_low, (u32)xhci->devs);
    writel(&xhci->op->dcbaap_high, 0);
    writel(&xhci->op->crcr_low, (u32)xhci->cmds | 1);
    writel(&xhci->op->crcr_high, 0);
    xhci->cmds->cs = 1;

    xhci->eseg->ptr_low = (u32)xhci->evts;
    xhci->eseg->ptr_high = 0;
    xhci->eseg->size = XHCI_RING_ITEMS;
    writel(&xhci->ir->erstsz, 1);
    writel(&xhci->ir->erdp_low, (u32)xhci->evts);
    writel(&xhci->ir->erdp_high, 0);
    writel(&xhci->ir->erstba_low, (u32)xhci->eseg);
    writel(&xhci->ir->erstba_high, 0);
    xhci->evts->cs = 1;

    reg = readl(&xhci->op->usbcmd);
    reg |= XHCI_CMD_RS;
    writel(&xhci->op->usbcmd, reg);

    // FIXME: try find a more elegant way than a fixed delay
    mdelay(100);

    usb_enumerate(&xhci->hub);
    if (xhci->hub.devcount)
        return;

    // No devices found - shutdown and free controller.
    dprintf(1, "XHCI no devices found\n");
    reg = readl(&xhci->op->usbcmd);
    reg &= ~XHCI_CMD_RS;
    writel(&xhci->op->usbcmd, reg);
    wait_bit(&xhci->op->usbsts, XHCI_STS_HCH, XHCI_STS_HCH, 32);

fail:
    free(xhci->eseg);
    free(xhci->evts);
    free(xhci->cmds);
    free(xhci->devs);
    free(xhci);
}

// --------------------------------------------------------------
// xhci root hub

// Check if device attached to port
static void
xhci_print_port_state(int loglevel, const char *prefix, u32 port, u32 portsc)
{
    ASSERT32FLAT();
    u32 pls = xhci_get_field(portsc, XHCI_PORTSC_PLS);
    u32 speed = xhci_get_field(portsc, XHCI_PORTSC_SPEED);

    dprintf(loglevel, "%s port #%d: 0x%08x,%s%s pls %d, speed %d [%s]\n",
            prefix, port + 1, portsc,
            (portsc & XHCI_PORTSC_PP)  ? " powered," : "",
            (portsc & XHCI_PORTSC_PED) ? " enabled," : "",
            pls, speed, speed_name[speed]);
}

static int
xhci_hub_detect(struct usbhub_s *hub, u32 port)
{
    ASSERT32FLAT();
    struct usb_xhci_s *xhci = container_of(hub->cntl, struct usb_xhci_s, usb);
    u32 portsc = readl(&xhci->pr[port].portsc);

    xhci_print_port_state(3, __func__, port, portsc);
    switch (xhci_get_field(portsc, XHCI_PORTSC_PLS)) {
    case PLS_U0:
    case PLS_POLLING:
        return 0;
    default:
        return -1;
    }
}

// Reset device on port
static int
xhci_hub_reset(struct usbhub_s *hub, u32 port)
{
    ASSERT32FLAT();
    struct usb_xhci_s *xhci = container_of(hub->cntl, struct usb_xhci_s, usb);
    u32 portsc = readl(&xhci->pr[port].portsc);
    int rc;

    switch (xhci_get_field(portsc, XHCI_PORTSC_PLS)) {
    case PLS_U0:
        rc = speed_from_xhci[xhci_get_field(portsc, XHCI_PORTSC_SPEED)];
        break;
    case PLS_POLLING:
        xhci_print_port_state(3, __func__, port, portsc);
        portsc |= XHCI_PORTSC_PR;
        writel(&xhci->pr[port].portsc, portsc);
        if (wait_bit(&xhci->pr[port].portsc, XHCI_PORTSC_PED, XHCI_PORTSC_PED, 100) != 0)
            return -1;
        portsc = readl(&xhci->pr[port].portsc);
        rc = speed_from_xhci[xhci_get_field(portsc, XHCI_PORTSC_SPEED)];
        break;
    default:
        rc = -1;
        break;
    }

    xhci_print_port_state(1, "XHCI", port, portsc);
    return rc;
}

static void
xhci_hub_disconnect(struct usbhub_s *hub, u32 port)
{
    ASSERT32FLAT();
    struct usb_xhci_s *xhci = container_of(hub->cntl, struct usb_xhci_s, usb);
    struct xhci_device *dev;

    hlist_for_each_entry(dev, &xhci->list, next) {
        if (dev->usbdev->hub == hub &&
            dev->usbdev->port == port &&
            dev->slotid != 0) {
            xhci_cmd_disable_slot(dev);
            hlist_del(&dev->next);
            return;
        }
    }
}

static struct usbhub_op_s xhci_hub_ops = {
    .detect = xhci_hub_detect,
    .reset = xhci_hub_reset,
    .disconnect = xhci_hub_disconnect,
};

// --------------------------------------------------------------
// external interface

struct usb_pipe *
xhci_alloc_pipe(struct usbdevice_s *usbdev
                , struct usb_endpoint_descriptor *epdesc)
{
    ASSERT32FLAT();
    if (!CONFIG_USB_XHCI)
        return NULL;
    u8 eptype = epdesc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
    struct usb_xhci_s *xhci = container_of(
        usbdev->hub->cntl, struct usb_xhci_s, usb);
    struct xhci_pipe *pipe;
    u32 epid;

    if (epdesc->bEndpointAddress == 0) {
        epid = 1;
    } else {
        epid = (epdesc->bEndpointAddress & 0x0f) * 2;
        epid += (epdesc->bEndpointAddress & USB_DIR_IN) ? 1 : 0;
    }

    if (eptype == USB_ENDPOINT_XFER_CONTROL)
        pipe = memalign_high(XHCI_RING_SIZE, sizeof(*pipe));
    else
        pipe = memalign_low(XHCI_RING_SIZE, sizeof(*pipe));
    if (!pipe) {
        warn_noalloc();
        return NULL;
    }
    memset(pipe, 0, sizeof(*pipe));

    usb_desc2pipe(&pipe->pipe, usbdev, epdesc);
    pipe->dev = xhci_find_alloc_device(xhci, usbdev);
    if (!pipe->dev) {
        free(pipe);
        return NULL;
    }
    pipe->epid = epid;
    pipe->reqs.cs = 1;
    if (eptype == USB_ENDPOINT_XFER_INT)
        pipe->buf = malloc_low(pipe->pipe.maxpacket);

    dprintf(3, "%s: usbdev %p, ring %p, slotid %d, epid %d\n", __func__,
            usbdev, &pipe->reqs, pipe->dev->slotid, pipe->epid);
    if (pipe->epid > 1 && pipe->dev->slotid) {
        struct xhci_inctx *in = &pipe->dev->inctx;
        in->add = (1 << pipe->epid) | 1;
        in->del = 0;

        in->slot.ctx[0]    |= (31 << 27); // context entries

        int e = pipe->epid-1;
        in->ep[e].ctx[1]   |= (eptype << 3);
        if (epdesc->bEndpointAddress & USB_DIR_IN)
            in->ep[e].ctx[1] |= (1 << 5);
        in->ep[e].ctx[1]   |= (pipe->pipe.maxpacket << 16);
        in->ep[e].deq_low  = (u32)&pipe->reqs.ring[0];
        in->ep[e].deq_low  |= 1;         // dcs
        in->ep[e].deq_high = 0;
        in->ep[e].length   = pipe->pipe.maxpacket;

        int cc = xhci_cmd_configure_endpoint(pipe->dev);
        if (cc != CC_SUCCESS) {
            dprintf(1, "%s: configure endpoint: failed (cc %d)\n", __func__, cc);
            free(pipe);
            return NULL;
        }
    }

    return &pipe->pipe;
}

struct usb_pipe *
xhci_update_pipe(struct usbdevice_s *usbdev, struct usb_pipe *upipe
                , struct usb_endpoint_descriptor *epdesc)
{
    ASSERT32FLAT();
    if (!CONFIG_USB_XHCI)
        return NULL;
    u8 eptype = epdesc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
    struct xhci_pipe *pipe = container_of(upipe, struct xhci_pipe, pipe);
    dprintf(3, "%s: usbdev %p, ring %p, slotid %d, epid %d\n", __func__,
            usbdev, &pipe->reqs, pipe->dev->slotid, pipe->epid);
    if (eptype == USB_ENDPOINT_XFER_CONTROL &&
        pipe->pipe.maxpacket !=  epdesc->wMaxPacketSize) {
        dprintf(1, "%s: reconf ctl endpoint pkt size: %d -> %d\n",
                __func__, pipe->pipe.maxpacket, epdesc->wMaxPacketSize);
        pipe->pipe.maxpacket = epdesc->wMaxPacketSize;
        struct xhci_inctx *in = &pipe->dev->inctx;
        in->add = (1 << 1);
        in->del = 0;
        in->ep[0].ctx[1] &= 0xffff;
        in->ep[0].ctx[1] |= (pipe->pipe.maxpacket << 16);
        int cc = xhci_cmd_evaluate_context(pipe->dev);
        if (cc != CC_SUCCESS) {
            dprintf(1, "%s: reconf ctl endpoint: failed (cc %d)\n",
                    __func__, cc);
        }
    }
    return upipe;
}

int
xhci_control(struct usb_pipe *p, int dir, const void *cmd, int cmdsize
             , void *data, int datalen)
{
    ASSERT32FLAT();
    if (!CONFIG_USB_XHCI)
        return -1;
    const struct usb_ctrlrequest *req = cmd;
    struct xhci_pipe *pipe = container_of(p, struct xhci_pipe, pipe);
    struct usb_xhci_s *xhci = pipe->dev->xhci;
    int cc;

    if (req->bRequest == USB_REQ_SET_ADDRESS) {
        int slotid = xhci_cmd_enable_slot(xhci);
        if (slotid < 0) {
            dprintf(1, "%s: enable slot: failed\n", __func__);
            return -1;
        }
        dprintf(3, "%s: enable slot: got slotid %d\n", __func__, slotid);
        pipe->dev->slotid = slotid;
        xhci->devs[slotid].ptr_low = (u32)&pipe->dev->devctx;
        xhci->devs[slotid].ptr_high = 0;

        struct usbdevice_s *usbdev = pipe->dev->usbdev;
        u32 route = 0;
        while (usbdev->hub->usbdev) {
            route <<= 4;
            route |= (usbdev->port+1) & 0xf;
            usbdev = usbdev->hub->usbdev;
        }
        dprintf(3, "%s: root port %d, route 0x%x\n",
                __func__, usbdev->port+1, route);

        struct xhci_inctx *in = &pipe->dev->inctx;
        in->add = 0x03;
        in->slot.ctx[0]    |= (1 << 27); // context entries
        in->slot.ctx[0]    |= speed_to_xhci[pipe->dev->usbdev->speed] << 20;
        in->slot.ctx[0]    |= route;
        in->slot.ctx[1]    |= (usbdev->port+1) << 16;
        /* TODO ctx0: hub bit */
        /* TODO ctx1: hub ports */

        in->ep[0].ctx[0]   |= (3 << 16); // interval: 1ms
        in->ep[0].ctx[1]   |= (4 << 3);  // control pipe
        in->ep[0].ctx[1]   |= (speed_to_ctlsize[pipe->dev->usbdev->speed] << 16);

        in->ep[0].deq_low  = (u32)&pipe->reqs.ring[0];
        in->ep[0].deq_low  |= 1;         // dcs
        in->ep[0].deq_high = 0;
        in->ep[0].length   = 8;

        cc = xhci_cmd_address_device(pipe->dev);
        if (cc != CC_SUCCESS) {
            dprintf(1, "%s: address device: failed (cc %d)\n", __func__, cc);
            return -1;
        }
        return 0;
    }

    xhci_xfer_setup(pipe, req, dir, datalen);
    if (datalen)
        xhci_xfer_data(pipe, dir, data, datalen);
    xhci_xfer_status(pipe, dir);

    cc = xhci_event_wait(xhci, &pipe->reqs, 1000);
    if (cc != CC_SUCCESS) {
        dprintf(1, "%s: control xfer failed (cc %d)\n", __func__, cc);
        return -1;
    }

    return 0;
}

int
xhci_send_bulk(struct usb_pipe *p, int dir, void *data, int datalen)
{
    if (!CONFIG_USB_XHCI)
        return -1;

    struct xhci_pipe *pipe = container_of(p, struct xhci_pipe, pipe);
    struct xhci_device *dev = GET_LOWFLAT(pipe->dev);
    struct usb_xhci_s *xhci = GET_LOWFLAT(dev->xhci);

    xhci_xfer_normal(pipe, data, datalen);
    int cc = xhci_event_wait(xhci, &pipe->reqs, 1000);
    if (cc != CC_SUCCESS) {
        dprintf(1, "%s: bulk xfer failed (cc %d)\n", __func__, cc);
        return -1;
    }
    return 0;
}

int
xhci_poll_intr(struct usb_pipe *p, void *data)
{
    if (!CONFIG_USB_XHCI)
        return -1;

    struct xhci_pipe *pipe = container_of(p, struct xhci_pipe, pipe);
    struct xhci_device *dev = GET_LOWFLAT(pipe->dev);
    struct usb_xhci_s *xhci = GET_LOWFLAT(dev->xhci);
    u32 len = GET_LOWFLAT(pipe->pipe.maxpacket);
    void *buf = GET_LOWFLAT(pipe->buf);
    int bufused = GET_LOWFLAT(pipe->bufused);

    if (!bufused) {
        xhci_xfer_normal(pipe, buf, len);
        bufused = 1;
        SET_LOWFLAT(pipe->bufused, bufused);
        return -1;
    }

    xhci_process_events(xhci);
    if (xhci_ring_busy(&pipe->reqs))
        return -1;
    dprintf(5, "%s: st %x ct %x [ %p <= %p / %d ]\n", __func__,
            GET_LOWFLAT(pipe->reqs.evt.status),
            GET_LOWFLAT(pipe->reqs.evt.control),
            MAKE_FLATPTR(GET_SEG(SS), data), buf, len);
    memcpy_fl(MAKE_FLATPTR(GET_SEG(SS), data), buf, len);
    xhci_xfer_normal(pipe, buf, len);
    return 0;
}

int
xhci_setup(struct pci_device *pci, int busid)
{
    ASSERT32FLAT();
    if (!CONFIG_USB_XHCI)
        return -1;

    struct usb_xhci_s *xhci = malloc_low(sizeof(*xhci));
    if (!xhci) {
        warn_noalloc();
        return -1;
    }
    memset(xhci, 0, sizeof(*xhci));

    xhci->baseaddr = pci_config_readl(pci->bdf, PCI_BASE_ADDRESS_0)
        & PCI_BASE_ADDRESS_MEM_MASK;
    xhci->caps  = (void*)(xhci->baseaddr);
    xhci->op    = (void*)(xhci->baseaddr + readb(&xhci->caps->caplength));
    xhci->pr    = (void*)(xhci->baseaddr + readb(&xhci->caps->caplength) + 0x400);
    xhci->db    = (void*)(xhci->baseaddr + readl(&xhci->caps->dboff));
    xhci->ir    = (void*)(xhci->baseaddr + readl(&xhci->caps->rtsoff) + 0x20);

    u32 hcs1 = readl(&xhci->caps->hcsparams1);
    u32 hcc  = readl(&xhci->caps->hccparams);
    xhci->ports = (hcs1 >> 24) & 0xff;
    xhci->slots = hcs1         & 0xff;
    xhci->xcap  = ((hcc >> 16) & 0xffff) << 2;

    xhci->usb.busid = busid;
    xhci->usb.pci = pci;
    xhci->usb.type = USB_TYPE_XHCI;
    xhci->hub.cntl = &xhci->usb;
    xhci->hub.portcount = xhci->ports;
    xhci->hub.op = &xhci_hub_ops;

    dprintf(1, "XHCI init on dev %02x:%02x.%x: regs @ %p, %d ports, %d slots\n"
            , pci_bdf_to_bus(pci->bdf), pci_bdf_to_dev(pci->bdf)
            , pci_bdf_to_fn(pci->bdf), xhci->caps
            , xhci->ports, xhci->slots);

    if (xhci->xcap) {
        u32 off, addr = xhci->baseaddr + xhci->xcap;
        do {
            struct xhci_xcap *xcap = (void*)addr;
            u32 ports, name, cap = readl(&xcap->cap);
            switch (cap & 0xff) {
            case 0x02:
                name  = readl(&xcap->data[0]);
                ports = readl(&xcap->data[1]);
                dprintf(1, "XHCI    protocol %c%c%c%c %x.%02x, %d ports (offset %d)\n"
                        , (name >>  0) & 0xff
                        , (name >>  8) & 0xff
                        , (name >> 16) & 0xff
                        , (name >> 24) & 0xff
                        , (cap >> 24) & 0xff
                        , (cap >> 16) & 0xff
                        , (ports >>  8) & 0xff
                        , (ports >>  0) & 0xff);
                break;
            default:
                dprintf(1, "XHCI    extcap 0x%x @ %x\n", cap & 0xff, addr);
                break;
            }
            off = (cap >> 8) & 0xff;
            addr += off << 2;
        } while (off > 0);
    }

    pci_config_maskw(pci->bdf, PCI_COMMAND, 0, PCI_COMMAND_MASTER);

    run_thread(configure_xhci, xhci);
    return 0;
}

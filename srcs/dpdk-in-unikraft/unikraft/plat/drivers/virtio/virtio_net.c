/*
 * Authors: Dan Williams
 *          Martin Lucina
 *          Ricardo Koller
 *          Razvan Cojocaru <razvan.cojocaru93@gmail.com>
 *          Sharan Santhanam
 *
 * Copyright (c) 2015-2017 IBM
 * Copyright (c) 2016-2017 Docker, Inc.
 * Copyright (c) 2018, NEC Europe Ltd., NEC Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uk/print.h>
#include <uk/assert.h>
#include <uk/essentials.h>
#include <uk/sglist.h>
#include <uk/arch/types.h>
#include <uk/arch/limits.h>
#include <uk/netbuf.h>
#include <uk/netdev.h>
#include <uk/netdev_core.h>
#include <uk/netdev_driver.h>
#include <virtio/virtio_bus.h>
#include <virtio/virtqueue.h>
#include <virtio/virtio_net.h>

/**
 * VIRTIO_PKT_BUFFER_LEN = VIRTIO_NET_HDR + ETH_HDR + ETH_PKT_PAYLOAD_LEN
 * VIRTIO_NET_HDR: 10 bytes in length in legacy mode + 2 byte of padded data.
 *		   12 bytes in length in modern mode.
 */
#define VIRTIO_HDR_LEN          12
#define VIRTIO_PKT_BUFFER_LEN ((UK_ETH_PAYLOAD_MAXLEN) \
			       + (UK_ETH_HDR_UNTAGGED_LEN) \
			       + (VIRTIO_HDR_LEN))

#define DRIVER_NAME           "virtio-net"


#define  VTNET_RX_HEADER_PAD (4)
#define  VTNET_INTR_EN   (1 << 0)
#define  VTNET_INTR_EN_MASK   (1)
#define  VTNET_INTR_USR_EN   (1 << 1)
#define  VTNET_INTR_USR_EN_MASK   (2)

/**
 * Define max possible fragments for the network packets.
 */
#define NET_MAX_FRAGMENTS    ((__U16_MAX >> __PAGE_SHIFT) + 2)
#define NET_MAX_PKT_BURST	(128)

#define to_virtionetdev(ndev) \
	__containerof(ndev, struct virtio_net_device, netdev)

#define VIRTIO_NET_DRV_FEATURES(features)           \
	(VIRTIO_FEATURES_UPDATE(features, VIRTIO_NET_F_MAC));  \
	(VIRTIO_FEATURES_UPDATE(features, VIRTIO_F_ANY_LAYOUT));\
	(VIRTIO_FEATURES_UPDATE(features, VIRTIO_NET_F_MAX_CHAIN_SIZE))

typedef enum {
	VNET_RX,
	VNET_TX,
} virtq_type_t;

/**
 * When mergeable buffers are not negotiated, the virtio_net_hdr_padded struct
 * below is placed at the beginning of the netbuf data. Use 4 bytes of pad to
 * both keep the VirtIO header and the data non-contiguous and to keep the
 * frame's payload 4 byte aligned.
 */
struct virtio_net_hdr_padded {
	struct virtio_net_hdr vhdr;
	char            vrh_pad[VTNET_RX_HEADER_PAD];
};

/**
 * @internal structure to represent the transmit queue.
 */
struct uk_netdev_tx_queue {
	/* The virtqueue reference */
	struct virtqueue *vq;
	/* The hw queue identifier */
	uint16_t hwvq_id;
	/* The user queue identifier */
	uint16_t lqueue_id;
	/* The nr. of descriptor limit */
	uint16_t max_nb_desc;
	/* The nr. of descriptor user configured */
	uint16_t nb_desc;
	/* The flag to interrupt on the transmit queue */
	uint8_t intr_enabled;
	/* Reference to the uk_netdev */
	struct uk_netdev *ndev;
	/* The scatter list and its associated fragements */
	struct uk_sglist sg;
	struct uk_sglist_seg sgsegs[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	struct uk_netbuf *buf[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	uint16_t readbuf_cnt[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	uint16_t writebuf_cnt[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	struct uk_netbuf *pkt[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	__u32 len[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];

#ifdef CONFIG_TXQ_STATS
	struct uk_netdev_txq_stats txq_stats;
#endif /* CONFIG_TXQ_STATS */
};

/**
 * @internal structure to represent the receive queue.
 */
struct uk_netdev_rx_queue {
	/* The virtqueue reference */
	struct virtqueue *vq;
	/* The virtqueue hw identifier */
	uint16_t hwvq_id;
	/* The libuknet queue identifier */
	uint16_t lqueue_id;
	/* The nr. of descriptor limit */
	uint16_t max_nb_desc;
	/* The nr. of descriptor user configured */
	uint16_t nb_desc;
	/* The flag to interrupt on the transmit queue */
	uint8_t intr_enabled;
	/* User-provided receive buffer allocation function */
	uk_netdev_alloc_rxpkts alloc_rxpkts;
	void *alloc_rxpkts_argp;
	/* Reference to the uk_netdev */
	struct uk_netdev *ndev;
	/* The scatter list and its associated fragements */
	struct uk_sglist sg;
	struct uk_sglist_seg sgsegs[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	struct uk_netbuf *buf[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	uint16_t readbuf_cnt[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	uint16_t writebuf_cnt[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];
	uint32_t len[CONFIG_LIBUKNETDEV_MAX_PKT_BURST];

#ifdef CONFIG_RXQ_STATS
	struct uk_netdev_rxq_stats rxq_stats;
#endif /* CONFIG_RXQ_STATS */
};

struct virtio_net_device {
	/* Virtio Device */
	struct virtio_dev *vdev;
	/* List of all the virtqueue in the pci device */
	struct virtqueue *vq;
	struct uk_netdev netdev;
	/* Count of the number of the virtqueues */
	__u16 max_vqueue_pairs;
	/* List of the Rx/Tx queue */
	__u16    rx_vqueue_cnt;
	struct   uk_netdev_rx_queue *rxqs;
	__u16    tx_vqueue_cnt;
	struct   uk_netdev_tx_queue *txqs;
	/* The netdevice identifier */
	__u16 uid;
	/* The max mtu */
	__u16 max_mtu;
	/* The mtu */
	__u16 mtu;
	/* The hw address of the netdevice */
	struct uk_hwaddr hw_addr;
	/*  Netdev state */
	__u8 state;
	/* Any Layout */
	__u8 any_layout;
	/* RX promiscuous mode. */
	__u8 promisc : 1;
};

/**
 * Static function declarations.
 */
static int virtio_net_drv_init(struct uk_alloc *drv_allocator);
static int virtio_net_add_dev(struct virtio_dev *vdev);
static void virtio_net_info_get(struct uk_netdev *dev,
				struct uk_netdev_info *dev_info);
static inline void virtio_netdev_feature_set(struct virtio_net_device *vndev);
static int virtio_netdev_configure(struct uk_netdev *n,
				   const struct uk_netdev_conf *conf);
static int virtio_netdev_rxtx_alloc(struct virtio_net_device *vndev,
				    const struct uk_netdev_conf *conf);
static int virtio_netdev_feature_negotiate(struct virtio_net_device *vndev);
static struct uk_netdev_tx_queue *virtio_netdev_tx_queue_setup(
					struct uk_netdev *n, uint16_t queue_id,
					uint16_t nb_desc,
					struct uk_netdev_txqueue_conf *conf);
static int virtio_netdev_vqueue_setup(struct virtio_net_device *vndev,
				      uint16_t queue_id, uint16_t nr_desc,
				      virtq_type_t queue_type,
				      struct uk_alloc *a);
static struct uk_netdev_rx_queue *virtio_netdev_rx_queue_setup(
					struct uk_netdev *n,
					uint16_t queue_id, uint16_t nb_desc,
					struct uk_netdev_rxqueue_conf *conf);
static int virtio_net_rx_intr_disable(struct uk_netdev *n,
				      struct uk_netdev_rx_queue *queue);
static int virtio_net_rx_intr_enable(struct uk_netdev *n,
				     struct uk_netdev_rx_queue *queue);
static int virtio_netdev_xmit_free(struct uk_netdev_tx_queue *txq);
static int virtio_netdev_xmit(struct uk_netdev *dev,
			      struct uk_netdev_tx_queue *queue,
			      struct uk_netbuf **pkt, __u16 *cnt);
static int virtio_netdev_recv(struct uk_netdev *dev,
			      struct uk_netdev_rx_queue *queue,
			      struct uk_netbuf **pkt, __u16 *cnt);
static const struct uk_hwaddr *virtio_net_mac_get(struct uk_netdev *n);
static __u16 virtio_net_mtu_get(struct uk_netdev *n);
static unsigned virtio_net_promisc_get(struct uk_netdev *n);
static int virtio_netdev_rxq_info_get(struct uk_netdev *dev, __u16 queue_id,
				      struct uk_netdev_queue_info *qinfo);
static int virtio_netdev_txq_info_get(struct uk_netdev *dev, __u16 queue_id,
				      struct uk_netdev_queue_info *qinfo);
static int virtio_netdev_rxq_dequeue(struct uk_netdev_rx_queue *rxq,
				     struct uk_netbuf **netbuf, int *cnt,
				     int16_t header_sz);
static int virtio_netdev_rxq_init(struct uk_netdev_rx_queue *rxq,
				  struct uk_netbuf *netbuf, __u8 any_layout);
static int virtio_netdev_recv_done(struct virtqueue *vq, void *priv);
static int virtio_netdev_rx_fillup(struct uk_netdev_rx_queue *rxq,
				   __u16 num, int notify, __u8 any_layout,
				   int16_t header_sz);

/**
 * Static global constants
 */
static const char *drv_name = DRIVER_NAME;
static struct uk_alloc *a;

/**
 * The Driver method implementation.
 */
static int virtio_netdev_recv_done(struct virtqueue *vq, void *priv)
{
	struct uk_netdev_rx_queue *rxq = NULL;

	UK_ASSERT(vq && priv);

	rxq = (struct uk_netdev_rx_queue *) priv;

	/* Disable the interrupt for the ring */
	virtqueue_intr_disable(vq);
	rxq->intr_enabled &= ~(VTNET_INTR_EN);

	/* Indicate to the network stack about an event */
	uk_netdev_drv_rx_event(rxq->ndev, rxq->lqueue_id);
	return 1;
}

#define TX_QUEUE_EMPTY CONFIG_LIBUKNETDEV_MAX_PKT_BURST

static int virtio_netdev_xmit_free(struct uk_netdev_tx_queue *txq)
{
	int cnt = TX_QUEUE_EMPTY;
	struct uk_netbuf *pkt[TX_QUEUE_EMPTY];
	__u32 len[TX_QUEUE_EMPTY];
	int rc;
	int i;
	struct virtio_net_hdr *hdr;
	int16_t header_sz = sizeof(*hdr);

	for (;;) {
		rc = virtqueue_buffer_dequeue_burst(txq->vq, (void **)pkt, &cnt, len);
		if (cnt == 0) {
			break;
		}

		uk_pr_debug("free %d desc %d\n", rc, cnt);

		for (i = 0; i < cnt; i++) {
			UK_ASSERT(pkt[i]);
			/**
			 * Releasing the free buffer back to netbuf. The netbuf
			 * could use the destructor to inform the stack
			 * regarding the free up of memory.
			 */
			//uk_netbuf_free_single(pkt[i]);
			pkt[i]->data = pkt[i]->buf + 128;
			pkt[i]->len  -= header_sz;
			//uk_netbuf_free_single(pkt[i]);
			//pkt[i]->dtor(pkt[i]);
		}
		uk_netbuf_pool_dtor(pkt, cnt);

		if (cnt < TX_QUEUE_EMPTY)
			break;
		cnt = TX_QUEUE_EMPTY;
	}
	uk_pr_debug("Free %"__PRIu16" descriptors\n", txq->nb_desc - rc);
	return txq->nb_desc - rc;
}

#define RX_FILLUP_BATCHLEN CONFIG_LIBUKNETDEV_MAX_PKT_BURST

static int virtio_netdev_rx_fillup(struct uk_netdev_rx_queue *rxq,
				   __u16 nb_desc,
				   int notify, __u8 any_layout,
				   int16_t header_sz)
{
	struct uk_netbuf *netbuf[RX_FILLUP_BATCHLEN];
	int rc = 0;
	int status = 0x0;
	__u16 i, j;
	__u16 req;
	__u16 cnt = 0;
	__u16 filled = 0;
	__u16 rqst_cnt = 0;

	/**
	 * Fixed amount of memory is allocated to each received buffer. In
	 * our case since we don't support jumbo frame or LRO yet we require
	 * that the buffer feed to the ring descriptor is atleast
	 * ethernet MTU + virtio net header.
	 * Because we using 2 descriptor for a single netbuf, our effective
	 * queue size is just the half.
	 */
	rqst_cnt = (any_layout) ? nb_desc : (nb_desc >> 1);
	while (filled < rqst_cnt) {
		//req = MIN(rqst_cnt, RX_FILLUP_BATCHLEN);
		req = rqst_cnt;
		cnt = rxq->alloc_rxpkts(rxq->alloc_rxpkts_argp, netbuf, req);
		if (cnt == 0) {
			status |= UK_NETDEV_STATUS_UNDERRUN;
			goto out;
		}

#ifdef CONFIG_USE_SG
		uk_sglist_reset(&rxq->sg);
#endif /* CONFIG_USE_SG */
		for (i = 0; i < cnt; i++) {

			rc = virtio_netdev_rxq_init(rxq, netbuf[i], any_layout);
			if (unlikely(rc < 0)) {
				uk_pr_err("Failed to add a buffer to receive virtqueue %p: %d\n",
					  rxq, rc);

				break;
			}
#ifdef CONFIG_USE_SG
			rxq->readbuf_cnt[i] = 0;
			rxq->writebuf_cnt[i] = rxq->sg.sg_nseg;
#else
			rxq->readbuf_cnt[i] = 0;
			rxq->writebuf_cnt[i] = rc;
#endif
			uk_pr_debug("Enqueue netbuf %"PRIu16"/%"PRIu16" (%p) to virtqueue %p write_cnt: %d...\n",
				    i + 1, cnt, netbuf[i], rxq,
				    rxq->writebuf_cnt[i]);
		}
#ifdef CONFIG_USE_SG
		/**
		 * Burst fill the buffer.
		 */
		rc = virtqueue_buffer_enqueue_burst(rxq->vq, (void **)netbuf, &rxq->sg,
						    &i, rxq->readbuf_cnt,
						    rxq->writebuf_cnt);
#else
		rc = virtnet_buffer_enqueue_burst(rxq->vq, (void **)netbuf,
						    &i, rxq->readbuf_cnt,
						    rxq->writebuf_cnt);

#endif
		filled += i;
		if (rc > 0 && cnt < req) {
			uk_pr_debug("Incomplete fill-up of netbufs on receive virtqueue %p: Out of memory cnt: %d, request: %d\n", rxq, cnt, req);
			status |= UK_NETDEV_STATUS_UNDERRUN;
			goto out;

		} else if (rc == -ENOSPC) {
			/*
			 * Release netbufs that we are not going
			 * to use anymore
			 */
			uk_pr_debug("%s: unable to fill: %d desc\n", __func__, cnt - i);
			for (j = i; j < cnt; j++) {
				uk_netbuf_header(netbuf[j], -header_sz);
				uk_netbuf_free(netbuf[j]);
			}
			goto out;
		}
	}

out:
	uk_pr_debug("Programmed %"PRIu16" receive netbufs to receive virtqueue %p (status %x)\n",
		    filled, rxq, status);

	/**
	 * Notify the host, when we submit new descriptor(s).
	 */
	if (notify && filled)
		virtqueue_host_notify(rxq->vq);

	return status;
}

static inline int _virtio_netdev_xmit_inorder(struct uk_netbuf *nb,
					      struct uk_sglist *sg)
{
	struct virtio_net_hdr_padded *padded_hdr;
	int16_t header_sz = sizeof(*padded_hdr);
	struct virtio_net_hdr *vhdr;
	size_t total_len = 0;
	int rc;
	__u8  *buf_start;
	size_t buf_len;

	total_len = 0;
	buf_start = nb->data;
	buf_len = nb->len;
	/**
	 * Use the preallocated header space for the virtio header.
	 */
	rc = uk_netbuf_header(nb, header_sz);
	if (unlikely(rc != 1)) {
		uk_pr_err("Failed to prepend virtio header\n");
		return rc;
	}

	vhdr = nb->data;

	/**
	 * Fill the virtio-net-header with the necessary information.
	 * Zero explicitly set.
	 */
	memset(vhdr, 0, sizeof(*vhdr));
	vhdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;
	/**
	 * According the specification 5.1.6.6, we need to explicitly use
	 * 2 descriptor for each transmit and receive network packet since we
	 * do not negotiate for the VIRTIO_F_ANY_LAYOUT.
	 *
	 * 1 for the virtio header and the other for the actual network packet.
	 */
	/* Appending the data to the list. */
	rc = uk_sglist_append(sg, vhdr, sizeof(*vhdr));
	if (unlikely(rc != 0)) {
		uk_pr_err("Failed to append to the sg list\n");
		goto remove_vhdr;
	}
	total_len += sizeof(*vhdr);
	rc = uk_sglist_append(sg, buf_start, buf_len);
	if (unlikely(rc != 0)) {
		uk_pr_err("Failed to append to the sg list\n");
		goto remove_vhdr;
	}
	total_len += buf_len;
	if (nb->next) {
		rc = uk_sglist_append_netbuf(sg, nb->next);
		if (unlikely(rc <= 0)) {
			uk_pr_err("Failed to append to the sg list\n");
			goto remove_vhdr;
		}
		total_len += rc;
	}
	if (unlikely(total_len > VIRTIO_PKT_BUFFER_LEN)) {
		uk_pr_err("Packet size too big: %lu, max:%u\n",
				total_len, VIRTIO_PKT_BUFFER_LEN);
		rc = -ENOTSUP;
		goto remove_vhdr;
	}

	return 0;

remove_vhdr:
	uk_netbuf_header(nb, -header_sz);
	return rc;
}

static inline int _virtio_netdev_xmit_anylayout(struct uk_netbuf *nb,
						struct uk_sglist *sg)
{
	struct virtio_net_hdr *vhdr;
#ifdef CONFIG_USE_SG
	uint16_t total_len;
#endif
	uint16_t header_sz = sizeof(*vhdr);
	int rc;

#ifdef CONFIG_USE_SG
	total_len = 0;
#endif

	uk_pr_debug("Pkt size: %u\n", nb->len);

	/**
	 * Use the preallocated header space for the virtio header.
	 */
#ifdef CONFIG_USE_NETBUF_HDR
	rc = uk_netbuf_header(nb, header_sz);
	if (unlikely(rc != 1)) {
		uk_pr_err("Failed to prepend virtio header\n");
		return rc;
	}
#else
	nb->data   = nb->buf + (128 - header_sz);
	nb->len    += header_sz;

#endif

#ifdef CONFIG_USE_SG
	uk_pr_debug("Pkt size: %u\n", nb->len);
	rc = uk_sglist_append_netbuf(sg, nb);
	if (unlikely(rc <= 0)) {
		uk_pr_err("Failed to append to the sg list\n");
		goto remove_vhdr;
	}
	total_len += rc;
	if (unlikely(total_len > VIRTIO_PKT_BUFFER_LEN)) {
		uk_pr_err("Packet size too big: %u, max:%u\n",
				total_len, VIRTIO_PKT_BUFFER_LEN);
		rc = -ENOTSUP;
		goto remove_vhdr;
	}
	uk_pr_debug("Total_len: %d\n", total_len);
#endif

	return 0;

#ifdef CONFIG_USE_SG
remove_vhdr:
	uk_netbuf_header(nb, -header_sz);
	return rc;
#endif /* CONFIG_USE_SG */
}

static int virtio_netdev_xmit(struct uk_netdev *dev,
			      struct uk_netdev_tx_queue *queue,
			      struct uk_netbuf **pkt, __u16 *cnt)
{
	struct virtio_net_device *vndev;
	int rc = 0;
	int status = 0x0;
	int16_t header_sz;
	int desc_cnt;
	__u16 i, count;
#ifdef CONFIG_TXQ_STATS
	__u64 start_tsc, end_tsc;
#endif /* CONFIG_TXQ_STATS */


	UK_ASSERT(dev);
	UK_ASSERT(pkt && queue && cnt);

	vndev = to_virtionetdev(dev);
	header_sz = vndev->any_layout ? sizeof(struct virtio_net_hdr) :
					sizeof(struct virtio_net_hdr_padded);

	count = *cnt;
#ifdef CONFIG_TXQ_STATS
	start_time = rdtsc();
#endif /* CONFIG_TXQ_STATS */
	/**
	 * We are reclaiming the free descriptors from buffers. The function is
	 * not protected by means of locks. We need to be careful if there are
	 * multiple context through which we free the tx descriptors.
	 */
	desc_cnt = virtio_netdev_xmit_free(queue);
#ifdef CONFIG_TXQ_STATS
	end_time = rdtsc();
	queue->txq_stats.tx_free_latency += (end_time - start_time);
#endif /* CONFIG_TXQ_STATS */

	if (count == 0 || desc_cnt == 0) {
#ifdef CONFIG_TXQ_STATS
		queue->txq_stats.tx_dropped += *cnt;
#endif /* CONFIG_TXQ_STATS */
		*cnt = 0;
		return UK_NETDEV_STATUS_UNDERRUN;
	}

#ifdef CONFIG_TXQ_STATS
	start_time = rdtsc();
#endif /* CONFIG_TXQ_STATS */
	/**
	 * Prepare the sglist and enqueue the buffer to the virtio-ring.
	 */
#ifdef CONFIG_USE_SG
	uk_sglist_reset(&queue->sg);
#else
	int t = (vndev->any_layout) ? 1 : 2;
#endif /* CONFIG_USE_SG */

	for (i = 0; i < count && i < desc_cnt; i++) {
		//if (vndev->any_layout) {
			rc = _virtio_netdev_xmit_anylayout(pkt[i], &queue->sg);
		//} else {
		//	rc = _virtio_netdev_xmit_inorder(pkt[i], &queue->sg);
		//}

		if (unlikely(rc < 0))
			break;

#ifdef CONFIG_USE_SG
		queue->readbuf_cnt[i] = queue->sg.sg_nseg;
		queue->writebuf_cnt[i] = 0;
#else
		queue->readbuf_cnt[i] = t;
		queue->writebuf_cnt[i] = 0;
#endif
	}

	if (i > 0 && i < count)
		status = UK_NETDEV_STATUS_UNDERRUN;
	else if (i == 0) {
		status = rc;
		*cnt = 0;
		return status;
	}
	count = i;
#ifdef CONFIG_TXQ_STATS
	end_time = rdtsc();
	queue->txq_stats.tx_prepare_latency += (end_time - start_time);
#endif /* CONFIG_TXQ_STATS */
	/**
	 * Adding the descriptors to the virtqueue.
	 */
#ifdef __Unikraft__
#ifdef CONFIG_TXQ_STATS
	start_time = rdtsc();
#endif /* CONFIG_TXQ_STATS */


#ifdef CONFIG_USE_SG
	rc = virtqueue_buffer_enqueue_burst(queue->vq, (void **)pkt, &queue->sg,
			&i, queue->readbuf_cnt, queue->writebuf_cnt);
#else
	rc = virtnet_buffer_enqueue_burst(queue->vq, pkt,
			&i, queue->readbuf_cnt, queue->writebuf_cnt);
#endif /* !CONFIG_USE_SG */

#else /* !__Unikraft__ */
	rc = -ENOSPC;
	i = 0;
#endif /* !__Unikraft__ */
	if (rc > 0) {
		/**
		 * Notify the host the new buffer.
		 */
		virtqueue_host_notify(queue->vq);
		/**
		 * When there is further space available in the ring
		 * return UK_NETDEV_STATUS_MORE.
		 */
		status |= UK_NETDEV_STATUS_SUCCESS | UK_NETDEV_STATUS_MORE;
#ifdef CONFIG_TXQ_STATS
		end_time = rdtsc();
		queue->txq_stats.tx_ring_latency += (end_time - start_time);
#endif /* CONFIG_TXQ_STATS */
		return status;
	} else if (rc == 0) {
		status |= UK_NETDEV_STATUS_SUCCESS;
		/**
		 * Notify the host the new buffer.
		 */
		virtqueue_host_notify(queue->vq);
#ifdef CONFIG_TXQ_STATS
		end_time = rdtsc();
		queue->txq_stats.tx_ring_latency += (end_time - start_time);
#endif /* CONFIG_TXQ_STATS */
		return status;
	} else if (rc == -ENOSPC && i > 0) {
		/**
		 * Remove header before exiting because we could not send
		 */
		*cnt = i;
		status |= UK_NETDEV_STATUS_UNDERRUN | UK_NETDEV_STATUS_SUCCESS;
		virtqueue_host_notify(queue->vq);
#ifdef CONFIG_TXQ_STATS
		end_time = rdtsc();
		queue->txq_stats.tx_ring_latency += (end_time - start_time);
#endif /* CONFIG_TXQ_STATS */
	} else if (rc == -ENOSPC) {
		*cnt = 0;
		status = UK_NETDEV_STATUS_UNDERRUN;
#ifdef CONFIG_TXQ_STATS
		end_time = rdtsc();
		queue->txq_stats.tx_ring_latency += (end_time - start_time);
#endif /* CONFIG_TXQ_STATS */
	} else {
		uk_pr_err("Failed to enqueue descriptors into the ring: %d\n",
				rc);
		*cnt = i;
		status = rc;
	}
	printf("free up %d desc\n", count - i);
	for (;i < count; i++)
		uk_netbuf_header(pkt[i], -header_sz);
err_exit:
	return status;
}

static int _virtio_netdev_rxq_init_inorder(struct uk_netbuf *nb,
					  struct uk_sglist *sg)
{
	__u8 *buf_start;
	size_t buf_len = 0;
	struct virtio_net_hdr_padded *rxhdr;
	int16_t header_sz = sizeof(*rxhdr);
	int rc;

	/**
	 * Saving the buffer information before reserving the header space.
	 */
	buf_start = nb->data;
	buf_len = nb->len;

	/**
	 * Retrieve the buffer header length.
	 */
	rc = uk_netbuf_header(nb, header_sz);
	if (unlikely(rc != 1)) {
		uk_pr_err("Failed to allocate space to prepend virtio header\n");
		return -EINVAL;
	}

	/* Appending the header buffer to the sglist */
	rc = uk_sglist_append(sg, nb->data, sizeof(struct virtio_net_hdr));
	if (rc < 0) {
		uk_pr_err("Failed(%d) to append to virtio header into rxq\n", rc);
		goto error_remove_hdr;
	}

	/* Appending the data buffer to the sglist */
	rc = uk_sglist_append(sg, buf_start, buf_len);
	if (rc < 0) {
		uk_pr_err("Failed(%d) to append empty buffer to rxq\n", rc);
		goto error_remove_sglist;
	}

	return 0;

error_remove_sglist:
	sg->sg_nseg--;
error_remove_hdr:
	uk_netbuf_header(nb, -header_sz);
	return rc;
}

static inline int _virtio_netdev_rxq_init_anylayout(struct uk_netbuf *nb,
					     struct uk_sglist *sg)
{
	int rc;
	struct virtio_net_hdr *hdr;
	int16_t header_sz = sizeof(*hdr);

#ifdef CONFIG_USE_NETBUF_HDR
	rc = uk_netbuf_header(nb, header_sz);
	if (unlikely(rc != 1)) {
		uk_pr_err("Failed to allocate space to prepend virtio header\n");
		return -EINVAL;
	}
#else
	nb->data   = nb->buf + (128 - header_sz);
	nb->len    += header_sz;
#endif

#ifdef CONFIG_USE_SG
	rc = uk_sglist_append(sg, nb->data, nb->len);
	if (rc < 0) {
		uk_pr_err("Failed(%d) to append empty buffer to rxq\n", rc);
		goto error_remove_sglist;
	}
#endif /* CONFIG_USE_SG */

	return 0;

#ifdef CONFIG_USE_SG
error_remove_sglist:
	sg->sg_nseg--;
	uk_netbuf_header(nb, -header_sz);
	return rc;
#endif
}

static inline int virtio_netdev_rxq_init(struct uk_netdev_rx_queue *rxq,
				  struct uk_netbuf *netbuf, __u8 any_layout)
{
	int rc = 0;

	if (any_layout) {
		rc =_virtio_netdev_rxq_init_anylayout(netbuf, &rxq->sg);
#ifndef CONFIG_USE_SG
#ifdef CONFIG_USE_NETBUF_HDR
		if (unlikely(rc < 0))
			return rc;
#endif /* CONFIG_USE_NETBUF_HDR */
		rc = 1;
#endif
	}
	else {
		rc = _virtio_netdev_rxq_init_inorder(netbuf, &rxq->sg);
#ifndef CONFIG_USE_SG
		if (unlikely(rc < 0))
			return rc;
		rc = 2;
#endif
	}

	return rc;
}

static int _virtio_pkt_hdr_process(struct uk_netbuf *buf, __u32 len,
				   int16_t header_sz)
{
	int rc;

	UK_ASSERT(buf);

	if (unlikely((len < VIRTIO_HDR_LEN + UK_ETH_HDR_UNTAGGED_LEN)
		     || (len > VIRTIO_PKT_BUFFER_LEN))) {
		uk_pr_err("Received invalid packet size: %"__PRIu32"\n", len);
		return -EINVAL;
	}
	uk_pr_debug("Received packet size: %"__PRIu32"\n", len);

	/**
	 * Removing the virtio header from the buffer and adjusting length.
	 * We pad "VTNET_RX_HEADER_PAD" to the rx buffer while enqueuing for
	 * alignment of the packet data. We compensate for this, by adding the
	 *  padding to the length on dequeue.
	 */
	//buf->len = len;// + VTNET_RX_HEADER_PAD;
#ifdef CONFIG_USE_NETBUF_HDR
	rc = uk_netbuf_header(buf, -header_sz);
	UK_ASSERT(rc == 1);
#else
	buf->data += header_sz;
	buf->len  = (len - header_sz);
#endif
	return 0;
}

static int virtio_netdev_rxq_dequeue(struct uk_netdev_rx_queue *rxq,
				     struct uk_netbuf **netbuf, int *cnt,
				     int16_t header_sz)
{
	int ret;
	int rc = 0, i, j, bad_pkts = 0;

	UK_ASSERT(netbuf);

	ret = virtqueue_buffer_dequeue_burst(rxq->vq, (void **) netbuf, cnt,
					     &rxq->len[0]);
	if (*cnt == 0) {
		return ret;
	}

	for (i = 0, j = 0; i < *cnt; i++) {
		rc = _virtio_pkt_hdr_process(netbuf[i], rxq->len[i], header_sz);
		if (unlikely(rc < 0)) {
			printf("received bad packet\n");
			uk_netbuf_free(netbuf[i]);
			bad_pkts++;
			continue;
		}
		netbuf[j] = netbuf[i];
		j++;
	}
	*cnt = *cnt - bad_pkts;

	return ret;
}

static int virtio_netdev_recv(struct uk_netdev *dev,
			      struct uk_netdev_rx_queue *queue,
			      struct uk_netbuf **pkt, __u16 *cnt)
{
	struct virtio_net_device *vndev;
	int status = 0x0;
	int rc = 0;
	int buf_cnt;
	int16_t header_sz;

	UK_ASSERT(dev && queue);
	UK_ASSERT(pkt && cnt);

	buf_cnt = *cnt;
	vndev = to_virtionetdev(dev);
	header_sz = vndev->any_layout ? sizeof(struct virtio_net_hdr) :
					sizeof(struct virtio_net_hdr_padded);

	/* Queue interrupts have to be off when calling receive */
	UK_ASSERT(!(queue->intr_enabled & VTNET_INTR_EN));

	rc = virtio_netdev_rxq_dequeue(queue, pkt, &buf_cnt, header_sz);
	if (unlikely(buf_cnt == 0)) {
		*cnt = buf_cnt;
		return status;
	} else {
		status = UK_NETDEV_STATUS_SUCCESS;
		status |= virtio_netdev_rx_fillup(queue, (queue->nb_desc - rc),
						  1, vndev->any_layout,
						  header_sz);
	}

	uk_pr_debug("%s: received %d pkts\n", __func__, buf_cnt);

	/* Enable interrupt only when user had previously enabled it */
	if (queue->intr_enabled & VTNET_INTR_USR_EN_MASK) {
		/* Need to enable the interrupt on the last packet */
		rc = virtqueue_intr_enable(queue->vq);
		if (rc == 1 && (buf_cnt < *cnt)) {
			int tmp_cnt = *cnt - buf_cnt;
			/**
			 * Packet arrive after reading the queue and before
			 * enabling the interrupt
			 */
			rc = virtio_netdev_rxq_dequeue(queue, &pkt[buf_cnt],
						       &tmp_cnt, header_sz);
			if (unlikely(rc < 0)) {
				uk_pr_err("Failed to dequeue the packet: %d\n",
					  rc);
				goto err_exit;
			}
			status |= UK_NETDEV_STATUS_SUCCESS;
			buf_cnt += tmp_cnt;

			/*
			 * Since we received something, we need to fillup
			 * and notify
			 */
			status |= virtio_netdev_rx_fillup(queue,
							  (queue->nb_desc - rc),
							  1, vndev->any_layout,
							  header_sz);

			/* Need to enable the interrupt on the last packet */
			rc = virtqueue_intr_enable(queue->vq);
			status |= (rc == 1) ? UK_NETDEV_STATUS_MORE : 0x0;
		} else if (buf_cnt == *cnt) {
			/* When we originally got a packet and there is more */
			status |= (rc == 1) ? UK_NETDEV_STATUS_MORE : 0x0;
		}
	} else if (buf_cnt == *cnt) {
		/**
		 * For polling case, we report always there are further
		 * packets unless the queue is empty.
		 */
		status |= UK_NETDEV_STATUS_MORE;
	}
	*cnt = buf_cnt;
	return status;

err_exit:
	UK_ASSERT(rc < 0);
	return rc;
}

static struct uk_netdev_rx_queue *virtio_netdev_rx_queue_setup(
				struct uk_netdev *n, uint16_t queue_id,
				uint16_t nb_desc,
				struct uk_netdev_rxqueue_conf *conf)
{
	struct virtio_net_device *vndev;
	struct uk_netdev_rx_queue *rxq = NULL;
	int rc;
	int16_t header_sz;

	UK_ASSERT(n);
	UK_ASSERT(conf);
	UK_ASSERT(conf->alloc_rxpkts);

	vndev = to_virtionetdev(n);
	if (queue_id >= vndev->max_vqueue_pairs) {
		uk_pr_err("Invalid virtqueue identifier: %"__PRIu16"\n",
			  queue_id);
		rc = -EINVAL;
		goto err_exit;
	}
	/* Setup the virtqueue with the descriptor */
	rc = virtio_netdev_vqueue_setup(vndev, queue_id, nb_desc, VNET_RX,
					conf->a);
	if (rc < 0) {
		uk_pr_err("Failed to set up virtqueue %"__PRIu16": %d\n",
			  queue_id, rc);
		goto err_exit;
	}
	rxq  = &vndev->rxqs[rc];
	rxq->alloc_rxpkts = conf->alloc_rxpkts;
	rxq->alloc_rxpkts_argp = conf->alloc_rxpkts_argp;

	header_sz = vndev->any_layout ? sizeof(struct virtio_net_hdr) :
					sizeof(struct virtio_net_hdr_padded);

	/* Allocate receive buffers for this queue */
	virtio_netdev_rx_fillup(rxq, rxq->nb_desc, 0, vndev->any_layout,
				header_sz);

exit:
	return rxq;

err_exit:
	rxq = ERR2PTR(rc);
	goto exit;
}

/**
 * This function setup the vring infrastructure.
 * @param vndev
 *	Reference to the virtio net device.
 * @param queue_id
 *	User queue identifier
 * @param nr_desc
 *	User configured number of descriptors.
 * @param queue_type
 *	Queue type.
 * @param a
 *	Reference to the allocator.
 */
static int virtio_netdev_vqueue_setup(struct virtio_net_device *vndev,
		uint16_t queue_id, uint16_t nr_desc, virtq_type_t queue_type,
		struct uk_alloc *a)
{
	int rc = 0;
	int id = 0;
	virtqueue_callback_t callback;
	uint16_t max_desc, hwvq_id;
	struct virtqueue *vq;

	if (queue_type == VNET_RX) {
		id = vndev->rx_vqueue_cnt;
		callback = virtio_netdev_recv_done;
		max_desc = vndev->rxqs[id].max_nb_desc;
		hwvq_id = vndev->rxqs[id].hwvq_id;
	} else {
		id = vndev->tx_vqueue_cnt;
		/* We don't support the callback from the txqueue yet */
		callback = NULL;
		max_desc = vndev->txqs[id].max_nb_desc;
		hwvq_id = vndev->txqs[id].hwvq_id;
	}

	if (unlikely(max_desc < nr_desc)) {
		uk_pr_err("Max allowed desc: %"__PRIu16" Requested desc:%"__PRIu16"\n",
			  max_desc, nr_desc);
		return -ENOBUFS;
	}

#if 0
	nr_desc = (nr_desc != 0) ? nr_desc : max_desc;
#endif
	nr_desc = max_desc;
	printf("Configuring the %d descriptors on queue: %d\n", nr_desc, hwvq_id);

	/* Check if the descriptor is a power of 2 */
	if (unlikely(nr_desc & (nr_desc - 1))) {
		uk_pr_err("Expect descriptor count as a power 2\n");
		return -EINVAL;
	}
	vq = virtio_vqueue_setup(vndev->vdev, hwvq_id, nr_desc, callback, a);
	if (unlikely(PTRISERR(vq))) {
		uk_pr_err("Failed to set up virtqueue %"__PRIu16"\n",
			  queue_id);
		rc = PTR2ERR(vq);
		return rc;
	}

	if (queue_type == VNET_RX) {
		vq->priv = &vndev->rxqs[id];
		vndev->rxqs[id].ndev = &vndev->netdev;
		vndev->rxqs[id].vq = vq;
		vndev->rxqs[id].nb_desc = nr_desc;
		vndev->rxqs[id].lqueue_id = queue_id;
		vndev->rx_vqueue_cnt++;
	} else {
		vndev->txqs[id].vq = vq;
		vndev->txqs[id].ndev = &vndev->netdev;
		vndev->txqs[id].nb_desc = nr_desc;
		vndev->txqs[id].lqueue_id = queue_id;
		vndev->tx_vqueue_cnt++;
	}
	return id;
}

static struct uk_netdev_tx_queue *virtio_netdev_tx_queue_setup(
				struct uk_netdev *n, uint16_t queue_id __unused,
				uint16_t nb_desc __unused,
				struct uk_netdev_txqueue_conf *conf __unused)
{
	struct uk_netdev_tx_queue *txq = NULL;
	struct virtio_net_device *vndev;
	int rc = 0;

	UK_ASSERT(n);
	vndev = to_virtionetdev(n);
	if (queue_id >= vndev->max_vqueue_pairs) {
		uk_pr_err("Invalid virtqueue identifier: %"__PRIu16"\n",
			  queue_id);
		rc = -EINVAL;
		goto err_exit;
	}
	/* Setup the virtqueue */
	rc = virtio_netdev_vqueue_setup(vndev, queue_id, nb_desc, VNET_TX,
					conf->a);
	if (rc < 0) {
		uk_pr_err("Failed to set up virtqueue %"__PRIu16": %d\n",
			  queue_id, rc);
		goto err_exit;
	}
	txq = &vndev->txqs[rc];
exit:
	return txq;

err_exit:
	txq = ERR2PTR(rc);
	goto exit;
}

static int virtio_netdev_rxq_info_get(struct uk_netdev *dev,
				      __u16 queue_id,
				      struct uk_netdev_queue_info *qinfo)
{
	struct virtio_net_device *vndev;
	struct uk_netdev_rx_queue *rxq;
	int rc = 0;

	UK_ASSERT(dev);
	UK_ASSERT(qinfo);
	vndev = to_virtionetdev(dev);
	if (unlikely(queue_id >= vndev->max_vqueue_pairs)) {
		uk_pr_err("Invalid virtqueue id: %"__PRIu16"\n", queue_id);
		rc = -EINVAL;
		goto exit;
	}
	rxq = &vndev->rxqs[queue_id];
	qinfo->nb_min = 1;
	qinfo->nb_max = rxq->max_nb_desc;
	qinfo->nb_is_power_of_two = 1;

exit:
	return rc;

}

static int virtio_netdev_txq_info_get(struct uk_netdev *dev,
				      __u16 queue_id __unused,
				      struct uk_netdev_queue_info *qinfo)
{
	struct virtio_net_device *vndev;
	struct uk_netdev_tx_queue *txq;
	int rc = 0;

	UK_ASSERT(dev);
	UK_ASSERT(qinfo);

	vndev = to_virtionetdev(dev);
	if (unlikely(queue_id >= vndev->max_vqueue_pairs)) {
		uk_pr_err("Invalid queue_id %"__PRIu16"\n", queue_id);
		rc = -EINVAL;
		goto exit;
	}
	txq = &vndev->txqs[queue_id];
	qinfo->nb_min = 1;
	qinfo->nb_max = txq->max_nb_desc;
	qinfo->nb_is_power_of_two = 1;

exit:
	return rc;
}

static unsigned virtio_net_promisc_get(struct uk_netdev *n)
{
	struct virtio_net_device *d;

	UK_ASSERT(n);
	d = to_virtionetdev(n);
	return d->promisc;
}

static const struct uk_hwaddr *virtio_net_mac_get(struct uk_netdev *n)
{
	struct virtio_net_device *d;
	int i;

	UK_ASSERT(n);
	d = to_virtionetdev(n);
	for ( i = 0; i < UK_NETDEV_HWADDR_LEN; i++) {
		uk_pr_info("%x:", d->hw_addr.addr_bytes[i]);
	}
	uk_pr_info("\n");
	return &d->hw_addr;
}

static __u16 virtio_net_mtu_get(struct uk_netdev *n)
{
	struct virtio_net_device *d;

	UK_ASSERT(n);
	d = to_virtionetdev(n);
	return d->mtu;
}

static int virtio_netdev_feature_negotiate(struct virtio_net_device *vndev)
{
	__u64 host_features = 0;
	__u16 hw_len;
	int rc = 0;

	/**
	 * Read device feature bits, and write the subset of feature bits
	 * understood by the OS and driver to the device. During this step the
	 * driver MAY read (but MUST NOT write) the device-specific
	 * configuration fields to check that it can support the device before
	 * accepting it.
	 */
	host_features = virtio_feature_get(vndev->vdev);
	if (!virtio_has_features(host_features, VIRTIO_NET_F_MAC)) {
		/**
		 * The feature that aren't supported are usually masked out and
		 * provided with default value. In this case we need to
		 * report an error as we don't support  generation of random
		 * MAC Address.
		 */
		uk_pr_err("Host system does not offer MAC feature\n");
		rc = -EINVAL;
		goto exit;
	}

	/**
	 * According to Virtio specification, section 2.3.1. Config fields
	 * greater than 32-bits cannot be atomically read. We may need to
	 * reconsider providing generic read/write function for all these
	 * virtio device in a separate header file which could be reused across
	 * different virtio devices.
	 */
	hw_len = virtio_config_get(vndev->vdev,
				   __offsetof(struct virtio_net_config, mac),
				   &vndev->hw_addr.addr_bytes[0],
				   UK_NETDEV_HWADDR_LEN, 1);
	if (unlikely(hw_len != UK_NETDEV_HWADDR_LEN)) {
		uk_pr_err("Failed to retrieve the mac address from device\n");
		rc = -EAGAIN;
		goto exit;
	}
	rc = 0;

	if (virtio_has_features(host_features, VIRTIO_F_ANY_LAYOUT)) {
		vndev->any_layout = 1;
		uk_pr_info("virtqueue are configure with any layout\n");
	}

	if (virtio_has_features(host_features, VIRTIO_NET_F_MAX_CHAIN_SIZE)) {
		printf("Configuring the Virtio Net max chain size\n");
	} else {
		printf("Configuring the Virtio Net TX chain \n");
	}

	/**
	 * Mask out features supported by both driver and device.
	 */
	vndev->vdev->features &= host_features;
	virtio_feature_set(vndev->vdev, vndev->vdev->features);
exit:
	return rc;
}

static int virtio_netdev_rxtx_alloc(struct virtio_net_device *vndev,
				    const struct uk_netdev_conf *conf)
{
	int rc = 0;
	int i = 0;
	int vq_avail = 0;
	int total_vqs = conf->nb_rx_queues + conf->nb_tx_queues;
	__u16 qdesc_size[total_vqs];

	if (conf->nb_rx_queues != 1 || conf->nb_tx_queues != 1) {
		uk_pr_err("Queue combination not supported: %"__PRIu16"/%"__PRIu16" rx/tx\n",
			  conf->nb_rx_queues, conf->nb_tx_queues);

		return -ENOTSUP;
	}

	/**
	 * TODO:
	 * The virtio device management data structure are allocated using the
	 * allocator from the netdev configuration. In the future it might be
	 * wiser to move it to the allocator of each individual queue. This
	 * would better considering NUMA support.
	 */
	vndev->rxqs = uk_malloc(a, sizeof(*vndev->rxqs) * conf->nb_rx_queues);
	vndev->txqs = uk_malloc(a, sizeof(*vndev->txqs) * conf->nb_tx_queues);
	if (unlikely(!vndev->rxqs || !vndev->txqs)) {
		uk_pr_err("Failed to allocate memory for queue management\n");
		rc = -ENOMEM;
		goto err_free_txrx;
	}

	vq_avail = virtio_find_vqs(vndev->vdev, total_vqs, qdesc_size);
	if (unlikely(vq_avail != total_vqs)) {
		uk_pr_err("Expected: %d queues, Found: %d queues\n",
			  total_vqs, vq_avail);
		rc = -ENOMEM;
		goto err_free_txrx;
	}

	/**
	 * The virtqueue are organized as:
	 * Virtqueue-rx0
	 * Virtqueue-tx0
	 * Virtqueue-rx1
	 * Virtqueue-tx1
	 * ...
	 * Virtqueue-ctrlq
	 */
	uk_pr_info("Max desc: %d\n", qdesc_size[vndev->rxqs[i].hwvq_id]);
	for (i = 0; i < vndev->max_vqueue_pairs; i++) {
		/**
		 * Initialize the received queue with the information received
		 * from the device.
		 */
		vndev->rxqs[i].hwvq_id = 2 * i;
		vndev->rxqs[i].max_nb_desc = qdesc_size[vndev->rxqs[i].hwvq_id];
		uk_sglist_init(&vndev->rxqs[i].sg,
			       (sizeof(vndev->rxqs[i].sgsegs) /
				sizeof(vndev->rxqs[i].sgsegs[0])),
			       &vndev->rxqs[i].sgsegs[0]);

		/**
		 * Initialize the transmit queue with the information received
		 * from the device.
		 */
		vndev->txqs[i].hwvq_id = (2 * i) + 1;
		vndev->txqs[i].max_nb_desc = qdesc_size[vndev->txqs[i].hwvq_id];
		uk_sglist_init(&vndev->txqs[i].sg,
			       (sizeof(vndev->txqs[i].sgsegs) /
				sizeof(vndev->txqs[i].sgsegs[0])),
			       &vndev->txqs[i].sgsegs[0]);
	}
exit:
	return rc;

err_free_txrx:
	if (!vndev->rxqs)
		uk_free(a, vndev->rxqs);
	if (!vndev->txqs)
		uk_free(a, vndev->txqs);
	goto exit;
}

static int virtio_netdev_configure(struct uk_netdev *n,
				   const struct uk_netdev_conf *conf)
{
	int rc = 0;
	struct virtio_net_device *vndev;

	UK_ASSERT(n);
	UK_ASSERT(conf);
	vndev = to_virtionetdev(n);

	rc = virtio_netdev_feature_negotiate(vndev);
	if (rc != 0) {
		uk_pr_err("Failed to negotiate the device feature %d\n", rc);
		goto err_negotiate_feature;
	}

	rc = virtio_netdev_rxtx_alloc(vndev, conf);
	if (rc != 0) {
		uk_pr_err("Failed to probe the rx and tx rings %d\n", rc);
		goto err_negotiate_feature;
	}

	/* Initialize the count of the virtio-net device */
	vndev->rx_vqueue_cnt = 0;
	vndev->tx_vqueue_cnt = 0;

	uk_pr_info("Configured: features=0x%lx max_virtqueue_pairs=%"__PRIu16"\n",
		   vndev->vdev->features, vndev->max_vqueue_pairs);
exit:
	return rc;

err_negotiate_feature:
	virtio_dev_status_update(vndev->vdev, VIRTIO_CONFIG_STATUS_FAIL);
	goto exit;
}

static int virtio_net_rx_intr_enable(struct uk_netdev *n,
				     struct uk_netdev_rx_queue *queue)
{
	struct virtio_net_device *d __unused;
	int rc = 0;

	UK_ASSERT(n);
	d = to_virtionetdev(n);
	/* If the interrupt is enabled */
	if (queue->intr_enabled & VTNET_INTR_EN)
		return 0;

	/**
	 * Enable the user configuration bit. This would cause the interrupt to
	 * be enabled automatically, if the interrupt could not be enabled now
	 * due to data in the queue.
	 */
	queue->intr_enabled = VTNET_INTR_USR_EN;
	rc = virtqueue_intr_enable(queue->vq);
	if (!rc)
		queue->intr_enabled |= VTNET_INTR_EN;

	return rc;
}

static int virtio_net_rx_intr_disable(struct uk_netdev *n,
				      struct uk_netdev_rx_queue *queue)
{
	struct virtio_net_device *vndev __unused;

	UK_ASSERT(n);
	vndev = to_virtionetdev(n);
	virtqueue_intr_disable(queue->vq);
	queue->intr_enabled &= ~(VTNET_INTR_USR_EN | VTNET_INTR_EN);
	return 0;
}

static void virtio_net_info_get(struct uk_netdev *dev,
				struct uk_netdev_info *dev_info)
{
	struct virtio_net_device *vndev;

	UK_ASSERT(dev && dev_info);
	vndev = to_virtionetdev(dev);

	dev_info->max_rx_queues = vndev->max_vqueue_pairs;
	dev_info->max_tx_queues = vndev->max_vqueue_pairs;
	dev_info->nb_encap_tx = sizeof(struct virtio_net_hdr_padded);
	dev_info->nb_encap_rx = sizeof(struct virtio_net_hdr_padded);
	dev_info->features = UK_FEATURE_RXQ_INTR_AVAILABLE;
}

static int virtio_net_start(struct uk_netdev *n)
{
	struct virtio_net_device *d;
	int i = 0;

	UK_ASSERT(n != NULL);
	d = to_virtionetdev(n);

	/*
	 * By default, interrupts are disabled and it is up to the user or
	 * network stack to manually enable them with a call to
	 * enable_tx|rx_intr()
	 */
	for (i = 0; i < d->rx_vqueue_cnt; i++) {
		virtqueue_intr_disable(d->rxqs[i].vq);
		d->rxqs[i].intr_enabled = 0;
	}

	for (i = 0; i < d->tx_vqueue_cnt; i++) {
		virtqueue_intr_disable(d->txqs[i].vq);
		d->txqs[i].intr_enabled = 0;
	}

	/*
	 * Set the DRIVER_OK status bit. At this point the device is "live".
	 */
	virtio_dev_drv_up(d->vdev);
	uk_pr_info(DRIVER_NAME": %"__PRIu16" started\n", d->uid);

	return 0;
}

static inline void virtio_netdev_feature_set(struct virtio_net_device *vndev)
{
	vndev->vdev->features = 0;
	/* Setting the feature the driver support */
	VIRTIO_NET_DRV_FEATURES(vndev->vdev->features);
	/**
	 * TODO:
	 * Adding multiqueue support for the virtio net driver.
	 */
	vndev->max_vqueue_pairs = 1;
}

static const struct uk_netdev_ops virtio_netdev_ops = {
	.configure = virtio_netdev_configure,
	.rxq_configure = virtio_netdev_rx_queue_setup,
	.txq_configure = virtio_netdev_tx_queue_setup,
	.start = virtio_net_start,
	.rxq_intr_enable = virtio_net_rx_intr_enable,
	.rxq_intr_disable = virtio_net_rx_intr_disable,
	.info_get = virtio_net_info_get,
	.promiscuous_get = virtio_net_promisc_get,
	.hwaddr_get = virtio_net_mac_get,
	.mtu_get = virtio_net_mtu_get,
	.txq_info_get = virtio_netdev_txq_info_get,
	.rxq_info_get = virtio_netdev_rxq_info_get,
};

static int virtio_net_add_dev(struct virtio_dev *vdev)
{
	struct virtio_net_device *vndev;
	int rc = 0;

	UK_ASSERT(vdev != NULL);

	vndev = uk_calloc(a, 1, sizeof(*vndev));
	if (!vndev) {
		rc = -ENOMEM;
		goto err_out;
	}
	vndev->vdev = vdev;
	/* register netdev */
	vndev->netdev.rx = virtio_netdev_recv;
	vndev->netdev.tx = virtio_netdev_xmit;
	vndev->netdev.ops = &virtio_netdev_ops;

	rc = uk_netdev_drv_register(&vndev->netdev, a, drv_name);
	if (rc < 0) {
		uk_pr_err("Failed to register virtio-net device with libuknet\n");
		goto err_netdev_data;
	}
	vndev->uid = rc;
	rc = 0;
	vndev->max_mtu = UK_ETH_PAYLOAD_MAXLEN;
	vndev->mtu = vndev->max_mtu;
	vndev->promisc = 0;
	virtio_netdev_feature_set(vndev);
	uk_pr_info("virtio-net device registered with libuknet\n");

exit:
	return rc;
err_netdev_data:
	uk_free(a, vndev);
err_out:
	goto exit;
}

static int virtio_net_drv_init(struct uk_alloc *drv_allocator)
{
	/* driver initialization */
	if (!drv_allocator)
		return -EINVAL;

	a = drv_allocator;
	return 0;
}

static const struct virtio_dev_id vnet_dev_id[] = {
	{VIRTIO_ID_NET},
	{VIRTIO_ID_INVALID} /* List Terminator */
};

static struct virtio_driver vnet_drv = {
	.dev_ids = vnet_dev_id,
	.init    = virtio_net_drv_init,
	.add_dev = virtio_net_add_dev
};
VIRTIO_BUS_REGISTER_DRIVER(&vnet_drv);

#ifdef ERPC_DPDK

#include <rte_thash.h>
#include "dpdk_transport.h"
#include "util/huge_alloc.h"

namespace erpc {

static void format_pkthdr(pkthdr_t *pkthdr,
                          const Transport::tx_burst_item_t &item,
                          const size_t pkt_size) {
  // We can do an 8-byte aligned memcpy as the 2-byte UDP csum is already 0
  static constexpr size_t hdr_copy_sz = kInetHdrsTotSize - 2;
  static_assert(hdr_copy_sz == 40, "");
  memcpy(&pkthdr->headroom[0], item.routing_info, hdr_copy_sz);

  if (kTesting && item.drop) {
    // XXX: Can this cause performance problems?
    eth_hdr_t *eth_hdr = pkthdr->get_eth_hdr();
    memset(&eth_hdr->dst_mac, 0, sizeof(eth_hdr->dst_mac));
  }

  // On most bare-metal clusters, a zero IP checksum works fine. But on Azure
  // VMs we need a valid checksum.
  ipv4_hdr_t *ipv4_hdr = pkthdr->get_ipv4_hdr();
  ipv4_hdr->tot_len = htons(pkt_size - sizeof(eth_hdr_t));
  ipv4_hdr->check = get_ipv4_checksum(ipv4_hdr);

  udp_hdr_t *udp_hdr = pkthdr->get_udp_hdr();
  assert(udp_hdr->check == 0);
  udp_hdr->len = htons(pkt_size - sizeof(eth_hdr_t) - sizeof(ipv4_hdr_t));
}

void DpdkTransport::tx_burst(const tx_burst_item_t *tx_burst_arr,
                             size_t num_pkts) {
  rte_mbuf *tx_mbufs[kPostlist];

  for (size_t i = 0; i < num_pkts; i++) {
    const tx_burst_item_t &item = tx_burst_arr[i];
    const MsgBuffer *msg_buffer = item.msg_buffer;

    tx_mbufs[i] = rte_pktmbuf_alloc(mempool_);
    assert(tx_mbufs[i] != nullptr);

    pkthdr_t *pkthdr;
    if (item.pkt_idx == 0) {
      // This is the first packet, so we need only one seg. This can be CR/RFR.
      pkthdr = msg_buffer->get_pkthdr_0();
      const size_t pkt_size = msg_buffer->get_pkt_size<kMaxDataPerPkt>(0);
      format_pkthdr(pkthdr, item, pkt_size);

      tx_mbufs[i]->nb_segs = 1;
      tx_mbufs[i]->pkt_len = pkt_size;
      tx_mbufs[i]->data_len = pkt_size;
      memcpy(rte_pktmbuf_mtod(tx_mbufs[i], uint8_t *), pkthdr, pkt_size);
    } else {
      // This is not the first packet, so we need 2 segments.
      pkthdr = msg_buffer->get_pkthdr_n(item.pkt_idx);
      const size_t pkt_size =
          msg_buffer->get_pkt_size<kMaxDataPerPkt>(item.pkt_idx);
      format_pkthdr(pkthdr, item, pkt_size);

      tx_mbufs[i]->nb_segs = 2;
      tx_mbufs[i]->pkt_len = pkt_size;
      tx_mbufs[i]->data_len = sizeof(pkthdr_t);
      memcpy(rte_pktmbuf_mtod(tx_mbufs[i], uint8_t *), pkthdr,
             sizeof(pkthdr_t));

      tx_mbufs[i]->next = rte_pktmbuf_alloc(mempool_);
      assert(tx_mbufs[i]->next != nullptr);
      tx_mbufs[i]->next->data_len = pkt_size - sizeof(pkthdr_t);
      memcpy(rte_pktmbuf_mtod(tx_mbufs[i]->next, uint8_t *),
             &msg_buffer->buf[item.pkt_idx * kMaxDataPerPkt],
             pkt_size - sizeof(pkthdr_t));
    }

    ERPC_TRACE(
        "Transport: TX (idx = %zu, drop = %u). pkthdr = %s. Frame  = %s.\n", i,
        item.drop, pkthdr->to_string().c_str(),
        frame_header_to_string(&pkthdr->headroom[0]).c_str());
  }

  size_t nb_tx_new = rte_eth_tx_burst(phy_port, qp_id_, tx_mbufs, num_pkts);
  if (unlikely(nb_tx_new != num_pkts)) {
    size_t retry_count = 0;
    while (nb_tx_new != num_pkts) {
      nb_tx_new += rte_eth_tx_burst(phy_port, qp_id_, &tx_mbufs[nb_tx_new],
                                    num_pkts - nb_tx_new);
      retry_count++;
      if (unlikely(retry_count == 1000000000)) {
        ERPC_WARN("Rpc %u stuck in rte_eth_tx_burst", rpc_id);
        retry_count = 0;
      }
    }
  }
}

void DpdkTransport::tx_flush() {
  // Nothing to do because we don't zero-copy for now
  testing.tx_flush_count++;
  return;
}

void DpdkTransport::drain_rx_queue() {
  struct rte_mbuf *rx_pkts[kRxBatchSize];

  while (true) {
    size_t nb_rx_new =
        rte_eth_rx_burst(phy_port, qp_id_, rx_pkts, kRxBatchSize);
    if (nb_rx_new == 0) return;
    for (size_t i = 0; i < nb_rx_new; i++) rte_pktmbuf_free(rx_pkts[i]);
  }
}

size_t DpdkTransport::rx_burst() {
  struct rte_mbuf *rx_pkts[kRxBatchSize];
  size_t nb_rx_new = rte_eth_rx_burst(phy_port, qp_id_, rx_pkts, kRxBatchSize);

  for (size_t i = 0; i < nb_rx_new; i++) {
    rx_ring_[rx_ring_head_] = rte_pktmbuf_mtod(rx_pkts[i], uint8_t *);
    assert(dpdk_dtom(rx_ring_[rx_ring_head_]) == rx_pkts[i]);

    auto *pkthdr = reinterpret_cast<pkthdr_t *>(rx_ring_[rx_ring_head_]);
    _unused(pkthdr);
    ERPC_TRACE("Transport: RX pkthdr = %s. Frame = %s.\n",
               pkthdr->to_string().c_str(),
               frame_header_to_string(&pkthdr->headroom[0]).c_str());

#if DEBUG
    if (unlikely(ntohl(pkthdr->get_ipv4_hdr()->dst_ip) != resolve_.ipv4_addr ||
                 ntohs(pkthdr->get_udp_hdr()->dst_port) != rx_flow_udp_port_)) {
      ERPC_ERROR("Invalid packet. Pkt: %u %s %s. Me: %u %s %s\n",
                 ntohs(pkthdr->get_udp_hdr()->dst_port),
                 ipv4_to_string(pkthdr->get_ipv4_hdr()->dst_ip).c_str(),
                 mac_to_string(pkthdr->get_eth_hdr()->dst_mac).c_str(),
                 rx_flow_udp_port_, ipv4_to_string(resolve_.ipv4_addr).c_str(),
                 mac_to_string(resolve_.mac_addr).c_str());
      exit(-1);
    }
#endif

    rx_ring_head_ = (rx_ring_head_ + 1) % kNumRxRingEntries;
  }

  return nb_rx_new;
}

void DpdkTransport::post_recvs(size_t num_recvs) {
  for (size_t i = 0; i < num_recvs; i++) {
    auto *mbuf = dpdk_dtom(rx_ring_[rx_ring_tail_]);
#if DEBUG
    rte_mbuf_sanity_check(mbuf, true /* is_header */);
#endif
    rte_pktmbuf_free(mbuf);

    rx_ring_tail_ = (rx_ring_tail_ + 1) % kNumRxRingEntries;
  }
}

}  // namespace erpc

#endif

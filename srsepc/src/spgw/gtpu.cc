/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsepc/hdr/spgw/gtpu.h"
#include "srsepc/hdr/mme/mme_gtpc.h"
#include "srslte/upper/gtpu.h"
#include <algorithm>
#include <fcntl.h>
#include <inttypes.h> // for printing uint64_t
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/ip.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

namespace srsepc {

/**************************************
 *
 * GTP-U class that handles the packet
 * forwarding to and from eNBs
 *
 **************************************/

spgw::gtpu::gtpu() : m_sgi_up(false), m_s1u_up(false)
{
  m_pool = srslte::byte_buffer_pool::get_instance();
  return;
}

spgw::gtpu::~gtpu()
{
  return;
}

int spgw::gtpu::init(spgw_args_t* args, spgw* spgw, gtpc_interface_gtpu* gtpc, srslte::log_ref gtpu_log)
{
  int err;

  // Init log
  m_gtpu_log = gtpu_log;

  // Store interfaces
  m_spgw = spgw;
  m_gtpc = gtpc;

  // Init SGi interface
  err = init_sgi(args);
  if (err != SRSLTE_SUCCESS) {
    srslte::console("Could not initialize the SGi interface.\n");
    return err;
  }

  // Init S1-U
  err = init_s1u(args);
  if (err != SRSLTE_SUCCESS) {
    srslte::console("Could not initialize the S1-U interface.\n");
    return err;
  }

  m_gtpu_log->info("SPGW GTP-U Initialized.\n");
  srslte::console("SPGW GTP-U Initialized.\n");
  return SRSLTE_SUCCESS;
}

void spgw::gtpu::stop()
{
  // Clean up SGi interface
  if (m_sgi_up) {
    close(m_sgi);
  }
  // Clean up S1-U socket
  if (m_s1u_up) {
    close(m_s1u);
  }
}

int spgw::gtpu::init_sgi(spgw_args_t* args)
{
  struct ifreq ifr;
  int          sgi_sock;

  if (m_sgi_up) {
    return SRSLTE_ERROR_ALREADY_STARTED;
  }

  // Construct the TUN device
  m_sgi = open("/dev/net/tun", O_RDWR);
  m_gtpu_log->info("TUN file descriptor = %d\n", m_sgi);
  if (m_sgi < 0) {
    m_gtpu_log->error("Failed to open TUN device: %s\n", strerror(errno));
    return SRSLTE_ERROR_CANT_START;
  }

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  strncpy(
      ifr.ifr_ifrn.ifrn_name, args->sgi_if_name.c_str(), std::min(args->sgi_if_name.length(), (size_t)(IFNAMSIZ - 1)));
  ifr.ifr_ifrn.ifrn_name[IFNAMSIZ - 1] = '\0';

  if (ioctl(m_sgi, TUNSETIFF, &ifr) < 0) {
    m_gtpu_log->error("Failed to set TUN device name: %s\n", strerror(errno));
    close(m_sgi);
    return SRSLTE_ERROR_CANT_START;
  }

  // Bring up the interface
  sgi_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (ioctl(sgi_sock, SIOCGIFFLAGS, &ifr) < 0) {
    m_gtpu_log->error("Failed to bring up socket: %s\n", strerror(errno));
    close(sgi_sock);
    close(m_sgi);
    return SRSLTE_ERROR_CANT_START;
  }

  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  if (ioctl(sgi_sock, SIOCSIFFLAGS, &ifr) < 0) {
    m_gtpu_log->error("Failed to set socket flags: %s\n", strerror(errno));
    close(sgi_sock);
    close(m_sgi);
    return SRSLTE_ERROR_CANT_START;
  }

  // Set IP of the interface
  struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
  addr->sin_family         = AF_INET;
  addr->sin_addr.s_addr    = inet_addr(args->sgi_if_addr.c_str());
  addr->sin_port           = 0;

  if (ioctl(sgi_sock, SIOCSIFADDR, &ifr) < 0) {
    m_gtpu_log->error(
        "Failed to set TUN interface IP. Address: %s, Error: %s\n", args->sgi_if_addr.c_str(), strerror(errno));
    close(m_sgi);
    close(sgi_sock);
    return SRSLTE_ERROR_CANT_START;
  }

  ifr.ifr_netmask.sa_family                                = AF_INET;
  ((struct sockaddr_in*)&ifr.ifr_netmask)->sin_addr.s_addr = inet_addr(args->sgi_if_netmask.c_str());
  if (ioctl(sgi_sock, SIOCSIFNETMASK, &ifr) < 0) {
    m_gtpu_log->error("Failed to set TUN interface Netmask. Error: %s\n", strerror(errno));
    close(m_sgi);
    close(sgi_sock);
    return SRSLTE_ERROR_CANT_START;
  }

  close(sgi_sock);
  m_sgi_up = true;
  m_gtpu_log->info("Initialized SGi interface\n");
  return SRSLTE_SUCCESS;
}

int spgw::gtpu::init_s1u(spgw_args_t* args)
{
  // Open S1-U socket
  m_s1u = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_s1u == -1) {
    m_gtpu_log->error("Failed to open socket: %s\n", strerror(errno));
    return SRSLTE_ERROR_CANT_START;
  }
  m_s1u_up = true;

  // Bind the socket
  m_s1u_addr.sin_family      = AF_INET;
  m_s1u_addr.sin_addr.s_addr = inet_addr(args->gtpu_bind_addr.c_str());
  m_s1u_addr.sin_port        = htons(GTPU_RX_PORT);

  if (bind(m_s1u, (struct sockaddr*)&m_s1u_addr, sizeof(struct sockaddr_in))) {
    m_gtpu_log->error("Failed to bind socket: %s\n", strerror(errno));
    return SRSLTE_ERROR_CANT_START;
  }
  m_gtpu_log->info("S1-U socket = %d\n", m_s1u);
  m_gtpu_log->info("S1-U IP = %s, Port = %d \n", inet_ntoa(m_s1u_addr.sin_addr), ntohs(m_s1u_addr.sin_port));

  m_gtpu_log->info("Initialized S1-U interface\n");
  return SRSLTE_SUCCESS;
}

void spgw::gtpu::handle_sgi_pdu(srslte::byte_buffer_t* msg)
{
  bool usr_found = false;
  bool ctr_found = false;

  std::map<uint32_t, srslte::gtpc_f_teid_ie>::iterator gtpu_fteid_it;
  std::map<in_addr_t, uint32_t>::iterator              gtpc_teid_it;
  srslte::gtpc_f_teid_ie                               enb_fteid;
  uint32_t                                             spgw_teid;
  struct iphdr*                                        iph = (struct iphdr*)msg->msg;
  m_gtpu_log->debug("Received SGi PDU. Bytes %d\n", msg->N_bytes);

  if (iph->version != 4) {
    m_gtpu_log->warning("IPv6 not supported yet.\n");
    return;
  }
  if (ntohs(iph->tot_len) < 20) {
    m_gtpu_log->warning("Invalid IP header length. IP length %d.\n", ntohs(iph->tot_len));
    return;
  }

  // Logging PDU info
  m_gtpu_log->debug("SGi PDU -- IP version %d, Total length %d\n", iph->version, ntohs(iph->tot_len));
  m_gtpu_log->debug("SGi PDU -- IP src addr %s\n", srslte::gtpu_ntoa(iph->saddr).c_str());
  m_gtpu_log->debug("SGi PDU -- IP dst addr %s\n", srslte::gtpu_ntoa(iph->daddr).c_str());

  // Find user and control tunnel
  gtpu_fteid_it = m_ip_to_usr_teid.find(iph->daddr);
  if (gtpu_fteid_it != m_ip_to_usr_teid.end()) {
    usr_found = true;
    enb_fteid = gtpu_fteid_it->second;
  }
  gtpc_teid_it = m_ip_to_ctr_teid.find(iph->daddr);
  if (gtpc_teid_it != m_ip_to_ctr_teid.end()) {
    ctr_found = true;
    spgw_teid = gtpc_teid_it->second;
  }

  // Handle SGi packet
  if (usr_found == false && ctr_found == false) {
    m_gtpu_log->debug("Packet for unknown UE.\n");
    goto pkt_discard_out;
  } else if (usr_found == false && ctr_found == true) {
    m_gtpu_log->debug("Packet for attached UE that is not ECM connected.\n");
    m_gtpu_log->debug("Triggering Donwlink Notification Requset.\n");
    m_gtpc->send_downlink_data_notification(spgw_teid);
    m_gtpc->queue_downlink_packet(spgw_teid, msg);
    return;
  } else if (usr_found == false && ctr_found == true) {
    m_gtpu_log->error("User plane tunnel found without a control plane tunnel present.\n");
    goto pkt_discard_out;
  } else {
    send_s1u_pdu(enb_fteid, msg);
  }
  return;

pkt_discard_out:
  m_pool->deallocate(msg);
  return;
}

void spgw::gtpu::handle_s1u_pdu(srslte::byte_buffer_t* msg)
{
  srslte::gtpu_header_t header;
  srslte::gtpu_read_header(msg, &header, m_gtpu_log);

  m_gtpu_log->debug("Received PDU from S1-U. Bytes=%d\n", msg->N_bytes);
  m_gtpu_log->debug("TEID 0x%x. Bytes=%d\n", header.teid, msg->N_bytes);
  int n = write(m_sgi, msg->msg, msg->N_bytes);
  if (n < 0) {
    m_gtpu_log->error("Could not write to TUN interface.\n");
  } else {
    m_gtpu_log->debug("Forwarded packet to TUN interface. Bytes= %d/%d\n", n, msg->N_bytes);
  }
  return;
}

void spgw::gtpu::send_s1u_pdu(srslte::gtp_fteid_t enb_fteid, srslte::byte_buffer_t* msg)
{
  // Set eNB destination address
  struct sockaddr_in enb_addr;
  enb_addr.sin_family      = AF_INET;
  enb_addr.sin_port        = htons(GTPU_RX_PORT);
  enb_addr.sin_addr.s_addr = enb_fteid.ipv4;

  // Setup GTP-U header
  srslte::gtpu_header_t header;
  header.flags        = GTPU_FLAGS_VERSION_V1 | GTPU_FLAGS_GTP_PROTOCOL;
  header.message_type = GTPU_MSG_DATA_PDU;
  header.length       = msg->N_bytes;
  header.teid         = enb_fteid.teid;

  m_gtpu_log->debug("User plane tunnel found SGi PDU. Forwarding packet to S1-U.\n");
  m_gtpu_log->debug("eNB F-TEID -- eNB IP %s, eNB TEID 0x%x.\n", inet_ntoa(enb_addr.sin_addr), enb_fteid.teid);

  // Write header into packet
  int n;
  if (!srslte::gtpu_write_header(&header, msg, m_gtpu_log)) {
    m_gtpu_log->error("Error writing GTP-U header on PDU\n");
    goto out;
  }

  // Send packet to destination
  n = sendto(m_s1u, msg->msg, msg->N_bytes, 0, (struct sockaddr*)&enb_addr, sizeof(enb_addr));
  if (n < 0) {
    m_gtpu_log->error("Error sending packet to eNB\n");
  } else if ((unsigned int)n != msg->N_bytes) {
    m_gtpu_log->error("Mis-match between packet bytes and sent bytes: Sent: %d/%d\n", n, msg->N_bytes);
  }

out:
  m_gtpu_log->debug("Deallocating packet after sending S1-U message\n");
  m_pool->deallocate(msg);
  return;
}

void spgw::gtpu::send_all_queued_packets(srslte::gtp_fteid_t                 dw_user_fteid,
                                         std::queue<srslte::byte_buffer_t*>& pkt_queue)
{
  m_gtpu_log->debug("Sending all queued packets\n");
  while (!pkt_queue.empty()) {
    srslte::byte_buffer_t* msg = pkt_queue.front();
    send_s1u_pdu(dw_user_fteid, msg);
    pkt_queue.pop();
  }
  return;
}

/*
 * Tunnel managment
 */
bool spgw::gtpu::modify_gtpu_tunnel(in_addr_t ue_ipv4, srslte::gtpc_f_teid_ie dw_user_fteid, uint32_t up_ctrl_teid)
{
  m_gtpu_log->info("Modifying GTP-U Tunnel.\n");
  m_gtpu_log->info("UE IP %s\n", srslte::gtpu_ntoa(ue_ipv4).c_str());
  m_gtpu_log->info(
      "Downlink eNB addr %s, U-TEID 0x%x\n", srslte::gtpu_ntoa(dw_user_fteid.ipv4).c_str(), dw_user_fteid.teid);
  m_gtpu_log->info("Uplink C-TEID: 0x%x\n", up_ctrl_teid);
  m_ip_to_usr_teid[ue_ipv4] = dw_user_fteid;
  m_ip_to_ctr_teid[ue_ipv4] = up_ctrl_teid;
  return true;
}

bool spgw::gtpu::delete_gtpu_tunnel(in_addr_t ue_ipv4)
{
  // Remove GTP-U connections, if any.
  if (m_ip_to_usr_teid.count(ue_ipv4)) {
    m_ip_to_usr_teid.erase(ue_ipv4);
  } else {
    m_gtpu_log->error("Could not find GTP-U Tunnel to delete.\n");
    return false;
  }
  return true;
}

bool spgw::gtpu::delete_gtpc_tunnel(in_addr_t ue_ipv4)
{
  // Remove Ctrl TEID from IP mapping.
  if (m_ip_to_ctr_teid.count(ue_ipv4)) {
    m_ip_to_ctr_teid.erase(ue_ipv4);
  } else {
    m_gtpu_log->error("Could not find GTP-C Tunnel info to delete.\n");
    return false;
  }
  return true;
}

} // namespace srsepc

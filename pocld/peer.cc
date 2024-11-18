/* peer.cc -- class representing a direct server-server connection

   Copyright (c) 2018 Michal Babej / Tampere University of Technology
   Copyright (c) 2019-2023 Jan Solanti / Tampere University

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#include <cassert>
#include <sstream>
#include <sys/socket.h>

#include "common_cl.hh"
#include "peer.hh"
#include "shared_cl_context.hh"

#ifdef ENABLE_RDMA
#include "rdma.hh"
#endif

const char *request_to_str(RequestMessageType type);

#ifdef ENABLE_RDMA
Peer::Peer(uint64_t id, uint32_t handler_id, VirtualContextBase *ctx,
           ExitHelper *eh, std::shared_ptr<Connection> Conn,
           std::shared_ptr<RdmaConnection> conn)
    : id(id), handler_id(handler_id), ctx(ctx), eh(eh), Conn(Conn), rdma(conn)
#else
Peer::Peer(uint64_t id, uint32_t handler_id, VirtualContextBase *ctx,
           ExitHelper *eh, std::shared_ptr<Connection> Conn)
    : id(id), handler_id(handler_id), Conn(Conn), ctx(ctx), eh(eh)
#endif
{
  std::stringstream ss;
  ss << "PEER_" << id;
#ifdef ENABLE_RDMA
  // TODO: register all existing buffers for rdma, if the ability to add peers
  // after init is added
  rdma_reader = RdmaRequestThreadUPtr(new RdmaRequestThread(
      ctx, eh, Conn->meter(), (ss.str() + "_RDMA_R").c_str(), rdma,
      &local_memory_regions, &local_regions_mutex));
  rdma_writer = std::thread(&Peer::rdmaWriterThread, this);
#endif
  reader = RequestQueueThreadUPtr(
      new RequestQueueThread(Conn, ctx, eh, (ss.str() + "_R").c_str()));
  writer = std::thread(&Peer::writerThread, this);
}

Peer::~Peer() {
  reader.reset();
  if (writer.joinable())
    writer.join();
#ifdef ENABLE_RDMA
  Request *R = new Request{};
  R->Body.message_type = MessageType_Shutdown;
  rdma_out_queue.push(R);
  rdma_reader.reset();
  if (rdma_writer.joinable())
    rdma_writer.join();
#endif
}

void Peer::pushRequest(Request *r) {
#ifdef ENABLE_RDMA
  if (pocl_request_is_rdma(&r->Body, 1))
    rdma_out_queue.push(r);
  else
#endif
    out_queue.push(r);
}

void Peer::writerThread() {
  do {
    Request *r = out_queue.pop();
    if (r == nullptr) {
      out_queue.wait_cond();
      continue;
    }

    uint32_t msg_size = request_size(r->Body.message_type);
    POCL_MSG_PRINT_GENERAL(
        "PHW: SENDING MESSAGE, ID: %" PRIu64 " TYPE: %s SIZE: %" PRIu32
        ", TO %" PRIu64 "\n",
        uint64_t(r->Body.msg_id),
        request_to_str(static_cast<RequestMessageType>(r->Body.message_type)),
        msg_size, id);

    CHECK_WRITE(Conn->writeFull(&msg_size, sizeof(uint32_t)), "PHW");
    CHECK_WRITE(Conn->writeFull(&r->Body, msg_size), "PHW");

    assert(r->Waitlist.size() == r->Body.waitlist_size);
    if (r->Body.waitlist_size > 0) {
      POCL_MSG_PRINT_GENERAL("PHW: WRITING WAIT LIST: %" PRIu32 "\n",
                             r->Body.waitlist_size);
      CHECK_WRITE(Conn->writeFull(r->Waitlist.data(),
                                  r->Body.waitlist_size * sizeof(uint64_t)),
                  "PHW");
    }

    assert(r->ExtraData.size() >= r->ExtraDataSize);
    if (r->ExtraDataSize > 0) {
      POCL_MSG_PRINT_GENERAL("PHW: WRITING EXTRA: %" PRIuS "\n",
                             r->ExtraDataSize);
      CHECK_WRITE(Conn->writeFull(r->ExtraData.data(), r->ExtraDataSize),
                  "PHW");
    }

    assert(r->ExtraData2.size() >= r->ExtraData2Size);
    if (r->ExtraData2Size > 0) {
      POCL_MSG_PRINT_GENERAL("PHW: WRITING EXTRA2: %" PRIuS "\n",
                             r->ExtraData2Size);
      CHECK_WRITE(Conn->writeFull(r->ExtraData2.data(), r->ExtraData2Size),
                  "PHW");
    }

    delete r;
  } while (!eh->exit_requested());
}

#ifdef ENABLE_RDMA
void Peer::rdmaWriterThread() {
  std::stringstream ss;
  ss << "PEER_" << id << "_RDMA_W";
  std::string id_str = ss.str();

  // Traffic is measured per peer, shared with the TCP connections
  std::shared_ptr<TrafficMonitor> Netstat = Conn->meter();

  RdmaBuffer<RequestMsg_t> cmd_buf(rdma->protectionDomain(), 1);

  while (!eh->exit_requested()) {
    Request *r = rdma_out_queue.pop();
    if (r == nullptr) {
      rdma_out_queue.wait_cond();
      continue;
    }

    /************* set up command metadata transfer *************/
    cmd_buf.at(0) = r->Body;

    uint32_t cmd_size = request_size(r->Body.message_type);
    WorkRequest cmd_wr = WorkRequest::Send(0, {{*cmd_buf, 0, cmd_size}},
                                           WorkRequest::Flags::Signaled |
                                               WorkRequest::Flags::Solicited);

    /************** set up buffer contents transfer *************/
    ptrdiff_t src_offset = transfer_src_offset(r->Body);
    uint64_t data_size = transfer_size(r->Body);

    POCL_MSG_PRINT_GENERAL("%s: RDMA WRITE FOR MESSAGE ID: %" PRIu64
                           ", SIZE: %" PRIu64 "\n",
                           id_str.c_str(), uint64_t(r->Body.msg_id), data_size);
    RdmaBufferData local;
    {
      std::unique_lock<std::mutex> l(local_regions_mutex);
      auto it = local_memory_regions.find(r->Body.obj_id);
      if (it == local_memory_regions.end()) {
        POCL_MSG_ERR(
            "%s: ERROR: no local RDMA memory region for buffer %" PRIu32 "\n",
            id_str.c_str(), uint32_t(r->Body.obj_id));
        eh->requestExit("RDMA transfer requested on unregistered buffer", -1);
        break;
      }
      local = it->second;
    }
    RdmaRemoteBufferData remote;
    {
      std::unique_lock<std::mutex> l(remote_regions_mutex);
      auto it = remote_memory_regions.find(r->Body.obj_id);
      if (it == remote_memory_regions.end()) {
        POCL_MSG_ERR(
            "%s: ERROR: no remote RDMA memory region for buffer %" PRIu32 "\n",
            id_str.c_str(), uint32_t(r->Body.obj_id));
        eh->requestExit("RDMA transfer requested on unregistered buffer", -1);
        break;
      }
      remote = it->second;
    }

    /* 0 is a special value that means 2^31 in the sg_list. Messages of >2^31
     * bytes are not allowed with RDMA_WRITE. */
    uint32_t write_size = data_size >= 0x80000000 ? 0 : uint32_t(data_size);
    std::vector<ScatterGatherEntry> sg_list;
    if (data_size > 0)
      sg_list.push_back({local.shadow_region, 0, write_size});
    WorkRequest data_wr = WorkRequest::RdmaWrite(
        0, std::move(sg_list), remote.address + src_offset, remote.rkey);
    for (uint64_t i = 1; i * 0x80000000 < data_size; ++i) {
      uint64_t offset = i * 0x80000000;
      uint64_t remain = data_size - offset;
      write_size = remain >= 0x80000000 ? 0 : uint32_t(remain);
      data_wr.chain(WorkRequest::RdmaWrite(
          0, {{local.shadow_region, (ptrdiff_t)offset, write_size}},
          remote.address + src_offset + offset, remote.rkey));
    }
    data_wr.chain(std::move(cmd_wr));

    /******************* submit work requests *******************/
    if (Netstat.get())
      Netstat->txSubmitted(cmd_size + data_size);

    try {
      rdma->post(data_wr);
    } catch (const std::runtime_error &e) {
      POCL_MSG_ERR("%s: RDMA ERROR: %s\n", id_str.c_str(), e.what());
      eh->requestExit("Posting RDMA send request failed", -1);
      delete r;
      break;
    }

    /************ await work completion notification ************/

    try {
      ibverbs::WorkCompletion wc = rdma->awaitSendCompletion();
    } catch (const std::runtime_error &e) {
      POCL_MSG_ERR("%s: RDMA event polling error: %s\n", id_str.c_str(),
                   e.what());
      eh->requestExit("RDMA event polling failure", -1);
      delete r;
      break;
    }

    if (Netstat.get())
      Netstat->txConfirmed(cmd_size + data_size);

    delete r;
  }
}

void Peer::notifyBufferRegistration(uint32_t buf_id, uint32_t rkey,
                                    uint64_t vaddr) {
  std::unique_lock<std::mutex> l(remote_regions_mutex);
  RdmaRemoteBufferData d = {vaddr, rkey};
  POCL_MSG_PRINT_GENERAL("PEER_%" PRIu64
                         ": storing remote rdma info mem_object=%" PRIu32
                         " rkey=0x%08" PRIx32 "\n",
                         this->id, buf_id, rkey);
  remote_memory_regions.insert({buf_id, d});
}

bool Peer::rdmaRegisterBuffer(uint32_t buf_id, char *buf, size_t size) {
  ibverbs::MemoryRegionPtr mem_region;
  try {
    mem_region = ibverbs::MemoryRegion::register_ptr(
        rdma->protectionDomain(), buf, size,
        ibverbs::MemoryRegion::Access::LocalWrite |
            ibverbs::MemoryRegion::Access::RemoteRead |
            ibverbs::MemoryRegion::Access::RemoteWrite);
  } catch (const std::runtime_error &e) {
    POCL_MSG_ERR("PEER_%" PRIu64 " Register memory region (ptr=%p, size=%" PRIuS
                 ") failed: %s\n",
                 this->id, buf, size, e.what());
    return false;
  }

  POCL_MSG_PRINT_GENERAL("PEER_%" PRIu64
                         ": storing local memory region ptr=%p, rkey=%08" PRIu32
                         " mem_obj=%" PRIu32 "\n",
                         this->id, (void *)buf, mem_region->rkey(), buf_id);
  RdmaBufferData d = {buf, mem_region};
  {
    std::unique_lock<std::mutex> l(local_regions_mutex);
    local_memory_regions.insert({buf_id, d});
  }

  // Very quick & dirty notification message that
  // grossly abuses the default message fields...
  Request *notif = new Request();
  notif->Body.message_type = MessageType_RdmaBufferRegistration;
  notif->Body.obj_id = buf_id;
  notif->Body.did = mem_region->rkey();
  notif->Body.msg_id = (uintptr_t)buf;
  notif->Body.cq_id = this->handler_id;
  this->pushRequest(notif);
  return true;
}

void Peer::rdmaUnregisterBuffer(uint32_t id) {
  {
    std::unique_lock<std::mutex> l(local_regions_mutex);
    local_memory_regions.erase(id);
  }
  {
    std::unique_lock<std::mutex> l(remote_regions_mutex);
    remote_memory_regions.erase(id);
  }
}

#endif

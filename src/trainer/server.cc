#include <thread>
#include <chrono>
#include "mshadow/tensor.h"
#include "trainer/server.h"
#include "utils/param.h"
#include "utils/singleton.h"
#include "utils/factory.h"
#include "utils/cluster.h"
#include "proto/common.pb.h"

namespace singa {

using namespace mshadow;
using std::vector;

Server::Server(int thread_id,int group_id, int server_id):
  thread_id_(thread_id),grp_id_(group_id), id_(server_id){
}

void Server::Setup(const UpdaterProto& proto,
    const vector<int>& slice2group,
    const vector<int>& slice2server) {
  updater_ = Updater::Create(proto);
  slice2group_ = slice2group;
  slice2server_ = slice2server;
  nUpdates_.resize(slice2group_.size(), 0);
  nPendingSync_.resize(slice2group_.size(), 0);
  last_sync_.resize(slice2group_.size());
}

Server::~Server() {
  delete updater_;
  // free Params (i.e., slices) in server shard
  for (auto entry : shard_)
    for (auto param : entry.second->shares)
      delete param;
}

void Stop(void * running) {
  *static_cast<bool *>(running) = false;
}

void Server::Run() {
  LOG(ERROR) << "Server (group = " << grp_id_ <<", id = " << id_ << ") start";

  auto dealer = new Dealer(2*thread_id_);
  CHECK(dealer->Connect(kInprocRouterEndpoint));
  Msg* ping = new Msg(Addr(grp_id_, id_, kServer), Addr(-1, -1, kStub));
  ping->set_type(kConnect);
  dealer->Send(&ping);

  auto cluster = Cluster::Get();
  bool running = true;
  CHECK(cluster->runtime()->WatchSGroup(grp_id_, id_, Stop, &running));
  Poller poll(dealer);
  // start recv loop and process requests
  while (running) {
    // must use poller here; otherwise Receive() gets stuck after workers stop.
    auto *sock = poll.Wait(cluster->poll_time());
    if (poll.Terminated()) {
      LOG(ERROR) << "Connection broken!";
      exit(0);
    } else if (sock == nullptr) {
      continue;
    }
    Msg* msg = dealer->Receive();
    if (msg == nullptr) break; //  interrupted
    Msg* response = nullptr;
    int type = msg->type();
    int slice_id = SliceID(msg->trgt_val());
    if (type == kPut) {
      response = HandlePut(&msg);
    } else {
      if (shard_.find(slice_id) == shard_.end()) {
        // delay the processing by re-queue the msg. May sleep for a while?
        response = msg;
      }  else {
        switch (type) {
          case kGet:
            response = HandleGet(&msg);
            break;
          case kUpdate:
            for (auto reply : HandleUpdate(&msg))
              dealer->Send(&reply);
            break;
          case kSyncRequest:
            response = HandleSyncRequest(&msg);
            break;
          case kSyncResponse:
            HandleSyncResponse(&msg);
            break;
          default:
            LOG(ERROR)<<"Unknown message type "<<type;
            break;
        }
      }
    }
    if (response != nullptr)
      dealer->Send(&response);
  }

  // send stop msg to stub
  Msg* msg = new Msg(Addr(grp_id_, id_, kServer), Addr(-1, -1, kStub));
  msg->set_type(kStop);
  dealer->Send(&msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  LOG(ERROR) << "Server (group = " << grp_id_ << ", id = " << id_ << ") stops";
  delete dealer;
}

Msg* Server::HandlePut(Msg **msg) {
  int version = (*msg)->trgt_version();
  int slice_id = SliceID((*msg)->trgt_val());
  if (shard_.find(slice_id) != shard_.end())
    LOG(FATAL) << "Param (" << slice_id << ") is put more than once";

  // TODO(wangwei) replace hard coded param type 0
  auto  param = Singleton<Factory<Param>>::Instance()->Create(0);
  auto response = param->HandlePutMsg(msg, true);
  // parse num of shares of this param from a worker group
  int num_shares = 1;
  if ((*msg)->NextFrame())
    (*msg)->ParseFormatFrame("i", &num_shares);
  DeleteMsg(msg);
  shard_[slice_id] = new ParamEntry(num_shares, param);
  // must set version after HandlePutMsg which allocates the memory
  param->set_version(version);
  param->set_local_version(version);
  param->set_id(slice_id);
  // allocate blob for param sync between groups.
  if (slice2group_[slice_id] != grp_id_) {
    last_sync_[slice_id].ReshapeLike(param->data());
    last_sync_[slice_id].CopyFrom(param->data());
  }
  LOG(INFO)<<"server (group = " << grp_id_ << ", id = " << id_ <<") put slice="
    << slice_id << " size=" << param->size();
  return response;
}

Msg* Server::HandleGet(Msg **msg) {
  int val = (*msg)->trgt_val();
  auto param = shard_.at(SliceID(val))->shares.at(0);
  // re-queue the request if the param is not updated to the required version
  if(param->version()<(*msg)->trgt_version())
    return *msg;
  else {
    // LOG(ERROR) << "get " << slice << " from "<<(*msg)->src_first();
    auto reply = param->HandleGetMsg(msg, false);
    reply->set_trgt(val, param->version());
    return reply;
  }
}

const vector<Msg*> Server::HandleUpdate(Msg **msg) {
  vector<Msg*> ret;
  int sliceid = SliceID((*msg)->trgt_val());
  auto entry = shard_.at(sliceid);
  buffer_requests_[sliceid].push_back(*msg);
  int num_update;
  (*msg)->LastFrame();
  (*msg)->ParseFormatFrame("i", &num_update);
  (*msg)->FirstFrame();
  entry->num_update += num_update;
  // LOG(ERROR) << "update "<< sliceid << " from " << AddrGrp((*msg)->src()) << ", " << num_update << " total " << entry->num_total;
  // do update until recv gradients from all shares of this param/slice
  if (entry->num_update >= entry->num_total) {
    CHECK_EQ(entry->num_update, entry->num_total);
    auto& request = buffer_requests_.at(sliceid);
    int step = (*msg)->trgt_version();
    auto param = entry->shares.at(0);
    // extract and aggregate gradients
    param->ParseUpdateMsgs(request);
    updater_->Update(step, param, 1.0f / entry->num_total);
    param->set_local_version(param->local_version() + 1);
    // response to all shares of this param
    for (auto response : param->GenUpdateResponseMsgs(&request, false)) {
      response->set_trgt((*msg)->trgt_val(), param->local_version());
      ret.push_back(response);
    }
    entry->num_update = 0;
    nUpdates_[sliceid]++;
    // sync with master group after at least sync_freq local updates
    // the last check is to avoid sending msg to stopped servers
    if (slice2group_[sliceid] != grp_id_
        && nUpdates_[sliceid] >= Cluster::Get()->sync_freq()
        && nPendingSync_[sliceid] <= Cluster::Get()->sync_freq()) {
      auto shape = Shape1(param->size());
      Tensor<cpu, 1> tmp(last_sync_[sliceid].mutable_cpu_data(), shape);
      Tensor<cpu, 1> cur(param->mutable_cpu_data(), shape);
      tmp = cur - tmp;
      int addr = Addr(slice2group_[sliceid], slice2server_[sliceid], kServer);
      Msg* sync = new Msg(Addr(grp_id_, id_, kServer), addr);
      sync->set_type(kSyncRequest);
      sync->set_trgt((*msg)->trgt_val(), param->local_version());
      sync->AddFrame(tmp.dptr, param->size() * sizeof(float));
      Copy(tmp, cur);
      ret.push_back(sync);
      nUpdates_[sliceid] = 0;
      nPendingSync_[sliceid]++;
    }
  }
  *msg = nullptr;
  return ret;
}

Msg* Server::HandleSyncRequest(Msg **msg) {
  Msg* msgg = *msg;
  int slice = SliceID(msgg->trgt_val());
  auto param = shard_.at(slice)->shares.at(0);
  auto shape = Shape1(param->size());
  CHECK_EQ(msgg->FrameSize(), param->size()*sizeof(float));
  Tensor<cpu, 1> inc(static_cast<float*>(msgg->FrameData()), shape);
  Tensor<cpu, 1> cur(param->mutable_cpu_data(), shape);
  // recv sync msg on the slice I am maintaining
  cur += inc;
  msgg->SwapAddr();
  msgg->set_type(kSyncResponse);
  // copy the fresh param value into the response msg
  Copy(inc, cur);
  return msgg;
}

// recv sync msg on slice mastered by others
void Server::HandleSyncResponse(Msg **msg) {
  Msg* msgg = *msg;
  int slice = SliceID(msgg->trgt_val());
  auto param = shard_.at(slice)->shares.at(0);
  auto shape=Shape1(param->size());
  Tensor<cpu, 1> prev(last_sync_[param->id()].mutable_cpu_data(), shape);
  Tensor<cpu, 1> cur(param->mutable_cpu_data(), shape);
  Tensor<cpu, 1> master(static_cast<float*>(msgg->FrameData()), shape);
  cur += master - prev;  // cur = master + (cur - prev);
  Copy(prev, cur);
  DeleteMsg(msg);
  nPendingSync_[slice]--;
}

} /* singa */

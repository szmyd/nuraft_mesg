///
// Copyright 2018 (c) eBay Corporation
//
// Authors:
//      Brian Szmyd <bszmyd@ebay.com>
//
// Brief:
//   grpc_factory static functions that makes for easy client creation.
//

#include "grpc_factory.hpp"

namespace raft_core {

template<typename Payload>
struct client_ctx {
   client_ctx(Payload payload, shared<grpc_factory> factory) :
      _payload(payload),
      _cli_factory(factory)
   { }

   Payload payload() const                      { return _payload; }
   shared<grpc_factory> cli_factory() const     { return _cli_factory; }
   std::future<bool> future()                   { return _promise.get_future(); }
   void set(bool const success)                 { return _promise.set_value(success); }

 private:
   Payload const           _payload;
   shared<grpc_factory>    _cli_factory;

   std::condition_variable _join_cv;
   std::mutex              _join_lk;

   std::promise<bool> _promise;
};

template<typename PayloadType>
shared<cstn::req_msg>
createMessage(PayloadType payload);

template<>
shared<cstn::req_msg>
createMessage(uint32_t const srv_id) {
   auto srv_addr = std::to_string(srv_id);
   auto srv_conf = cstn::srv_config(srv_id, srv_addr);
   auto log = std::make_shared<cstn::log_entry>(
      0,
      srv_conf.serialize(),
      cstn::log_val_type::cluster_server
      );
   auto msg = std::make_shared<cstn::req_msg>(0, cstn::msg_type::add_server_request, 0, 0, 0, 0, 0);
   msg->log_entries().push_back(log);
   return msg;
}

template<>
shared<cstn::req_msg>
createMessage(shared<cstn::buffer> buf) {
   auto log = std::make_shared<cstn::log_entry>(0, buf);
   auto msg = std::make_shared<cstn::req_msg>(0, cstn::msg_type::client_request, 0, 1, 0, 0, 0);
   msg->log_entries().push_back(log);
   return msg;
}

template<>
shared<cstn::req_msg>
createMessage(int32_t const srv_id) {
    auto buf = cstn::buffer::alloc(sizeof(srv_id));
    buf->put(srv_id);
    buf->pos(0);
    auto log = std::make_shared<cstn::log_entry>(0, buf, cstn::log_val_type::cluster_server);
    auto msg = std::make_shared<cstn::req_msg>(0, cstn::msg_type::remove_server_request, 0, 0, 0, 0, 0);
    msg->log_entries().push_back(log);
    return msg;
}

template<typename ContextType>
void
respHandler(shared<ContextType> ctx,
            shared<cstn::resp_msg>& rsp,
            shared<cstn::rpc_exception>& err) {
   if (err) {
      LOGERROR("{}", err->what());
      ctx->set(false);
      return;
   } else if (rsp->get_accepted()) {
      ctx->set(true);
      return;
   } else if (0 > rsp->get_dst()) {
      LOGWARN("No known leader!");
      ctx->set(false);
      return;
   }

   // Not accepted: means that `get_dst()` is a new leader.
   auto factory = ctx->cli_factory();
   LOGDEBUGMOD(raft_core, "Updating leader from {} to {}", factory->current_leader(), rsp->get_dst());
   factory->update_leader(rsp->get_dst());
   auto client = factory->create_client(std::to_string(rsp->get_dst()));

   // We'll try again by forwarding the message
   auto handler = static_cast<cstn::rpc_handler>([ctx] (shared<cstn::resp_msg>& rsp,
                                                        shared<cstn::rpc_exception>& err) {
         respHandler(ctx, rsp, err);
      });

   auto msg = createMessage(ctx->payload());
   client->send(msg, handler);
}

cstn::ptr<cstn::rpc_client>
grpc_factory::create_client(const std::string &client) {
    // FIXME: GrpcClient currently joins() indefinitely...leak it
    [[maybe_unused]] sds::grpc::GrpcClient* old_client;
    cstn::ptr<cstn::rpc_client> raft_client;

    // Protected section
    { std::lock_guard<std::mutex> lk(_client_lock);
    auto [it, happened] = _clients.emplace(client, nullptr);
    if (_clients.end() == it) return raft_client;

    // Defer destruction of old client if it existed
    if (!happened) {
        LOGDEBUGMOD(raft_core, "Re-creating client for {}", client);
        // FIXME
        old_client =it->second.release();
    }

    auto grpc_client = new sds::grpc::GrpcClient(); // check if grpc client has already run
    grpc_client->run(1);

    if (auto err = create_client(client, &(grpc_client->cq()), raft_client); err) {
        LOGERROR("Failed to create client for {}: {}", client, err.message());
        delete grpc_client;
    } else {
        it->second.reset(grpc_client);
    }
    } // End of Protected section
    return raft_client;
}

std::future<bool>
grpc_factory::add_server(uint32_t const srv_id, shared<grpc_factory> factory) {
   auto client = factory->create_client(std::to_string(factory->current_leader()));
   assert(client);

   auto ctx = std::make_shared<client_ctx<uint32_t>>(srv_id, factory);
   auto handler = static_cast<cstn::rpc_handler>([ctx] (shared<cstn::resp_msg>& rsp,
                                                        shared<cstn::rpc_exception>& err) {
         respHandler(ctx, rsp, err);
      });

   auto msg = createMessage(srv_id);
   client->send(msg, handler);
   return ctx->future();
}

std::future<bool>
grpc_factory::rem_server(uint32_t const srv_id, shared<grpc_factory> factory) {
   auto client = factory->create_client(std::to_string(factory->current_leader()));
   assert(client);

   auto ctx = std::make_shared<client_ctx<uint32_t>>(srv_id, factory);
   auto handler = static_cast<cstn::rpc_handler>([ctx] (shared<cstn::resp_msg>& rsp,
                                                        shared<cstn::rpc_exception>& err) {
         respHandler(ctx, rsp, err);
      });

   auto msg = createMessage(srv_id);
   client->send(msg, handler);
   return ctx->future();
}

std::future<bool>
grpc_factory::client_request(shared<cstn::buffer> buf, shared<grpc_factory> factory) {
   auto client = factory->create_client(std::to_string(factory->current_leader()));
   assert(client);

   auto ctx = std::make_shared<client_ctx<shared<cstn::buffer>>>(buf, factory);
   auto handler = static_cast<cstn::rpc_handler>([ctx] (shared<cstn::resp_msg>& rsp,
                                                        shared<cstn::rpc_exception>& err) {
         respHandler(ctx, rsp, err);
      });

   auto msg = createMessage(buf);
   client->send(msg, handler);
   return ctx->future();
}

}
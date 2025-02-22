/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "clients/storage/InternalStorageClient.h"

#include "common/base/Base.h"

namespace nebula {
namespace storage {

template <typename T>
::nebula::cpp2::ErrorCode getErrorCode(T& tryResp) {
  if (!tryResp.hasValue()) {
    LOG(ERROR) << tryResp.exception().what();
    return nebula::cpp2::ErrorCode::E_RPC_FAILURE;
  }

  auto& stResp = tryResp.value();
  if (!stResp.ok()) {
    switch (stResp.status().code()) {
      case Status::Code::kLeaderChanged:
        return nebula::cpp2::ErrorCode::E_LEADER_CHANGED;
      default:
        LOG(ERROR) << "not impl error transform: code="
                   << static_cast<int32_t>(stResp.status().code());
    }
    return nebula::cpp2::ErrorCode::E_UNKNOWN;
  }

  auto& failedPart = stResp.value().get_result().get_failed_parts();
  for (auto& p : failedPart) {
    return p.code;
  }
  return nebula::cpp2::ErrorCode::SUCCEEDED;
}

void InternalStorageClient::chainUpdateEdge(cpp2::UpdateEdgeRequest& reversedRequest,
                                            TermID termOfSrc,
                                            folly::Optional<int64_t> optVersion,
                                            folly::Promise<::nebula::cpp2::ErrorCode>&& p,
                                            folly::EventBase* evb) {
  auto spaceId = reversedRequest.get_space_id();
  auto partId = reversedRequest.get_part_id();
  auto optLeader = getLeader(spaceId, partId);
  if (!optLeader.ok()) {
    LOG(WARNING) << folly::sformat("failed to get leader, space {}, part {}. ", spaceId, partId)
                 << optLeader.status();
    p.setValue(::nebula::cpp2::ErrorCode::E_SPACE_NOT_FOUND);
    return;
  }
  HostAddr& leader = optLeader.value();
  leader.port += kInternalPortOffset;
  VLOG(1) << "leader host: " << leader;

  cpp2::ChainUpdateEdgeRequest chainReq;
  chainReq.update_edge_request_ref() = reversedRequest;
  chainReq.term_ref() = termOfSrc;
  if (optVersion) {
    chainReq.edge_version_ref() = optVersion.value();
  }
  auto resp = getResponse(
      evb,
      std::make_pair(leader, chainReq),
      [](cpp2::InternalStorageServiceAsyncClient* client, const cpp2::ChainUpdateEdgeRequest& r) {
        return client->future_chainUpdateEdge(r);
      });

  std::move(resp).thenTry([=, p = std::move(p)](auto&& t) mutable {
    auto code = getErrorCode(t);
    if (code == ::nebula::cpp2::ErrorCode::E_LEADER_CHANGED) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      chainUpdateEdge(reversedRequest, termOfSrc, optVersion, std::move(p));
    } else {
      p.setValue(code);
    }
    return;
  });
}

void InternalStorageClient::chainAddEdges(cpp2::AddEdgesRequest& directReq,
                                          TermID termId,
                                          folly::Optional<int64_t> optVersion,
                                          folly::Promise<nebula::cpp2::ErrorCode>&& p,
                                          folly::EventBase* evb) {
  auto spaceId = directReq.get_space_id();
  auto partId = directReq.get_parts().begin()->first;
  auto optLeader = getLeader(directReq.get_space_id(), partId);
  if (!optLeader.ok()) {
    LOG(WARNING) << folly::sformat("failed to get leader, space {}, part {}", spaceId, partId)
                 << optLeader.status();
    p.setValue(::nebula::cpp2::ErrorCode::E_SPACE_NOT_FOUND);
    return;
  }
  HostAddr& leader = optLeader.value();
  leader.port += kInternalPortOffset;
  VLOG(2) << "leader host: " << leader;

  cpp2::ChainAddEdgesRequest chainReq = makeChainAddReq(directReq, termId, optVersion);
  auto resp = getResponse(
      evb,
      std::make_pair(leader, chainReq),
      [](cpp2::InternalStorageServiceAsyncClient* client, const cpp2::ChainAddEdgesRequest& r) {
        return client->future_chainAddEdges(r);
      });

  std::move(resp).thenTry([=, p = std::move(p)](auto&& t) mutable {
    auto code = getErrorCode(t);
    if (code == nebula::cpp2::ErrorCode::E_LEADER_CHANGED) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      chainAddEdges(directReq, termId, optVersion, std::move(p));
    } else {
      p.setValue(code);
    }
    return;
  });
}

cpp2::ChainAddEdgesRequest InternalStorageClient::makeChainAddReq(const cpp2::AddEdgesRequest& req,
                                                                  TermID termId,
                                                                  folly::Optional<int64_t> ver) {
  cpp2::ChainAddEdgesRequest ret;
  ret.space_id_ref() = req.get_space_id();
  ret.parts_ref() = req.get_parts();
  ret.prop_names_ref() = req.get_prop_names();
  ret.if_not_exists_ref() = req.get_if_not_exists();
  ret.term_ref() = termId;
  if (ver) {
    ret.edge_version_ref() = ver.value();
  }
  return ret;
}

void InternalStorageClient::chainDeleteEdges(cpp2::DeleteEdgesRequest& req,
                                             const std::string& txnId,
                                             TermID termId,
                                             folly::Promise<nebula::cpp2::ErrorCode>&& p,
                                             folly::EventBase* evb) {
  auto spaceId = req.get_space_id();
  auto partId = req.get_parts().begin()->first;
  auto optLeader = getLeader(req.get_space_id(), partId);
  if (!optLeader.ok()) {
    LOG(WARNING) << folly::sformat("failed to get leader, space {}, part {}", spaceId, partId)
                 << optLeader.status();
    p.setValue(::nebula::cpp2::ErrorCode::E_SPACE_NOT_FOUND);
    return;
  }
  HostAddr& leader = optLeader.value();
  leader.port += kInternalPortOffset;
  VLOG(2) << "leader host: " << leader;

  cpp2::ChainDeleteEdgesRequest chainReq;
  chainReq.space_id_ref() = req.get_space_id();
  chainReq.parts_ref() = req.get_parts();
  chainReq.txn_id_ref() = txnId;
  chainReq.term_ref() = termId;
  auto resp = getResponse(
      evb,
      std::make_pair(leader, chainReq),
      [](cpp2::InternalStorageServiceAsyncClient* client, const cpp2::ChainDeleteEdgesRequest& r) {
        return client->future_chainDeleteEdges(r);
      });

  std::move(resp).thenTry([=, p = std::move(p)](auto&& t) mutable {
    auto code = getErrorCode(t);
    if (code == nebula::cpp2::ErrorCode::E_LEADER_CHANGED) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      chainDeleteEdges(req, txnId, termId, std::move(p));
    } else {
      p.setValue(code);
    }
    return;
  });
}

}  // namespace storage
}  // namespace nebula

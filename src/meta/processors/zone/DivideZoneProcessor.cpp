/* Copyright (c) 2021 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/processors/zone/DivideZoneProcessor.h"

namespace nebula {
namespace meta {

void DivideZoneProcessor::process(const cpp2::DivideZoneReq& req) {
  folly::SharedMutex::WriteHolder zHolder(LockUtils::zoneLock());
  folly::SharedMutex::WriteHolder sHolder(LockUtils::spaceLock());
  auto zoneName = req.get_zone_name();
  auto zoneKey = MetaKeyUtils::zoneKey(zoneName);
  auto zoneValueRet = doGet(zoneKey);
  if (!nebula::ok(zoneValueRet)) {
    LOG(ERROR) << "Zone " << zoneName << " not existed error: "
               << apache::thrift::util::enumNameSafe(nebula::cpp2::ErrorCode::E_ZONE_NOT_FOUND);
    handleErrorCode(nebula::cpp2::ErrorCode::E_ZONE_NOT_FOUND);
    onFinished();
    return;
  }

  auto& zoneItems = req.get_zone_items();
  auto zoneHosts = MetaKeyUtils::parseZoneHosts(std::move(nebula::value(zoneValueRet)));
  if (zoneItems.size() > zoneHosts.size()) {
    LOG(ERROR) << "Zone Item should not greater than hosts size";
    handleErrorCode(nebula::cpp2::ErrorCode::E_INVALID_PARM);
    onFinished();
    return;
  }

  std::vector<std::string> zoneNames;
  std::unordered_set<HostAddr> totalHosts;
  auto batchHolder = std::make_unique<kvstore::BatchHolder>();
  nebula::cpp2::ErrorCode code = nebula::cpp2::ErrorCode::SUCCEEDED;
  for (auto iter = zoneItems.begin(); iter != zoneItems.end(); iter++) {
    auto zone = iter->first;
    auto hosts = iter->second;
    auto valueRet = doGet(MetaKeyUtils::zoneKey(zone));
    if (nebula::ok(valueRet)) {
      LOG(ERROR) << "Zone " << zone << " have existed";
      code = nebula::cpp2::ErrorCode::E_EXISTED;
      break;
    }

    auto it = std::find(zoneNames.begin(), zoneNames.end(), zone);
    if (it == zoneNames.end()) {
      LOG(ERROR) << "Zone have duplicated name";
      zoneNames.emplace_back(zone);
    } else {
      code = nebula::cpp2::ErrorCode::E_INVALID_PARM;
      break;
    }

    if (hosts.empty()) {
      LOG(ERROR) << "Hosts should not be empty";
      code = nebula::cpp2::ErrorCode::E_INVALID_PARM;
    }

    if (std::unique(hosts.begin(), hosts.end()) != hosts.end()) {
      LOG(ERROR) << "Zone have duplicated host";
      code = nebula::cpp2::ErrorCode::E_INVALID_PARM;
      break;
    }

    std::copy(hosts.begin(), hosts.end(), std::inserter(totalHosts, totalHosts.end()));

    auto key = MetaKeyUtils::zoneKey(std::move(zone));
    auto val = MetaKeyUtils::zoneVal(std::move(hosts));
    batchHolder->put(std::move(key), std::move(val));
  }

  if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
    handleErrorCode(code);
    onFinished();
    return;
  }

  if (totalHosts.size() != zoneHosts.size()) {
    LOG(ERROR) << "The total host is not all hosts";
    handleErrorCode(nebula::cpp2::ErrorCode::E_INVALID_PARM);
    onFinished();
    return;
  }

  for (auto& host : totalHosts) {
    auto iter = std::find(zoneHosts.begin(), zoneHosts.end(), host);
    if (iter == zoneHosts.end()) {
      LOG(ERROR) << "Host " << host << " not exist in original zone";
      code = nebula::cpp2::ErrorCode::E_INVALID_PARM;
      break;
    }
  }

  if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
    handleErrorCode(code);
    onFinished();
    return;
  }

  // Remove original zone
  batchHolder->remove(std::move(zoneKey));
  code = updateSpacesZone(batchHolder.get(), zoneName, zoneNames);
  if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
    handleErrorCode(code);
    onFinished();
    return;
  }

  auto batch = encodeBatchValue(std::move(batchHolder)->getBatch());
  doBatchOperation(std::move(batch));
}

nebula::cpp2::ErrorCode DivideZoneProcessor::updateSpacesZone(
    kvstore::BatchHolder* batchHolder,
    const std::string& originalZoneName,
    const std::vector<std::string>& zoneNames) {
  const auto& prefix = MetaKeyUtils::spacePrefix();
  auto ret = doPrefix(prefix);

  if (!nebula::ok(ret)) {
    LOG(ERROR) << "List spaces failed";
    return nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND;
  }

  auto iter = nebula::value(ret).get();
  while (iter->valid()) {
    auto id = MetaKeyUtils::spaceId(iter->key());
    auto properties = MetaKeyUtils::parseSpace(iter->val());
    auto zones = properties.get_zone_names();

    auto it = std::find(zones.begin(), zones.end(), originalZoneName);
    if (it != zones.end()) {
      zones.erase(it);
      for (auto& zone : zoneNames) {
        zones.emplace_back(zone);
      }

      properties.zone_names_ref() = zones;
      auto spaceKey = MetaKeyUtils::spaceKey(id);
      auto spaceVal = MetaKeyUtils::spaceVal(properties);
      batchHolder->put(std::move(spaceKey), std::move(spaceVal));
    }
    iter->next();
  }
  return nebula::cpp2::ErrorCode::SUCCEEDED;
}

}  // namespace meta
}  // namespace nebula

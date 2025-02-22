/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/upgrade/MetaDataUpgrade.h"

#include <thrift/lib/cpp/util/EnumUtils.h>

#include "common/base/Base.h"
#include "common/base/ObjectPool.h"
#include "common/conf/Configuration.h"
#include "common/datatypes/Map.h"
#include "common/datatypes/Value.h"
#include "common/expression/ConstantExpression.h"
#include "common/utils/MetaKeyUtils.h"
#include "interface/gen-cpp2/meta_types.h"
#include "kvstore/Common.h"
#include "meta/ActiveHostsMan.h"
#include "meta/MetaServiceUtils.h"
#include "meta/upgrade/v1/MetaServiceUtilsV1.h"
#include "meta/upgrade/v2/MetaServiceUtilsV2.h"

DECLARE_bool(null_type);
DECLARE_uint32(string_index_limit);

namespace nebula {
namespace meta {

Status MetaDataUpgrade::rewriteHosts(const folly::StringPiece &key, const folly::StringPiece &val) {
  auto host = meta::v1::MetaServiceUtilsV1::parseHostKey(key);
  auto info = HostInfo::decodeV1(val);
  auto newVal = HostInfo::encodeV2(info);
  auto newKey =
      MetaKeyUtils::hostKeyV2(network::NetworkUtils::intToIPv4(host.get_ip()), host.get_port());
  NG_LOG_AND_RETURN_IF_ERROR(put(newKey, newVal));
  NG_LOG_AND_RETURN_IF_ERROR(remove(key));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteLeaders(const folly::StringPiece &key,
                                       const folly::StringPiece &val) {
  auto host = meta::v1::MetaServiceUtilsV1::parseLeaderKey(key);
  auto newKey =
      MetaKeyUtils::leaderKey(network::NetworkUtils::intToIPv4(host.get_ip()), host.get_port());
  NG_LOG_AND_RETURN_IF_ERROR(put(newKey, val));
  NG_LOG_AND_RETURN_IF_ERROR(remove(key));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteSpaces(const folly::StringPiece &key,
                                      const folly::StringPiece &val) {
  auto oldProps = meta::v1::MetaServiceUtilsV1::parseSpace(val);
  cpp2::SpaceDesc spaceDesc;
  spaceDesc.space_name_ref() = oldProps.get_space_name();
  spaceDesc.partition_num_ref() = oldProps.get_partition_num();
  spaceDesc.replica_factor_ref() = oldProps.get_replica_factor();
  spaceDesc.charset_name_ref() = oldProps.get_charset_name();
  spaceDesc.collate_name_ref() = oldProps.get_collate_name();
  (*spaceDesc.vid_type_ref()).type_length_ref() = 8;
  (*spaceDesc.vid_type_ref()).type_ref() = nebula::cpp2::PropertyType::INT64;
  NG_LOG_AND_RETURN_IF_ERROR(put(key, MetaKeyUtils::spaceVal(spaceDesc)));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteSpacesV2ToV3(const folly::StringPiece &key,
                                            const folly::StringPiece &val) {
  auto oldProps = meta::v2::MetaServiceUtilsV2::parseSpace(val);
  cpp2::SpaceDesc spaceDesc;
  spaceDesc.space_name_ref() = oldProps.get_space_name();
  spaceDesc.partition_num_ref() = oldProps.get_partition_num();
  spaceDesc.replica_factor_ref() = oldProps.get_replica_factor();
  spaceDesc.charset_name_ref() = oldProps.get_charset_name();
  spaceDesc.collate_name_ref() = oldProps.get_collate_name();
  cpp2::ColumnTypeDef def;
  auto &type = oldProps.get_vid_type();
  def.type_length_ref() = *type.get_type_length();
  def.type_ref() = convertToPropertyType(type.get_type());

  if (type.geo_shape_ref().has_value()) {
    def.geo_shape_ref() = convertToGeoShape(*type.get_geo_shape());
  }
  spaceDesc.vid_type_ref() = std::move(def);
  if (oldProps.isolation_level_ref().has_value()) {
    if (*oldProps.isolation_level_ref() == nebula::meta::v2::cpp2::IsolationLevel::DEFAULT) {
      spaceDesc.isolation_level_ref() = nebula::meta::cpp2::IsolationLevel::DEFAULT;
    } else {
      spaceDesc.isolation_level_ref() = nebula::meta::cpp2::IsolationLevel::TOSS;
    }
  }

  if (oldProps.comment_ref().has_value()) {
    spaceDesc.comment_ref() = *oldProps.comment_ref();
  }

  if (oldProps.group_name_ref().has_value()) {
    auto groupName = *oldProps.group_name_ref();
    auto groupKey = meta::v2::MetaServiceUtilsV2::groupKey(groupName);
    std::string zoneValue;
    auto code = kv_->get(kDefaultSpaceId, kDefaultPartId, std::move(groupKey), &zoneValue);
    if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
      return Status::Error("Get Group Failed");
    }

    auto zones = meta::v2::MetaServiceUtilsV2::parseZoneNames(std::move(zoneValue));
    spaceDesc.zone_names_ref() = std::move(zones);
  } else {
    const auto &zonePrefix = MetaKeyUtils::zonePrefix();
    std::unique_ptr<kvstore::KVIterator> iter;
    auto code = kv_->prefix(kDefaultSpaceId, kDefaultPartId, zonePrefix, &iter);
    if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
      return Status::Error("Get Zones Failed");
    }

    std::vector<::std::string> zones;
    while (iter->valid()) {
      auto zoneName = MetaKeyUtils::parseZoneName(iter->key());
      zones.emplace_back(std::move(zoneName));
      iter->next();
    }
    spaceDesc.zone_names_ref() = std::move(zones);
  }

  NG_LOG_AND_RETURN_IF_ERROR(put(key, MetaKeyUtils::spaceVal(spaceDesc)));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteParts(const folly::StringPiece &key, const folly::StringPiece &val) {
  auto oldHosts = meta::v1::MetaServiceUtilsV1::parsePartVal(val);
  std::vector<HostAddr> newHosts;
  for (auto &host : oldHosts) {
    HostAddr hostAddr;
    hostAddr.host = network::NetworkUtils::intToIPv4(host.get_ip());
    hostAddr.port = host.get_port();
    newHosts.emplace_back(std::move(hostAddr));
  }
  NG_LOG_AND_RETURN_IF_ERROR(put(key, MetaKeyUtils::partVal(newHosts)));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteSchemas(const folly::StringPiece &key,
                                       const folly::StringPiece &val) {
  auto oldSchema = meta::v1::MetaServiceUtilsV1::parseSchema(val);
  cpp2::Schema newSchema;
  cpp2::SchemaProp newSchemaProps;
  auto &schemaProp = oldSchema.get_schema_prop();
  if (schemaProp.ttl_duration_ref().has_value()) {
    newSchemaProps.ttl_duration_ref() = *schemaProp.get_ttl_duration();
  }
  if (schemaProp.ttl_col_ref().has_value()) {
    newSchemaProps.ttl_col_ref() = *schemaProp.get_ttl_col();
  }
  newSchema.schema_prop_ref() = std::move(newSchemaProps);
  NG_LOG_AND_RETURN_IF_ERROR(
      convertToNewColumns((*oldSchema.columns_ref()), (*newSchema.columns_ref())));

  auto nameLen = *reinterpret_cast<const int32_t *>(val.data());
  auto schemaName = val.subpiece(sizeof(int32_t), nameLen).str();
  auto encodeVal = MetaKeyUtils::schemaVal(schemaName, newSchema);
  NG_LOG_AND_RETURN_IF_ERROR(put(key, encodeVal));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteIndexes(const folly::StringPiece &key,
                                       const folly::StringPiece &val) {
  auto oldItem = meta::v1::MetaServiceUtilsV1::parseIndex(val);
  cpp2::IndexItem newItem;
  newItem.index_id_ref() = oldItem.get_index_id();
  newItem.index_name_ref() = oldItem.get_index_name();
  nebula::cpp2::SchemaID schemaId;
  if (oldItem.get_schema_id().getType() == meta::v1::cpp2::SchemaID::Type::tag_id) {
    schemaId.tag_id_ref() = oldItem.get_schema_id().get_tag_id();
  } else {
    schemaId.edge_type_ref() = oldItem.get_schema_id().get_edge_type();
  }
  newItem.schema_id_ref() = schemaId;
  NG_LOG_AND_RETURN_IF_ERROR(
      convertToNewIndexColumns((*oldItem.fields_ref()), (*newItem.fields_ref())));
  NG_LOG_AND_RETURN_IF_ERROR(put(key, MetaKeyUtils::indexVal(newItem)));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteConfigs(const folly::StringPiece &key,
                                       const folly::StringPiece &val) {
  auto item = meta::v1::MetaServiceUtilsV1::parseConfigValue(val);

  Value configVal;
  switch (item.get_type()) {
    case meta::v1::cpp2::ConfigType::INT64: {
      auto value = *reinterpret_cast<const int64_t *>(item.get_value().data());
      configVal.setInt(boost::get<int64_t>(value));
      break;
    }
    case meta::v1::cpp2::ConfigType::DOUBLE: {
      auto value = *reinterpret_cast<const double *>(item.get_value().data());
      configVal.setFloat(boost::get<double>(value));
      break;
    }
    case meta::v1::cpp2::ConfigType::BOOL: {
      auto value = *reinterpret_cast<const bool *>(item.get_value().data());
      configVal.setBool(boost::get<bool>(value) ? "True" : "False");
      break;
    }
    case meta::v1::cpp2::ConfigType::STRING: {
      configVal.setStr(boost::get<std::string>(item.get_value()));
      break;
    }
    case meta::v1::cpp2::ConfigType::NESTED: {
      auto value = item.get_value();
      // transform to map value
      conf::Configuration conf;
      auto status = conf.parseFromString(boost::get<std::string>(value));
      if (!status.ok()) {
        LOG(ERROR) << "Parse value: " << value << " failed: " << status;
        return Status::Error("Parse value: %s failed", value.c_str());
      }
      Map map;
      conf.forEachItem([&map](const folly::StringPiece &confKey, const folly::dynamic &confVal) {
        map.kvs.emplace(confKey, confVal.asString());
      });
      configVal.setMap(std::move(map));
      break;
    }
  }
  auto newVal =
      MetaKeyUtils::configValue(static_cast<cpp2::ConfigMode>(item.get_mode()), configVal);
  NG_LOG_AND_RETURN_IF_ERROR(put(key, newVal));
  return Status::OK();
}

Status MetaDataUpgrade::rewriteJobDesc(const folly::StringPiece &key,
                                       const folly::StringPiece &val) {
  auto jobDesc = meta::v1::MetaServiceUtilsV1::parseJobDesc(val);
  auto cmdStr = std::get<0>(jobDesc);
  nebula::meta::cpp2::AdminCmd adminCmd;
  if (cmdStr.find("flush") == 0) {
    adminCmd = nebula::meta::cpp2::AdminCmd::FLUSH;
  } else if (cmdStr.find("compact") == 0) {
    adminCmd = nebula::meta::cpp2::AdminCmd::COMPACT;
  } else {
    return Status::Error("Wrong job cmd: %s", cmdStr.c_str());
  }
  auto paras = std::get<1>(jobDesc);
  auto status = std::get<2>(jobDesc);
  auto startTime = std::get<3>(jobDesc);
  auto stopTime = std::get<4>(jobDesc);
  std::string str;
  str.reserve(256);
  // use a big num to avoid possible conflict
  int32_t dataVersion = INT_MAX - 1;
  str.append(reinterpret_cast<const char *>(&dataVersion), sizeof(dataVersion))
      .append(reinterpret_cast<const char *>(&adminCmd), sizeof(adminCmd));
  auto paraSize = paras.size();
  str.append(reinterpret_cast<const char *>(&paraSize), sizeof(size_t));
  for (auto &para : paras) {
    auto len = para.length();
    str.append(reinterpret_cast<const char *>(&len), sizeof(len))
        .append(reinterpret_cast<const char *>(&para[0]), len);
  }
  str.append(reinterpret_cast<const char *>(&status), sizeof(nebula::meta::cpp2::JobStatus))
      .append(reinterpret_cast<const char *>(&startTime), sizeof(int64_t))
      .append(reinterpret_cast<const char *>(&stopTime), sizeof(int64_t));

  NG_LOG_AND_RETURN_IF_ERROR(put(key, str));
  return Status::OK();
}

Status MetaDataUpgrade::deleteKeyVal(const folly::StringPiece &key) {
  NG_LOG_AND_RETURN_IF_ERROR(remove(key));
  return Status::OK();
}

Status MetaDataUpgrade::convertToNewColumns(const std::vector<meta::v1::cpp2::ColumnDef> &oldCols,
                                            std::vector<cpp2::ColumnDef> &newCols) {
  ObjectPool objPool;
  auto pool = &objPool;
  for (auto &colDef : oldCols) {
    cpp2::ColumnDef columnDef;
    columnDef.name_ref() = colDef.get_name();
    columnDef.type.type_ref() =
        static_cast<nebula::cpp2::PropertyType>(colDef.get_type().get_type());
    if (colDef.default_value_ref().has_value()) {
      std::string encodeStr;
      switch (colDef.get_type().get_type()) {
        case meta::v1::cpp2::SupportedType::BOOL:
          encodeStr = Expression::encode(
              *ConstantExpression::make(pool, colDef.get_default_value()->get_bool_value()));
          break;
        case meta::v1::cpp2::SupportedType::INT:
          encodeStr = Expression::encode(
              *ConstantExpression::make(pool, colDef.get_default_value()->get_int_value()));
          break;
        case meta::v1::cpp2::SupportedType::DOUBLE:
          encodeStr = Expression::encode(
              *ConstantExpression::make(pool, colDef.get_default_value()->get_double_value()));
          break;
        case meta::v1::cpp2::SupportedType::STRING:
          encodeStr = Expression::encode(
              *ConstantExpression::make(pool, colDef.get_default_value()->get_string_value()));
          break;
        case meta::v1::cpp2::SupportedType::TIMESTAMP:
          encodeStr = Expression::encode(
              *ConstantExpression::make(pool, colDef.get_default_value()->get_timestamp()));
          break;
        default:
          return Status::Error(
              "Wrong default type: %s",
              apache::thrift::util::enumNameSafe(colDef.get_type().get_type()).c_str());
      }

      columnDef.default_value_ref() = std::move(encodeStr);
    }
    if (FLAGS_null_type) {
      columnDef.nullable_ref() = true;
    }
    newCols.emplace_back(std::move(columnDef));
  }
  return Status::OK();
}

Status MetaDataUpgrade::convertToNewIndexColumns(
    const std::vector<meta::v1::cpp2::ColumnDef> &oldCols, std::vector<cpp2::ColumnDef> &newCols) {
  for (auto &colDef : oldCols) {
    cpp2::ColumnDef columnDef;
    columnDef.name_ref() = colDef.get_name();
    if (colDef.get_type().get_type() == meta::v1::cpp2::SupportedType::STRING) {
      cpp2::ColumnTypeDef type;
      type.type_ref() = nebula::cpp2::PropertyType::FIXED_STRING;
      type.type_length_ref() = FLAGS_string_index_limit;
      columnDef.type_ref() = std::move(type);
    } else {
      columnDef.type.type_ref() =
          static_cast<nebula::cpp2::PropertyType>(colDef.get_type().get_type());
    }
    DCHECK(!colDef.default_value_ref().has_value());
    if (FLAGS_null_type) {
      columnDef.nullable_ref() = true;
    }
    newCols.emplace_back(std::move(columnDef));
  }
  return Status::OK();
}

nebula::cpp2::PropertyType MetaDataUpgrade::convertToPropertyType(
    nebula::meta::v2::cpp2::PropertyType type) {
  switch (type) {
    case nebula::meta::v2::cpp2::PropertyType::BOOL:
      return nebula::cpp2::PropertyType::BOOL;
    case nebula::meta::v2::cpp2::PropertyType::INT64:
      return nebula::cpp2::PropertyType::INT64;
    case nebula::meta::v2::cpp2::PropertyType::VID:
      return nebula::cpp2::PropertyType::VID;
    case nebula::meta::v2::cpp2::PropertyType::FLOAT:
      return nebula::cpp2::PropertyType::FLOAT;
    case nebula::meta::v2::cpp2::PropertyType::DOUBLE:
      return nebula::cpp2::PropertyType::DOUBLE;
    case nebula::meta::v2::cpp2::PropertyType::STRING:
      return nebula::cpp2::PropertyType::STRING;
    case nebula::meta::v2::cpp2::PropertyType::FIXED_STRING:
      return nebula::cpp2::PropertyType::FIXED_STRING;
    case nebula::meta::v2::cpp2::PropertyType::INT8:
      return nebula::cpp2::PropertyType::INT8;
    case nebula::meta::v2::cpp2::PropertyType::INT16:
      return nebula::cpp2::PropertyType::INT16;
    case nebula::meta::v2::cpp2::PropertyType::INT32:
      return nebula::cpp2::PropertyType::INT32;
    case nebula::meta::v2::cpp2::PropertyType::TIMESTAMP:
      return nebula::cpp2::PropertyType::TIMESTAMP;
    case nebula::meta::v2::cpp2::PropertyType::DATE:
      return nebula::cpp2::PropertyType::DATE;
    case nebula::meta::v2::cpp2::PropertyType::DATETIME:
      return nebula::cpp2::PropertyType::DATETIME;
    case nebula::meta::v2::cpp2::PropertyType::TIME:
      return nebula::cpp2::PropertyType::TIME;
    case nebula::meta::v2::cpp2::PropertyType::GEOGRAPHY:
      return nebula::cpp2::PropertyType::GEOGRAPHY;
    default:
      return nebula::cpp2::PropertyType::UNKNOWN;
  }
}

nebula::meta::cpp2::GeoShape MetaDataUpgrade::convertToGeoShape(
    nebula::meta::v2::cpp2::GeoShape shape) {
  switch (shape) {
    case nebula::meta::v2::cpp2::GeoShape::ANY:
      return nebula::meta::cpp2::GeoShape::ANY;
    case nebula::meta::v2::cpp2::GeoShape::POINT:
      return nebula::meta::cpp2::GeoShape::POINT;
    case nebula::meta::v2::cpp2::GeoShape::LINESTRING:
      return nebula::meta::cpp2::GeoShape::LINESTRING;
    case nebula::meta::v2::cpp2::GeoShape::POLYGON:
      return nebula::meta::cpp2::GeoShape::POLYGON;
    default:
      LOG(FATAL) << "Unimplemented";
  }
}

void MetaDataUpgrade::printHost(const folly::StringPiece &key, const folly::StringPiece &val) {
  auto host = meta::v1::MetaServiceUtilsV1::parseHostKey(key);
  auto info = HostInfo::decodeV1(val);
  LOG(INFO) << "Host ip: " << network::NetworkUtils::intToIPv4(host.get_ip());
  LOG(INFO) << "Host port: " << host.get_port();
  LOG(INFO) << "Host info: lastHBTimeInMilliSec: " << info.lastHBTimeInMilliSec_;
  LOG(INFO) << "Host info: role_: " << apache::thrift::util::enumNameSafe(info.role_);
  LOG(INFO) << "Host info: gitInfoSha_: " << info.gitInfoSha_;
}

void MetaDataUpgrade::printSpacesV1(const folly::StringPiece &val) {
  auto oldProps = meta::v1::MetaServiceUtilsV1::parseSpace(val);
  LOG(INFO) << "Space name: " << oldProps.get_space_name();
  LOG(INFO) << "Partition num: " << oldProps.get_partition_num();
  LOG(INFO) << "Replica factor: " << oldProps.get_replica_factor();
  LOG(INFO) << "Charset name: " << oldProps.get_charset_name();
  LOG(INFO) << "Collate name: " << oldProps.get_collate_name();
}

void MetaDataUpgrade::printSpacesV2(const folly::StringPiece &val) {
  auto oldProps = meta::v2::MetaServiceUtilsV2::parseSpace(val);
  LOG(INFO) << "Space name: " << oldProps.get_space_name();
  LOG(INFO) << "Partition num: " << oldProps.get_partition_num();
  LOG(INFO) << "Replica factor: " << oldProps.get_replica_factor();
  LOG(INFO) << "Charset name: " << oldProps.get_charset_name();
  LOG(INFO) << "Collate name: " << oldProps.get_collate_name();
  if (oldProps.group_name_ref().has_value()) {
    LOG(INFO) << "Group name: " << *oldProps.group_name_ref();
  }
}

void MetaDataUpgrade::printParts(const folly::StringPiece &key, const folly::StringPiece &val) {
  auto spaceId = meta::v1::MetaServiceUtilsV1::parsePartKeySpaceId(key);
  auto partId = meta::v1::MetaServiceUtilsV1::parsePartKeyPartId(key);
  auto oldHosts = meta::v1::MetaServiceUtilsV1::parsePartVal(val);
  LOG(INFO) << "Part spaceId: " << spaceId;
  LOG(INFO) << "Part      id: " << partId;
  for (auto &host : oldHosts) {
    LOG(INFO) << "Part host   ip: " << network::NetworkUtils::intToIPv4(host.get_ip());
    LOG(INFO) << "Part host port: " << host.get_port();
  }
}

void MetaDataUpgrade::printLeaders(const folly::StringPiece &key) {
  auto host = meta::v1::MetaServiceUtilsV1::parseLeaderKey(key);
  LOG(INFO) << "Leader host ip: " << network::NetworkUtils::intToIPv4(host.get_ip());
  LOG(INFO) << "Leader host port: " << host.get_port();
}

void MetaDataUpgrade::printSchemas(const folly::StringPiece &val) {
  auto oldSchema = meta::v1::MetaServiceUtilsV1::parseSchema(val);
  auto nameLen = *reinterpret_cast<const int32_t *>(val.data());
  auto schemaName = val.subpiece(sizeof(int32_t), nameLen).str();
  LOG(INFO) << "Schema name: " << schemaName;
  for (auto &colDef : oldSchema.get_columns()) {
    LOG(INFO) << "Schema column name: " << colDef.get_name();
    LOG(INFO) << "Schema column type: "
              << apache::thrift::util::enumNameSafe(colDef.get_type().get_type());
    Value defaultValue;
    if (colDef.default_value_ref().has_value()) {
      switch (colDef.get_type().get_type()) {
        case meta::v1::cpp2::SupportedType::BOOL:
          defaultValue = colDef.get_default_value()->get_bool_value();
          break;
        case meta::v1::cpp2::SupportedType::INT:
          defaultValue = colDef.get_default_value()->get_int_value();
          break;
        case meta::v1::cpp2::SupportedType::DOUBLE:
          defaultValue = colDef.get_default_value()->get_double_value();
          break;
        case meta::v1::cpp2::SupportedType::STRING:
          defaultValue = colDef.get_default_value()->get_string_value();
          break;
        case meta::v1::cpp2::SupportedType::TIMESTAMP:
          defaultValue = colDef.get_default_value()->get_timestamp();
          break;
        default:
          LOG(ERROR) << "Wrong default type: "
                     << apache::thrift::util::enumNameSafe(colDef.get_type().get_type());
      }
      LOG(INFO) << "Schema default value: " << defaultValue;
    }
  }
}

void MetaDataUpgrade::printIndexes(const folly::StringPiece &val) {
  auto oldItem = meta::v1::MetaServiceUtilsV1::parseIndex(val);
  LOG(INFO) << "Index   id: " << oldItem.get_index_id();
  LOG(INFO) << "Index name: " << oldItem.get_index_name();
  if (oldItem.get_schema_id().getType() == meta::v1::cpp2::SchemaID::Type::tag_id) {
    LOG(INFO) << "Index on tag id: " << oldItem.get_schema_id().get_tag_id();
  } else {
    LOG(INFO) << "Index on edgetype: " << oldItem.get_schema_id().get_edge_type();
  }
  for (auto &colDef : oldItem.get_fields()) {
    LOG(INFO) << "Index field name: " << colDef.get_name();
    LOG(INFO) << "Index field type: "
              << apache::thrift::util::enumNameSafe(colDef.get_type().get_type());
  }
}

void MetaDataUpgrade::printConfigs(const folly::StringPiece &key, const folly::StringPiece &val) {
  auto item = meta::v1::MetaServiceUtilsV1::parseConfigValue(val);
  auto configName = meta::v1::MetaServiceUtilsV1::parseConfigKey(key);
  Value configVal;
  switch (item.get_type()) {
    case meta::v1::cpp2::ConfigType::INT64: {
      auto value = *reinterpret_cast<const int64_t *>(item.get_value().data());
      configVal.setInt(boost::get<int64_t>(value));
      break;
    }
    case meta::v1::cpp2::ConfigType::DOUBLE: {
      auto value = *reinterpret_cast<const double *>(item.get_value().data());
      configVal.setFloat(boost::get<double>(value));
      break;
    }
    case meta::v1::cpp2::ConfigType::BOOL: {
      auto value = *reinterpret_cast<const bool *>(item.get_value().data());
      configVal.setBool(boost::get<bool>(value) ? "True" : "False");
      break;
    }
    case meta::v1::cpp2::ConfigType::STRING: {
      configVal.setStr(boost::get<std::string>(item.get_value()));
      break;
    }
    case meta::v1::cpp2::ConfigType::NESTED: {
      auto value = item.get_value();
      // transform to map value
      conf::Configuration conf;
      auto status = conf.parseFromString(boost::get<std::string>(value));
      if (!status.ok()) {
        LOG(ERROR) << "Parse value: " << value << " failed: " << status;
        return;
      }
      Map map;
      conf.forEachItem([&map](const folly::StringPiece &confKey, const folly::dynamic &confVal) {
        map.kvs.emplace(confKey, confVal.asString());
      });
      configVal.setMap(std::move(map));
      break;
    }
  }
  LOG(INFO) << "Config   name: " << configName.second;
  LOG(INFO) << "Config module: " << apache::thrift::util::enumNameSafe(configName.first);
  LOG(INFO) << "Config   mode: " << apache::thrift::util::enumNameSafe(item.get_mode());
  LOG(INFO) << "Config  value: " << configVal;
}

void MetaDataUpgrade::printJobDesc(const folly::StringPiece &key, const folly::StringPiece &val) {
  auto jobId = meta::v1::MetaServiceUtilsV1::parseJobId(key);
  LOG(INFO) << "JobDesc id: " << jobId;
  auto jobDesc = meta::v1::MetaServiceUtilsV1::parseJobDesc(val);
  auto cmdStr = std::get<0>(jobDesc);
  auto paras = std::get<1>(jobDesc);
  auto status = std::get<2>(jobDesc);
  auto startTime = std::get<3>(jobDesc);
  auto stopTime = std::get<4>(jobDesc);

  LOG(INFO) << "JobDesc id: " << jobId;
  LOG(INFO) << "JobDesc cmd: " << cmdStr;
  for (auto &para : paras) {
    LOG(INFO) << "JobDesc para: " << para;
  }
  LOG(INFO) << "JobDesc status: " << apache::thrift::util::enumNameSafe(status);
  LOG(INFO) << "JobDesc startTime: " << startTime;
  LOG(INFO) << "JobDesc stopTime: " << stopTime;
}

Status MetaDataUpgrade::saveMachineAndZone(std::vector<kvstore::KV> data) {
  NG_LOG_AND_RETURN_IF_ERROR(put(data));
  return Status::OK();
}

}  // namespace meta
}  // namespace nebula

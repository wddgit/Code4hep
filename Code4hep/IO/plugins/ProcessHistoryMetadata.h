#ifndef Code4hep_IO_ProcessHistoryMetadata_h
#define Code4hep_IO_ProcessHistoryMetadata_h

#include "DataFormats/Provenance/interface/ProcessHistoryRegistry.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "podio/Frame.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace c4h {
  inline constexpr char processHistoriesVersionKey[] = "Code4hep.edm.ProcessHistories.version";
  inline constexpr char processHistoriesIDsKey[] = "Code4hep.edm.ProcessHistories.ids";
  inline constexpr char processHistoriesPrefix[] = "Code4hep.edm.ProcessHistories.";
  inline constexpr char eventProcessHistoryKey[] = "Code4hep.edm.Event.processHistoryID";
  inline constexpr char runProcessHistoryKey[] = "Code4hep.edm.Event.runProcessHistoryID";
  inline constexpr char lumiProcessHistoryKey[] = "Code4hep.edm.Event.lumiProcessHistoryID";
  inline constexpr char luminosityBlockKey[] = "Code4hep.edm.Event.luminosityBlock";

  inline std::string historyIDString(edm::ProcessHistoryID const& id) {
    std::string result;
    // Framework auxiliaries can use the default ID for an empty history.
    (id.isValid() ? id : edm::ProcessHistory{}.id()).toString(result);
    return result;
  }

  namespace processHistoryMetadataDetail {
    inline bool isHexID(std::string const& id) {
      return id.size() == 32 && id.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    }

    inline void validateHardware(std::string const& serialized) {
      try {
        if (edm::HardwareResourcesDescription(serialized).serialize() != serialized) {
          throw edm::Exception(edm::errors::FormatIncompatibility)
              << "Non-round-trippable ProcessHistory hardware metadata";
        }
      } catch (edm::Exception const& ex) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << ex.message();
      }
    }
  }  // namespace processHistoryMetadataDetail

  inline void putProcessHistories(podio::Frame& metadata,
                                 std::map<edm::ProcessHistoryID, edm::ProcessHistory> const& histories) {
    std::vector<std::string> ids;
    std::map<std::string, std::vector<std::string>> encoded;
    ids.reserve(histories.size());
    for (auto const& [id, history] : histories) {
      auto recomputed = history;
      recomputed.initializeTransients();
      auto const idString = historyIDString(id);
      if (recomputed.id() != edm::ProcessHistoryID(idString)) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "ProcessHistory ID mismatch: " << idString;
      }
      auto [entry, inserted] = encoded.emplace(idString, std::vector<std::string>{});
      if (!inserted) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Duplicate ProcessHistory ID: " << idString;
      }
      ids.push_back(idString);
      auto& fields = entry->second;
      fields.reserve(history.size() * 4);
      for (auto const& config : history) {
        std::string psetID;
        config.parameterSetID().toString(psetID);
        auto const& hardware = config.hardwareResourcesDescriptionSerialized();
        processHistoryMetadataDetail::validateHardware(hardware);
        fields.push_back(config.processName());
        fields.push_back(std::move(psetID));
        fields.push_back(config.releaseVersion());
        fields.push_back(hardware);
      }
    }
    metadata.putParameter(processHistoriesVersionKey, 1);
    metadata.putParameter(processHistoriesIDsKey, std::move(ids));
    for (auto& [id, fields] : encoded) {
      metadata.putParameter(std::string(processHistoriesPrefix) + id, std::move(fields));
    }
  }

  inline bool restoreProcessHistories(podio::Frame const& metadata, edm::ProcessHistoryRegistry& registry) {
    auto const& parameters = metadata.getParameters();
    auto hasHistoryKey = [](auto const& map) {
      for (auto const& entry : map) {
        if (entry.first.compare(0, sizeof(processHistoriesPrefix) - 1, processHistoriesPrefix) == 0)
          return true;
      }
      return false;
    };
    if (!hasHistoryKey(parameters.getMap<int>()) && !hasHistoryKey(parameters.getMap<float>()) &&
        !hasHistoryKey(parameters.getMap<double>()) && !hasHistoryKey(parameters.getMap<std::string>())) {
      return false;
    }
    auto version = metadata.getParameter<std::vector<int>>(processHistoriesVersionKey);
    auto ids = metadata.getParameter<std::vector<std::string>>(processHistoriesIDsKey);
    if (!version || *version != std::vector<int>{1} || !ids) {
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Invalid or unsupported Code4hep ProcessHistory metadata";
    }

    // Validate the complete snapshot before changing the supplied registry.
    std::set<std::string> expectedKeys{processHistoriesVersionKey, processHistoriesIDsKey};
    std::set<edm::ProcessHistoryID> uniqueIDs;
    std::vector<edm::ProcessHistory> decoded;
    decoded.reserve(ids->size());
    for (auto const& idString : *ids) {
      if (!processHistoryMetadataDetail::isHexID(idString)) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory ID: " << idString;
      }
      edm::ProcessHistoryID const id(idString);
      if (!id.isValid() || !uniqueIDs.insert(id).second) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid or duplicate ProcessHistory ID: " << idString;
      }
      auto const key = std::string(processHistoriesPrefix) + idString;
      expectedKeys.insert(key);
      auto fields = metadata.getParameter<std::vector<std::string>>(key);
      if (!fields || fields->size() % 4 != 0) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory fields: " << idString;
      }
      decoded.emplace_back();
      auto& history = decoded.back();
      history.reserve(fields->size() / 4);
      for (std::size_t i = 0; i != fields->size(); i += 4) {
        if (!processHistoryMetadataDetail::isHexID((*fields)[i + 1])) {
          throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory ParameterSetID";
        }
        auto const& hardware = (*fields)[i + 3];
        processHistoryMetadataDetail::validateHardware(hardware);
        history.emplace_back((*fields)[i],
                             edm::ParameterSetID((*fields)[i + 1]),
                             (*fields)[i + 2],
                             edm::HardwareResourcesDescription(hardware));
      }
      if (history.id() != id) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "ProcessHistory ID mismatch: " << idString;
      }
    }
    auto validateKeys = [&expectedKeys](auto const& map, bool integers) {
      for (auto const& entry : map) {
        auto const& key = entry.first;
        if (key.compare(0, sizeof(processHistoriesPrefix) - 1, processHistoriesPrefix) != 0)
          continue;
        if (!expectedKeys.count(key) || integers != (key == processHistoriesVersionKey)) {
          throw edm::Exception(edm::errors::FormatIncompatibility) << "Unexpected ProcessHistory metadata key: " << key;
        }
      }
    };
    validateKeys(parameters.getMap<int>(), true);
    validateKeys(parameters.getMap<std::string>(), false);
    if (hasHistoryKey(parameters.getMap<float>()) || hasHistoryKey(parameters.getMap<double>())) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << "Wrong ProcessHistory metadata parameter type";
    }
    for (auto const& history : decoded) {
      registry.registerProcessHistory(history);
    }
    return true;
  }

  inline edm::ProcessHistoryID readHistoryID(podio::Frame const& frame,
                                           std::string const& key,
                                           edm::ProcessHistoryRegistry const& registry) {
    auto ids = frame.getParameter<std::vector<std::string>>(key);
    auto const& parameters = frame.getParameters();
    if (!ids || ids->size() != 1 || !processHistoryMetadataDetail::isHexID(ids->front()) ||
        parameters.getMap<int>().count(key) || parameters.getMap<float>().count(key) ||
        parameters.getMap<double>().count(key)) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory reference: " << key;
    }
    edm::ProcessHistoryID const id(ids->front());
    if (!id.isValid() || !registry.getMapped(id)) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << "Missing referenced ProcessHistory: " << ids->front();
    }
    return id;
  }
}  // namespace c4h

#endif

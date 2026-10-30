#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DataFormats/Provenance/interface/HardwareResourcesDescription.h"
#include "DataFormats/Provenance/interface/ProcessHistoryRegistry.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "podio/Frame.h"

#include "ProcessHistoryMetadata.h"

namespace c4h {
  std::string historyIDString(edm::ProcessHistoryID const& id) {
    std::string result;
    // Framework auxiliaries can use the default ID for an empty history.
    (id.isValid() ? id : edm::ProcessHistory{}.id()).toString(result);
    return result;
  }

  namespace {
    bool isHexID(std::string const& id) {
      return id.size() == 32 && id.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    }

    void validateHardware(std::string const& serialized) {
      try {
        if (edm::HardwareResourcesDescription(serialized).serialize() != serialized) {
          throw edm::Exception(edm::errors::FormatIncompatibility)
              << "Non-round-trippable ProcessHistory hardware metadata";
        }
      } catch (edm::Exception const& ex) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << ex.message();
      }
    }
  }  // namespace

  void saveProcessHistories(podio::Frame& metadata,
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
        validateHardware(hardware);
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

  bool restoreProcessHistories(podio::Frame const& metadata, edm::ProcessHistoryRegistry& registry) {
    auto version = metadata.getParameter<std::vector<int>>(processHistoriesVersionKey);
    if (!version) {
      return false;
    }
    auto ids = metadata.getParameter<std::vector<std::string>>(processHistoriesIDsKey);
    if (*version != std::vector<int>{1} || !ids) {
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Invalid or unsupported Code4hep ProcessHistory metadata";
    }

    // Validate the complete snapshot before changing the supplied registry.
    std::set<edm::ProcessHistoryID> uniqueIDs;
    std::vector<edm::ProcessHistory> decoded;
    decoded.reserve(ids->size());
    for (auto const& idString : *ids) {
      if (!isHexID(idString)) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory ID: " << idString;
      }
      edm::ProcessHistoryID const id(idString);
      // The computed empty-history hash is also CompactHash's default sentinel.
      // The recomputed history ID below validates it just like nonempty histories.
      if (!uniqueIDs.insert(id).second) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid or duplicate ProcessHistory ID: " << idString;
      }
      auto const key = std::string(processHistoriesPrefix) + idString;
      auto fields = metadata.getParameter<std::vector<std::string>>(key);
      if (!fields || fields->size() % 4 != 0) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory fields: " << idString;
      }
      decoded.emplace_back();
      auto& history = decoded.back();
      history.reserve(fields->size() / 4);
      for (std::size_t i = 0; i != fields->size(); i += 4) {
        if (!isHexID((*fields)[i + 1])) {
          throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory ParameterSetID";
        }
        auto const& hardware = (*fields)[i + 3];
        validateHardware(hardware);
        history.emplace_back((*fields)[i],
                             edm::ParameterSetID((*fields)[i + 1]),
                             (*fields)[i + 2],
                             edm::HardwareResourcesDescription(hardware));
      }
      if (history.id() != id) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << "ProcessHistory ID mismatch: " << idString;
      }
    }
    for (auto const& history : decoded) {
      registry.registerProcessHistory(history);
    }
    return true;
  }

  edm::ProcessHistoryID readHistoryID(podio::Frame const& frame,
                                    std::string const& key,
                                    edm::ProcessHistoryRegistry const& registry) {
    auto ids = frame.getParameter<std::vector<std::string>>(key);
    auto const& parameters = frame.getParameters();
    if (!ids || ids->size() != 1 || !isHexID(ids->front()) ||
        parameters.getMap<int>().count(key) || parameters.getMap<float>().count(key) ||
        parameters.getMap<double>().count(key)) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ProcessHistory reference: " << key;
    }
    edm::ProcessHistoryID const id(ids->front());
    if (!registry.getMapped(id)) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << "Missing referenced ProcessHistory: " << ids->front();
    }
    return id;
  }
}  // namespace c4h

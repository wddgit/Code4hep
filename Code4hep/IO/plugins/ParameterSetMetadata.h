#ifndef Code4hep_IO_ParameterSetMetadata_h
#define Code4hep_IO_ParameterSetMetadata_h

#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "podio/Frame.h"

#include <string>
#include <utility>
#include <vector>

namespace c4h {
  inline constexpr char parameterSetVersionKey[] = "Code4hep.edm.ParameterSetRegistry.version";
  inline constexpr char parameterSetIDsKey[] = "Code4hep.edm.ParameterSetRegistry.ids";
  inline constexpr char parameterSetBlobsKey[] = "Code4hep.edm.ParameterSetRegistry.blobs";

  inline podio::Frame parameterSetMetadata() {
    edm::pset::Registry::regmap_type persistentSets;
    edm::pset::Registry::instance()->fillMap(persistentSets);
    std::vector<std::string> ids;
    std::vector<std::string> blobs;
    ids.reserve(persistentSets.size());
    blobs.reserve(persistentSets.size());
    for (auto const& [id, blob] : persistentSets) {
      ids.emplace_back();
      id.toString(ids.back());
      blobs.push_back(blob.pset());
    }
    podio::Frame metadata;
    metadata.putParameter(parameterSetVersionKey, 1);
    metadata.putParameter(parameterSetIDsKey, std::move(ids));
    metadata.putParameter(parameterSetBlobsKey, std::move(blobs));
    return metadata;
  }

  inline void restoreParameterSets(podio::Frame const& metadata) {
    auto const& parameters = metadata.getParameters();
    auto containsKey = [&parameters](std::string const& key) {
      return parameters.getMap<int>().count(key) || parameters.getMap<float>().count(key) ||
             parameters.getMap<double>().count(key) || parameters.getMap<std::string>().count(key);
    };
    // Ordinary podio files may have unrelated metadata and no EDM registry.
    if (!containsKey(parameterSetVersionKey) && !containsKey(parameterSetIDsKey) &&
        !containsKey(parameterSetBlobsKey)) {
      return;
    }

    auto version = metadata.getParameter<std::vector<int>>(parameterSetVersionKey);
    auto ids = metadata.getParameter<std::vector<std::string>>(parameterSetIDsKey);
    auto blobs = metadata.getParameter<std::vector<std::string>>(parameterSetBlobsKey);
    if (!version || *version != std::vector<int>{1} || !ids || !blobs || ids->size() != blobs->size()) {
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Invalid or unsupported Code4hep ParameterSet registry metadata";
    }

    // Decode every entry before changing the global registry. Nested PSets are
    // resolved by ID only after all entries have been inserted.
    std::vector<edm::ParameterSet> decoded;
    decoded.reserve(ids->size());
    try {
      for (std::size_t i = 0; i != ids->size(); ++i) {
        auto const& id = (*ids)[i];
        if (id.size() != 32 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
          throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ParameterSetID in podio metadata: " << id;
        }
        decoded.emplace_back((*blobs)[i]);
        decoded.back().setID(edm::ParameterSetID(id));
        if (!decoded.back().id().isValid()) {
          throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid ParameterSetID in podio metadata: " << id;
        }
      }
    } catch (edm::Exception const& ex) {
      if (ex.categoryCode() == edm::errors::Configuration) {
        throw edm::Exception(edm::errors::FormatIncompatibility) << ex.message();
      }
      throw;
    }
    auto& registry = *edm::pset::Registry::instance();
    for (auto const& pset : decoded) {
      registry.insertMapped(pset);
    }
  }
}  // namespace c4h

#endif

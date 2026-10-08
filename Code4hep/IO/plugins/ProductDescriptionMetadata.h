#ifndef Code4hep_IO_ProductDescriptionMetadata_h
#define Code4hep_IO_ProductDescriptionMetadata_h

#include "TypeNameConversion.h"
#include "Code4hep/PodioUtilities/CollectionWrapperConverterBaseFactory.h"
#include "DataFormats/Provenance/interface/ProductDescription.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/FriendlyName.h"
#include "podio/Frame.h"

#include <charconv>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace c4h {
  inline constexpr char productDescriptionPrefix[] = "Code4hep.edm.ProductDescriptions.";
  inline constexpr char productDescriptionVersionKey[] = "Code4hep.edm.ProductDescriptions.version";
  inline constexpr char productDescriptionIDsKey[] = "Code4hep.edm.ProductDescriptions.ids";

  namespace product_description_metadata {
    [[noreturn]] inline void invalid(std::string const& reason) {
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Invalid Code4hep ProductDescription metadata: " << reason;
    }

    inline unsigned int unsignedValue(std::string const& text, bool allowZero = false) {
      unsigned int value = 0;
      auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
          std::to_string(value) != text || (!allowZero && value == 0)) {
        invalid("noncanonical unsigned ID or value '" + text + "'");
      }
      return value;
    }

    struct Entry {
      // branchType, moduleLabel, processName, instance, fullClassName,
      // friendlyClassName, aliasForBranchID, collectionName.
      std::vector<std::string> fields;
      std::vector<std::string> aliases;
    };
    using Entries = std::map<edm::BranchID, Entry>;

    inline void validate(Entries const& entries) {
      std::set<std::string> collections;
      for (auto const& [id, entry] : entries) {
        auto const& f = entry.fields;
        if (!id.isValid() || f.size() != 8)
          invalid("invalid ID or field count");
        auto const branchType = unsignedValue(f[0], true);
        if (branchType >= edm::NumBranchTypes || f[1].empty() || f[2].empty() || f[4].empty() || f[5].empty())
          invalid("invalid branch type or empty identity field");
        if (f[1].find('_') != std::string::npos || f[2].find('_') != std::string::npos ||
            f[3].find('_') != std::string::npos || f[5].find('_') != std::string::npos ||
            edm::friendlyname::friendlyName(f[4]) != f[5])
          invalid("invalid class name or branch name component");
        if (edm::BranchID(f[5] + "_" + f[1] + "_" + f[3] + "_" + f[2] + ".") != id)
          invalid("BranchID does not match the original product identity");
        auto const target = edm::BranchID(unsignedValue(f[6], true));
        std::set<std::string> aliases(entry.aliases.begin(), entry.aliases.end());
        if (aliases.size() != entry.aliases.size() || aliases.count(""))
          invalid("empty or duplicate branch alias");
        if (!f[7].empty() && (branchType != edm::InEvent || !collections.insert(f[7]).second))
          invalid("stored collections must be unique event products");
        if (target.isValid()) {
          auto const found = entries.find(target);
          if (found == entries.end())
            invalid("missing alias target");
          auto const& t = found->second;
          if (t.fields.size() != 8 || f[0] != t.fields[0] || f[2] != t.fields[2] ||
              f[4] != t.fields[4] || f[5] != t.fields[5])
            invalid("alias target has incompatible type or process");
          for (auto const& alias : t.aliases) {
            if (!aliases.count(alias))
              invalid("alias description omits an inherited branch alias");
          }
        }
      }
      // Check the complete graph before loading any converter plugins.
      std::map<edm::BranchID, unsigned int> state;
      std::function<void(edm::BranchID)> visit = [&](edm::BranchID id) {
        if (state[id] == 1)
          invalid("cyclic alias targets");
        if (state[id] == 2)
          return;
        state[id] = 1;
        auto const target = edm::BranchID(unsignedValue(entries.at(id).fields[6], true));
        if (target.isValid())
          visit(target);
        state[id] = 2;
      };
      for (auto const& [id, entry] : entries)
        visit(id);
    }
  }  // namespace product_description_metadata

  inline std::string const& productCollectionName(
      edm::ProductDescription const& description, std::map<edm::BranchID, std::string> const& collectionNames) {
    if (collectionNames.empty())
      return description.moduleLabel();
    auto const found = collectionNames.find(description.branchID());
    if (found == collectionNames.end() || found->second.empty())
      product_description_metadata::invalid("no collection name for product " +
                                             std::to_string(description.branchID().id()));
    return found->second;
  }

  inline void putProductDescriptions(podio::Frame& metadata,
                                     std::map<edm::BranchID, edm::ProductDescription> const& descriptions,
                                     std::map<edm::BranchID, std::string> const& collectionNames) {
    using namespace product_description_metadata;
    Entries entries;
    for (auto const& [id, description] : descriptions) {
      if (id != description.branchID())
        invalid("description map key differs from BranchID");
      auto const collection = collectionNames.find(id);
      entries.emplace(id, Entry{{std::to_string(description.branchType()),
                                 description.moduleLabel(),
                                 description.processName(),
                                 description.productInstanceName(),
                                 description.fullClassName(),
                                 description.friendlyClassName(),
                                 std::to_string(description.aliasForBranchID().id()),
                                 collection == collectionNames.end() ? "" : collection->second},
                                {description.branchAliases().begin(), description.branchAliases().end()}});
    }
    for (auto const& [id, name] : collectionNames) {
      if (!entries.count(id) || name.empty())
        invalid("stored collection has no description or an empty name");
    }
    validate(entries);
    std::vector<std::string> ids;
    for (auto const& [id, entry] : entries)
      ids.push_back(std::to_string(id.id()));
    metadata.putParameter(productDescriptionVersionKey, 1);
    metadata.putParameter(productDescriptionIDsKey, std::move(ids));
    for (auto const& [id, entry] : entries) {
      auto const key = productDescriptionPrefix + std::to_string(id.id());
      metadata.putParameter(key, entry.fields);
      metadata.putParameter(key + ".aliases", entry.aliases);
    }
  }

  inline std::unique_ptr<edm::ProductRegistry> restoreProductDescriptions(
      podio::Frame const& metadata, std::map<edm::BranchID, std::string>& collectionNames) {
    using namespace product_description_metadata;
    try {
      auto const& parameters = metadata.getParameters();
      std::map<std::string, unsigned int> keys;
      auto collectKeys = [&keys](auto const& values) {
        for (auto const& [key, value] : values) {
          if (key.compare(0, sizeof(productDescriptionPrefix) - 1, productDescriptionPrefix) == 0)
            ++keys[key];
        }
      };
      collectKeys(parameters.getMap<int>());
      collectKeys(parameters.getMap<float>());
      collectKeys(parameters.getMap<double>());
      collectKeys(parameters.getMap<std::string>());
      if (keys.empty())
        return nullptr;
      auto version = metadata.getParameter<std::vector<int>>(productDescriptionVersionKey);
      auto ids = metadata.getParameter<std::vector<std::string>>(productDescriptionIDsKey);
      if (!version || *version != std::vector<int>{1} || !ids)
        invalid("missing schema keys or unsupported version");
      std::set<std::string> expected{productDescriptionVersionKey, productDescriptionIDsKey};
      Entries entries;
      for (auto const& text : *ids) {
        auto const id = edm::BranchID(unsignedValue(text));
        auto const key = productDescriptionPrefix + text;
        auto fields = metadata.getParameter<std::vector<std::string>>(key);
        auto aliases = metadata.getParameter<std::vector<std::string>>(key + ".aliases");
        if (!fields || !aliases || !entries.emplace(id, Entry{*fields, *aliases}).second)
          invalid("missing fields or aliases, or duplicate BranchID");
        expected.insert(key);
        expected.insert(key + ".aliases");
      }
      if (keys.size() != expected.size())
        invalid("unexpected or missing schema keys");
      for (auto const& key : expected) {
        if (keys[key] != 1)
          invalid("missing or multiply typed schema key");
      }
      validate(entries);

      std::map<edm::BranchID, edm::ProductDescription> decoded;
      std::function<edm::ProductDescription const&(edm::BranchID)> reconstruct =
          [&](edm::BranchID id) -> edm::ProductDescription const& {
        auto const found = decoded.find(id);
        if (found != decoded.end())
          return found->second;
        auto const& entry = entries.at(id);
        auto const& f = entry.fields;
        auto const target = edm::BranchID(unsignedValue(f[6], true));
        auto description = [&]() {
          if (target.isValid())
            return edm::ProductDescription(reconstruct(target), f[1], f[3]);
          auto converter = CollectionWrapperConverterBaseFactory::get()->create(typeNameConversion(f[4]));
          if (!converter)
            invalid("no converter for " + f[4]);
          return edm::ProductDescription(static_cast<edm::BranchType>(unsignedValue(f[0], true)),
                                         f[1], f[2], f[3], edm::TypeID(converter->typeID()), false, false,
                                         std::set<std::string>(entry.aliases.begin(), entry.aliases.end()));
        }();
        for (auto const& alias : entry.aliases)
          description.insertBranchAlias(alias);
        description.setProduced(false);
        if (description.branchID() != id || description.aliasForBranchID() != target ||
            description.fullClassName() != f[4] || description.friendlyClassName() != f[5] ||
            description.branchAliases() != std::set<std::string>(entry.aliases.begin(), entry.aliases.end()))
          invalid("reconstructed description changes the original identity or aliases");
        return decoded.emplace(id, std::move(description)).first->second;
      };
      auto registry = std::make_unique<edm::ProductRegistry>();
      std::map<edm::BranchID, std::string> restoredNames;
      for (auto const& [id, entry] : entries) {
        auto description = reconstruct(id);
        if (!entry.fields[7].empty()) {
          restoredNames.emplace(id, entry.fields[7]);
        } else {
          // Keep unselected alias targets for subsequent output files without
          // advertising an input collection that does not exist.
          description.setDropped(true);
        }
        registry->copyProduct(description);
      }
      collectionNames = std::move(restoredNames);
      return registry;
    } catch (cms::Exception const& ex) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << ex.what();
    } catch (std::exception const& ex) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << ex.what();
    }
  }
}  // namespace c4h

#endif

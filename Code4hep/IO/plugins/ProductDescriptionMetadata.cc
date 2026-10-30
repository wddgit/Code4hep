#include <charconv>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Code4hep/PodioUtilities/CollectionWrapperConverterBaseFactory.h"
#include "DataFormats/Provenance/interface/ProductDescription.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/FriendlyName.h"
#include "podio/Frame.h"

#include "ProductDescriptionMetadata.h"
#include "TypeNameConversion.h"

namespace c4h {
  namespace {
    [[noreturn]] void invalid(std::string const& reason) {
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Invalid Code4hep ProductDescription metadata: " << reason;
    }

    unsigned int unsignedValue(std::string const& text, bool allowZero = false) {
      unsigned int value = 0;
      auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
          std::to_string(value) != text || (!allowZero && value == 0)) {
        invalid("noncanonical unsigned ID or value '" + text + "'");
      }
      return value;
    }

    using Entry = StoredProductDescription;
    using Entries = StoredProductDescriptions;

    // Index of each field in StoredProductDescription::fields.
    enum Field : std::size_t {
      kBranchType = 0,
      kModuleLabel,
      kProcessName,
      kInstance,
      kFullClassName,
      kFriendlyClassName,
      kAliasFor,
      kCollectionName,
      kNumFields
    };

    void validate(Entries const& entries) {
      std::set<std::string> collections;
      for (auto const& [id, entry] : entries) {
        auto const& f = entry.fields;
        if (!id.isValid() || f.size() != kNumFields)
          invalid("invalid ID or field count");
        auto const branchType = unsignedValue(f[kBranchType], true);
        if (branchType >= edm::NumBranchTypes || f[kModuleLabel].empty() || f[kProcessName].empty() ||
            f[kFullClassName].empty() || f[kFriendlyClassName].empty())
          invalid("invalid branch type or empty identity field");
        if (f[kModuleLabel].find('_') != std::string::npos || f[kProcessName].find('_') != std::string::npos ||
            f[kInstance].find('_') != std::string::npos || f[kFriendlyClassName].find('_') != std::string::npos ||
            edm::friendlyname::friendlyName(f[kFullClassName]) != f[kFriendlyClassName])
          invalid("invalid class name or branch name component");
        if (edm::BranchID(f[kFriendlyClassName] + "_" + f[kModuleLabel] + "_" + f[kInstance] + "_" +
                          f[kProcessName] + ".") != id)
          invalid("BranchID does not match the original product identity");
        auto const identity = "module '" + f[kModuleLabel] + "', process '" + f[kProcessName] + "', instance '" +
                              f[kInstance] + "', BranchID " + std::to_string(id.id());
        // The storage name is always derived from the original product's labels,
        // so podio collection IDs (and therefore relations and links) never change.
        std::string expectedCollection = f[kModuleLabel] + f[kInstance];
        auto const aliasFor = unsignedValue(f[kAliasFor], true);
        if (aliasFor != 0) {
          auto const target = entries.find(edm::BranchID(aliasFor));
          if (target == entries.end() || target->first == id)
            invalid("EDAlias " + identity + " refers to alias target BranchID " + f[kAliasFor] +
                    ", which has no stored description");
          auto const& t = target->second.fields;
          if (t.size() != kNumFields || t[kAliasFor] != "0" || !t[kCollectionName].empty() ||
              t[kBranchType] != f[kBranchType] || t[kProcessName] != f[kProcessName] ||
              t[kFullClassName] != f[kFullClassName])
            invalid("EDAlias " + identity + " target BranchID " + f[kAliasFor] +
                    " must be a dropped, non-alias description with the same branch type, process and class");
          if (f[kCollectionName].empty())
            invalid("EDAlias " + identity + " must be stored with a collection name");
          expectedCollection = t[kModuleLabel] + t[kInstance];
        }
        if (!f[kCollectionName].empty() && f[kCollectionName] != expectedCollection)
          invalid("stored collection name '" + f[kCollectionName] + "' differs from expected collection name '" +
                  expectedCollection + "' for " + identity +
                  "; Code4hep preserves the original BranchID and podio collection IDs on rewrite; "
                  "use a consistent moduleLabel + productInstanceName collection mapping"
                  " (for an EDAlias, the original product's labels)");
        std::set<std::string> aliases(entry.aliases.begin(), entry.aliases.end());
        if (aliases.size() != entry.aliases.size() || aliases.count(""))
          invalid("empty or duplicate branch alias");
        if (!f[kCollectionName].empty() &&
            (branchType != edm::InEvent || !collections.insert(f[kCollectionName]).second))
          invalid("stored collections must be unique event products");
      }
    }
  }  // namespace

  std::string const& productCollectionName(
      edm::ProductDescription const& description, std::map<edm::BranchID, std::string> const& collectionNames) {
    if (collectionNames.empty())
      return description.moduleLabel();
    auto const found = collectionNames.find(description.branchID());
    if (found == collectionNames.end() || found->second.empty())
      invalid("no collection name for product " + std::to_string(description.branchID().id()));
    return found->second;
  }

  void saveProductDescriptions(podio::Frame& metadata,
                              std::map<edm::BranchID, edm::ProductDescription> const& descriptions,
                              std::map<edm::BranchID, std::string> const& collectionNames) {
    // One entry contains the persistent information sufficient to
    // rebuild a ProductDescription after writing it to an output
    // file and reading it back. It also has a collection name
    // needed for podio. The information is formatted into vectors
    // of strings because that format works with podio parameters.

    // Fill the entries
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

    // Put the entries into the podio Frame
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

  std::optional<StoredProductDescriptions> restoreStoredProductDescriptions(podio::Frame const& metadata) {
    try {
      auto version = metadata.getParameter<std::vector<int>>(productDescriptionVersionKey);
      if (!version)
        return std::nullopt;
      auto const& keyToVectorOfIDs = metadata.getParameters().getMap<std::string>();
      auto const idsEntry = keyToVectorOfIDs.find(productDescriptionIDsKey);
      if (*version != std::vector<int>{1} || idsEntry == keyToVectorOfIDs.end())
        invalid("missing schema keys or unsupported version");
      auto const& ids = idsEntry->second;
      Entries entries;
      for (auto const& text : ids) {
        auto const id = edm::BranchID(unsignedValue(text));
        auto const key = productDescriptionPrefix + text;
        auto fields = metadata.getParameter<std::vector<std::string>>(key);
        auto aliases = metadata.getParameter<std::vector<std::string>>(key + ".aliases");
        if (!fields || !aliases || !entries.emplace(id, Entry{*fields, *aliases}).second)
          invalid("missing fields or aliases, or duplicate BranchID");
      }
      validate(entries);
      return entries;
    } catch (cms::Exception const& ex) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << ex.what();
    } catch (std::exception const& ex) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << ex.what();
    }
  }

  std::unique_ptr<edm::ProductRegistry> restoreProductDescriptions(
      podio::Frame const& metadata, std::map<edm::BranchID, std::string>& collectionNames) {
    try {
      auto stored = restoreStoredProductDescriptions(metadata);
      if (!stored)
        return nullptr;
      auto const& entries = *stored;
      auto registry = std::make_unique<edm::ProductRegistry>();
      std::map<edm::BranchID, std::string> restoredNames;
      for (auto const& [id, entry] : entries) {
        auto const& f = entry.fields;
        auto converter = CollectionWrapperConverterBaseFactory::get()->create(typeNameConversion(f[kFullClassName]));
        if (!converter)
          invalid("no converter for " + f[kFullClassName]);
        std::set<std::string> const branchAliases(entry.aliases.begin(), entry.aliases.end());
        auto makeDescription = [&](std::vector<std::string> const& fields) {
          return edm::ProductDescription(static_cast<edm::BranchType>(unsignedValue(fields[kBranchType], true)),
                                         fields[kModuleLabel],
                                         fields[kProcessName],
                                         fields[kInstance],
                                         edm::TypeID(converter->typeID()),
                                         false,
                                         false,
                                         branchAliases);
        };
        auto const aliasFor = edm::BranchID(unsignedValue(f[kAliasFor], true));
        // ProductDescription has no setter for aliasForBranchID. Rebuild an EDAlias with
        // the alias constructor from its (validated) original; the copy has produced()
        // false, so the framework treats it as an ordinary input product that is
        // accessible only by the alias labels.
        auto description = aliasFor.isValid()
                               ? edm::ProductDescription(makeDescription(entries.at(aliasFor).fields), f[kModuleLabel],
                                                         f[kInstance])
                               : makeDescription(f);
        description.setProduced(false);
        if (description.branchID() != id || description.aliasForBranchID() != aliasFor ||
            description.fullClassName() != f[kFullClassName] ||
            description.friendlyClassName() != f[kFriendlyClassName] || description.branchAliases() != branchAliases)
          invalid("reconstructed description changes the original identity or aliases");
        if (!f[kCollectionName].empty()) {
          restoredNames.emplace(id, f[kCollectionName]);
        } else {
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

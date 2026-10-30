#ifndef Code4hep_IO_ProductDescriptionMetadata_h
#define Code4hep_IO_ProductDescriptionMetadata_h

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "DataFormats/Provenance/interface/ProductDescription.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "podio/Frame.h"

namespace c4h {
  inline constexpr char productDescriptionPrefix[] = "Code4hep.edm.ProductDescriptions.";
  inline constexpr char productDescriptionVersionKey[] = "Code4hep.edm.ProductDescriptions.version";
  inline constexpr char productDescriptionIDsKey[] = "Code4hep.edm.ProductDescriptions.ids";

  struct StoredProductDescription {
    // branchType, moduleLabel, processName, instance, fullClassName,
    // friendlyClassName, aliasForBranchID, collectionName.
    // aliasForBranchID is zero except for an EDAlias. An EDAlias must be stored, and
    // its collection name is its original's moduleLabel + productInstanceName, so
    // podio collection IDs and relations are unchanged. The original must be a
    // dropped (empty collection name), non-alias description of the same branch
    // type, process and class. Input products are then accessible only by the alias.
    std::vector<std::string> fields;
    std::vector<std::string> aliases;
  };
  using StoredProductDescriptions = std::map<edm::BranchID, StoredProductDescription>;

  // Validate and decode persistent identities without instantiating converter plugins.
  // An absent integer version parameter means ordinary podio metadata.
  std::optional<StoredProductDescriptions> restoreStoredProductDescriptions(podio::Frame const& metadata);

  // Return the mapped collection name, or the module label if the map is empty.
  std::string const& productCollectionName(
      edm::ProductDescription const& description, std::map<edm::BranchID, std::string> const& collectionNames);

  // Store product identities, branch aliases, and collection names in the
  // metadata podio::Frame for an output file, as vectors of strings by BranchID.
  void saveProductDescriptions(podio::Frame& metadata,
                              std::map<edm::BranchID, edm::ProductDescription> const& descriptions,
                              std::map<edm::BranchID, std::string> const& collectionNames);

  // Create an input ProductRegistry and fill the collection-name map from
  // the metadata podio::Frame. Return nullptr if this metadata is absent.
  std::unique_ptr<edm::ProductRegistry> restoreProductDescriptions(
      podio::Frame const& metadata, std::map<edm::BranchID, std::string>& collectionNames);
}  // namespace c4h

#endif

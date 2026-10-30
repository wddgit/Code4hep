#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "podio/CollectionBase.h"

#include "Code4hep/PodioUtilities/CollectionWrapperConverterBaseFactory.h"
#include "DataFormats/Common/interface/WrapperBase.h"
#include "DataFormats/Provenance/interface/ProductDescription.h"
#include "DataFormats/Provenance/interface/ProductProvenance.h"

#include "putOnReadForAllProducts.h"
#include "TypeNameConversion.h"
#include "ProductDescriptionMetadata.h"
#include "FWCore/Utilities/interface/EDMException.h"

namespace c4h {

  void validateInputSubsets(podio::Frame const& frame) {
    // Reader/Frame hide raw buffers. Resolve first, then inspect null-safe typed
    // handles and retained subset IDs before any collection is moved to the event.
    auto const names = frame.getAvailableCollections();
    bool hasSubsets = false;
    for (auto const& name : names) {
      auto const* collection = frame.get(name);
      hasSubsets |= collection && collection->isSubsetCollection();
    }
    if (!hasSubsets) {
      return;
    }
    CollectionWrapperConverterBase::CollectionTargets targets;
    CollectionWrapperConverterBase::CollectionNames knownNames;
    auto const ids = frame.getCollectionIDTableForWrite();
    for (size_t index = 0; index < ids.ids().size(); ++index) {
      knownNames.emplace(ids.ids()[index], ids.names()[index]);
    }
    for (auto const& name : names) {
      auto const* collection = frame.get(name);
      if (collection) {
        knownNames.emplace(collection->getID(), name);
        if (name != "EventHeader" && !collection->isSubsetCollection()) {
          targets.emplace(collection->getID(), std::make_pair(name, collection));
        }
      }
    }
    auto factory = CollectionWrapperConverterBaseFactory::get();
    for (auto const& name : names) {
      auto const* collection = frame.get(name);
      if (collection && collection->isSubsetCollection()) {
        try {
          factory->create(std::string(collection->getTypeName()))->validateSubset(*collection, name, targets, knownNames, true);
        } catch (cms::Exception const& ex) {
          throw edm::Exception(edm::errors::FormatIncompatibility) << ex.what();
        }
      }
    }
  }

  void putOnReadForAllProducts(podio::Frame const& podioFrame,
                               edm::ProductRegistry const& productRegistry,
                                edm::EventPrincipal& eventPrincipal,
                                std::map<edm::BranchID, std::string> const& collectionNames) {
    auto factory = c4h::CollectionWrapperConverterBaseFactory::get();

    // It is important that are two iterations, one to get the
    // collections into the Frame and then a second to move them
    // into the Event. The initialization of one-to-many relations
    // fails if these steps are done in the same iteration.
    for (auto const& iter : productRegistry.productList()) {
      auto const& productDescription = iter.second;
      if (productDescription.dropped()) {
        continue;
      }
      podioFrame.get(productCollectionName(productDescription, collectionNames));
    }

    for (auto const& iter : productRegistry.productList()) {
      auto const& productDescription = iter.second;
      if (productDescription.dropped()) {
        continue;
      }
      auto const& collectionName = productCollectionName(productDescription, collectionNames);
      std::optional<edm::ProductProvenance> productProvenance;
      std::unique_ptr<edm::WrapperBase> wrapperBase;

      const podio::CollectionBase* constCollection = podioFrame.get(collectionName);

      // We need to construct a Wrapper<T> object to put in the edm::Event which
      // contains the collection.

      if (constCollection) {
        // Most of the time the pointer is not null and we take this code path

        // We must use a const_cast here. It is not nice, but I have not yet thought
        // of a way to avoid it.
        // Note that if you trace this pointer's origin it comes from an internal
        // pointer in podio that is the result of converting a non-const pointer
        // to a const pointer, so this is not undefined behavior, but it is fragile.
        auto* collection = const_cast<podio::CollectionBase*>(constCollection);

        std::string type_name(collection->getTypeName());
        auto converter = factory->create(type_name);

        wrapperBase = converter->toWrapper(*collection);
      } else {
        // As currently implemented, PodioOutputModule will always write a collection.
        // It will write an empty collection if one is not found in the Event.
        // Therefore, when reading a file written by PodioOutputModule, we should never
        // encounter the case where constCollection is a null pointer.
        // But it is possible using Podio directly to create a file where the pointer
        // is null. In that case, the following code will do the correct thing. A
        // ProductNotFound exception will be thrown if a module tries to get such a collection.
        std::string type_name(productDescription.fullClassName());
        auto converter = factory->create(typeNameConversion(type_name));
        wrapperBase = converter->createEmptyWrapper();
      }

      eventPrincipal.putOnRead(productDescription, std::move(wrapperBase), productProvenance);
    }
  }
}  // namespace c4h

#include "Code4hep/PodioUtilities/CollectionWrapperConverterBase.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "podio/CollectionBufferFactory.h"
#include "podio/CollectionBase.h"
#include <cassert>

namespace c4h {
  namespace {
    void fromVectorToVector(void const* iFrom,
                            void* iTo,
                            CollectionWrapperConverterBase::FromVectorToVector converter) {
      auto f = reinterpret_cast<void* const*>(iFrom);
      converter(*f, iTo);
    }
  }  // namespace

  std::unique_ptr<podio::CollectionBase> CollectionWrapperConverterBase::copy(podio::CollectionBase& iCollection) const {
    iCollection.prepareForWrite();
    auto writeBufs = iCollection.getBuffers();

    auto readBufs = podio::CollectionBufferFactory::instance().createBuffers(
        std::string(iCollection.getTypeName()), iCollection.getSchemaVersion(), iCollection.isSubsetCollection());
    assert(readBufs);

    if (writeBufs.data) {
      fromVectorToVector(writeBufs.data, readBufs->data, copier(iCollection.getDataTypeName()));
    }

    if (writeBufs.references == nullptr) {
      assert(readBufs->references == nullptr);
    } else {
      assert(readBufs->references != nullptr or writeBufs.references->empty());
      assert(readBufs->references == nullptr or readBufs->references->size() == writeBufs.references->size());

      {
        unsigned index = 0;
        for (auto const& r : *(writeBufs.references)) {
          podio::UVecPtr<podio::ObjectID> entry;
          if (r) {
            entry = std::make_unique<std::vector<podio::ObjectID>>();
            entry->reserve(r->size());
            std::copy(r->begin(), r->end(), std::back_inserter(*entry));
          }
          readBufs->references->at(index++) = std::move(entry);
        }
      }
    }
    if (readBufs->vectorMembers != nullptr) {
      assert(writeBufs.vectorMembers != nullptr);

      assert(readBufs->vectorMembers->size() == writeBufs.vectorMembers->size());
      for (size_t index = 0; index < writeBufs.vectorMembers->size(); ++index) {
        assert((*writeBufs.vectorMembers)[index].first == (*(readBufs->vectorMembers))[index].first);
        fromVectorToVector((*writeBufs.vectorMembers)[index].second,
                           (*(readBufs->vectorMembers))[index].second,
                           copier((*writeBufs.vectorMembers)[index].first));
      }
    } else {
      assert(writeBufs.vectorMembers == nullptr or writeBufs.vectorMembers->empty());
    }
    auto newCol = readBufs->createCollection(std::move(*readBufs), iCollection.isSubsetCollection());
    newCol->prepareAfterRead();
    // createCollection already sets subset state. Setting it again erases subset IDs.
    newCol->setID(iCollection.getID());
    return newCol;
  }

  void CollectionWrapperConverterBase::validateSubsetElement(
      std::string const& name, size_t index, bool available, podio::ObjectID id,
      std::string_view type, CollectionTargets const& targets, CollectionNames const& names) {
    auto const target = targets.find(id.collectionID);
    if (available && id.index >= 0 && target != targets.end() &&
        target->second.second->getTypeName() == type &&
        static_cast<size_t>(id.index) < target->second.second->size()) {
      return;
    }
    auto const known = names.find(id.collectionID);
    throw cms::Exception("InvalidSubset")
        << "Subset collection '" << name << "' element index " << index
        << " references target collection ID " << id.collectionID
        << (known == names.end() ? " (name unknown)" : " ('" + known->second + "')")
        << ", target element index " << id.index << ": "
        << (!available ? "element is unavailable. " : "target is not a selected, present owning collection of the correct type with this element. ")
        << "Keep the owning target collection and its payload, or drop the subset collection.";
  }

  void CollectionWrapperConverterBase::throwMissingConverter(std::string_view iCollection,
                                                             std::string_view iContainedType) {
    throw cms::Exception("MissingConverter")
        << "The collection '" << iCollection << "' contains type '" << iContainedType << "'. The type '"
        << iContainedType << "' needs to be registered using the C4H_CONTAINED_CLASS macro.";
  }
}  // namespace c4h

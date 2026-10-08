#ifndef Code4Hep_PodioUtilities_CollectionWrapperConverterBase_h
#define Code4Hep_PodioUtilities_CollectionWrapperConverterBase_h

#include <string_view>
#include <typeinfo>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "podio/CollectionBase.h"

namespace edm {
  class WrapperBase;
}

namespace c4h {
  class CollectionWrapperConverterBase {
  public:
    virtual ~CollectionWrapperConverterBase() = default;

    //The CollectionBase will be moved
    virtual std::unique_ptr<edm::WrapperBase> toWrapper(podio::CollectionBase&) const = 0;
    virtual podio::CollectionBase const* getCollection(edm::WrapperBase const&) const = 0;
    virtual std::string_view typeName() const = 0;
    virtual std::type_info const& typeID() const = 0;

    //Internally the buffer will be requested which is a non-const interface call
    std::unique_ptr<podio::CollectionBase> copy(podio::CollectionBase&) const;

    using CollectionTargets = std::unordered_map<uint32_t, std::pair<std::string, podio::CollectionBase const*>>;
    using CollectionNames = std::unordered_map<uint32_t, std::string>;
    virtual void validateSubset(podio::CollectionBase const&, std::string const&,
                                CollectionTargets const&, CollectionNames const&, bool useStoredIDs = false) const = 0;

    virtual std::unique_ptr<podio::CollectionBase> createEmpty() const = 0;
    virtual std::unique_ptr<edm::WrapperBase> createEmptyWrapper() const = 0;

    using FromVectorToVector = std::function<void(void const*, void*)>;

  protected:
    static void throwMissingConverter(std::string_view iCollection, std::string_view iContainedType);
    static void validateSubsetElement(std::string const&, size_t, bool, podio::ObjectID,
                                      std::string_view, CollectionTargets const&, CollectionNames const&);

  private:
    virtual FromVectorToVector copier(std::string_view) const = 0;
  };
}  // namespace c4h

#endif

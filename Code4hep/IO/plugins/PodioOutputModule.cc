#include "podio/Writer.h"
#include "podio/Frame.h"
#include "podio/CollectionBase.h"
#include "podio/FrameCategories.h"
#include "edm4hep/edm4hep.h"

#include "Code4hep/PodioUtilities/CollectionWrapperConverterBaseFactory.h"
#include "FWCore/Framework/interface/one/OutputModule.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/Framework/interface/EventForOutput.h"
#include "FWCore/Framework/interface/RunForOutput.h"
#include "FWCore/Framework/interface/LuminosityBlockForOutput.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"

#include <memory>
#include <map>
#include <set>
#include <string>

#include "TypeNameConversion.h"
#include "ParameterSetMetadata.h"
#include "ProductDescriptionMetadata.h"
#include "ProcessHistoryMetadata.h"

namespace c4h {

  class PodioOutputModule : public edm::one::OutputModule<> {
  public:
    PodioOutputModule(edm::ParameterSet const&);
    static void fillDescriptions(edm::ConfigurationDescriptions& descriptions);

  private:
    void write(edm::EventForOutput const& e) final;
    void writeLuminosityBlock(edm::LuminosityBlockForOutput const&) final;
    void writeRun(edm::RunForOutput const&) final;
    bool finalSelection(edm::ProductDescription const&) const final;
    void initialRegistry(edm::ProductRegistry const&) final;
    void reallyCloseFile() final;
    bool isFileOpen() const final { return !fileClosed_; }
    podio::Writer writer_;
    bool fileClosed_ = false;
    std::map<edm::BranchID, edm::ProductDescription> descriptions_;
    std::map<edm::BranchID, std::string> collectionNames_;
    std::map<edm::ProcessHistoryID, edm::ProcessHistory> histories_;
  };

  PodioOutputModule::PodioOutputModule(edm::ParameterSet const& iPSet)
      : edm::one::OutputModuleBase(iPSet),
        edm::one::OutputModule<>(iPSet),
        writer_{podio::makeWriter(iPSet.getUntrackedParameter<std::string>("fileName"),
                                  iPSet.getUntrackedParameter<std::string>("type"))} {}

  void PodioOutputModule::fillDescriptions(edm::ConfigurationDescriptions& oDesc) {
    edm::ParameterSetDescription pset;
    edm::one::OutputModule<>::fillDescription(pset);
    pset.addUntracked<std::string>("fileName")->setComment("The name of the file to write.");
    pset.addUntracked<std::string>("type", "default")
        ->setComment(
            "The type of the file to write, e.g. 'root'. The value of 'default' says the type should be based on the "
            "file extension in the name.");
    oDesc.addDefault(pset);
  }

  bool PodioOutputModule::finalSelection(edm::ProductDescription const& product) const {
    std::string const& fullClassName = product.fullClassName();
    if (fullClassName == "edm::EndPathStatus" ||
        fullClassName == "edm::HLTPathStatus" ||
        fullClassName == "edm::TriggerResults") {
      return false;
    }
    try {
      auto const& typeName = typeNameConversion(fullClassName);

      auto factory = c4h::CollectionWrapperConverterBaseFactory::get();
      (void)factory->create(typeName);
    } catch (cms::Exception const&) {
      edm::LogWarning("UnstorableType") << "The type '" << fullClassName
                                        << "' is not registered with the C4H_COLLECTION macro so cannot be stored. It "
                                           "will not be consumed by this module.";
      return false;
    }
    return true;
  }

  void PodioOutputModule::initialRegistry(edm::ProductRegistry const& registry) {
    std::map<std::string, unsigned int> nameCounts;
    for (auto const& product : keptProducts()[edm::InEvent]) {
      ++nameCounts[product.first->moduleLabel() + product.first->productInstanceName()];
    }
    std::set<std::string> usedNames{"EventHeader"};
    for (auto const& product : keptProducts()[edm::InEvent]) {
      auto const& description = *product.first;
      descriptions_.emplace(description.branchID(), description);
      auto name = description.moduleLabel() + description.productInstanceName();
      if (name == "EventHeader" || nameCounts.at(name) != 1) {
        name += "_" + std::to_string(description.branchID().id());
      }
      if (!usedNames.insert(name).second) {
        throw cms::Exception("Configuration") << "Duplicate podio collection name: " << name;
      }
      collectionNames_.emplace(description.branchID(), std::move(name));
    }
    // Alias constructors require the target description, even if that target
    // was not selected for storage. Keep it as metadata only.
    for (auto it = descriptions_.begin(); it != descriptions_.end(); ++it) {
      auto target = it->second.aliasForBranchID();
      while (target.isValid() && descriptions_.count(target) == 0) {
        edm::ProductDescription const* found = nullptr;
        for (auto const& entry : registry.productList()) {
          if (entry.second.branchID() == target) {
            found = &entry.second;
            break;
          }
        }
        if (!found) {
          throw cms::Exception("Configuration") << "Missing alias target description for BranchID " << target.id();
        }
        descriptions_.emplace(target, *found);
        target = found->aliasForBranchID();
      }
    }
  }

  void PodioOutputModule::write(edm::EventForOutput const& e) {
    auto factory = c4h::CollectionWrapperConverterBaseFactory::get();

    podio::Frame frame;
    auto rememberHistory = [this](edm::OccurrenceForOutput const& occurrence) {
      auto history = occurrence.processHistory();
      auto const id = history.setProcessHistoryID();
      histories_.emplace(id, std::move(history));
      return historyIDString(id);
    };
    frame.putParameter(eventProcessHistoryKey, rememberHistory(e));
    frame.putParameter(runProcessHistoryKey, rememberHistory(e.getRun()));
    frame.putParameter(lumiProcessHistoryKey, rememberHistory(e.getLuminosityBlock()));
    frame.putParameter(luminosityBlockKey, std::to_string(e.luminosityBlock()));
    for (auto const& product : keptProducts()[edm::InEvent]) {
      auto const& typeName = typeNameConversion(product.first->fullClassName());

      auto converter = factory->create(typeName);

      edm::TypeID const& tid = product.first->unwrappedTypeID();
      edm::EDGetToken const& token = product.second;
      edm::BasicHandle bh = e.getByToken(token, tid);

      auto const& collectionName = collectionNames_.at(product.first->branchID());
      if (bh.isValid()) {
        assert(bh.wrapper());
        auto collectionBase = converter->getCollection(*bh.wrapper());
        assert(collectionBase);
        auto copiedCollection = converter->copy(*const_cast<podio::CollectionBase*>(collectionBase));
        assert(copiedCollection);
        frame.put(std::move(copiedCollection), collectionName);
      } else {
        //Data product is missing, but we MUST include one
        auto empty = converter->createEmpty();
        frame.put(std::move(empty), collectionName);
      }
    }
    {
      auto ehc = std::make_unique<edm4hep::EventHeaderCollection>();
      auto eh = ehc->create();
      eh.setEventNumber(e.id().event());
      eh.setRunNumber(e.id().run());
      eh.setTimeStamp(e.time().value());
      frame.put(std::move(ehc), "EventHeader");
    }
    writer_.writeEvent(frame);
  }
  void PodioOutputModule::writeLuminosityBlock(edm::LuminosityBlockForOutput const& lumi) {
    auto history = lumi.processHistory();
    auto const id = history.setProcessHistoryID();
    histories_.emplace(id, std::move(history));
  }
  void PodioOutputModule::writeRun(edm::RunForOutput const& run) {
    auto history = run.processHistory();
    auto const id = history.setProcessHistoryID();
    histories_.emplace(id, std::move(history));
  }

  void PodioOutputModule::reallyCloseFile() {
    // Snapshot at close so configurations from later input files are included.
    auto metadata = parameterSetMetadata();
    putProductDescriptions(metadata, descriptions_, collectionNames_);
    putProcessHistories(metadata, histories_);
    writer_.writeFrame(metadata, podio::Category::Metadata);
    writer_.finish();
    fileClosed_ = true;
  }
}  // namespace c4h

using c4h::PodioOutputModule;
DEFINE_FWK_MODULE(PodioOutputModule);

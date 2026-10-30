/**\class c4h::PodioSource

  Description: An InputSource for reading Podio files

  Written to support serial processing of files (1 file open at a time).

*/
//
// Author:      W. David Dagenhart
// Created:     31 October 2025

#include <memory>
#include <string>
#include <vector>

#include "PodioFile.h"

#include "FWCore/Framework/interface/FileBlock.h"
#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/InputSource.h"
#include "FWCore/Framework/interface/InputSourceMacros.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"
#include "FWCore/Utilities/interface/EDMException.h"

namespace c4h {

  using namespace edm;

  class PodioSource : public InputSource {
  public:
    PodioSource(ParameterSet const& pset, InputSourceDescription const& desc);

    static void fillDescriptions(ConfigurationDescriptions& descriptions);

  private:
    ItemTypeInfo getNextItemType() override;
    std::shared_ptr<RunAuxiliary> readRunAuxiliary_() override;
    std::shared_ptr<LuminosityBlockAuxiliary> readLuminosityBlockAuxiliary_() override;
    std::shared_ptr<FileBlock> readFile_() override;
    void closeFile_() override;
    void readRun_(RunPrincipal& runPrincipal) override;
    void readLuminosityBlock_(LuminosityBlockPrincipal& lumiPrincipal) override;
    void readEvent_(EventPrincipal& eventPrincipal) override;

    std::vector<std::string> inputFiles_;
    std::vector<std::string>::const_iterator currentFileIter_;
    std::vector<std::string>::const_iterator lastFileIter_;
    bool firstFile_{true};
    bool firstFileHasProvenanceMetadata_{false};
    bool ignoreMissingOnFirstEvent_;
    RunNumber_t runOffset_;
    EventNumber_t eventOffset_;
    std::unique_ptr<PodioFile> podioFile_;
  };

  PodioSource::PodioSource(ParameterSet const& pset, InputSourceDescription const& desc)
      : InputSource(pset, desc),
        inputFiles_(pset.getUntrackedParameter<std::vector<std::string>>("fileNames")),
        currentFileIter_(inputFiles_.begin()),
        lastFileIter_(inputFiles_.end() - 1),
        ignoreMissingOnFirstEvent_(pset.getUntrackedParameter<bool>("ignoreMissingOnFirstEvent")),
	runOffset_(pset.getUntrackedParameter<unsigned int>("runOffset")),
        eventOffset_(pset.getUntrackedParameter<unsigned int>("eventOffset")) {
    if (processingMode() != RunsLumisAndEvents) {
      throw Exception(errors::Configuration) << "PodioSource constructor: PodioSource does not support\n"
                                             << "processing modes other than RunsLumisAndEvents\n";
    }
    if (currentFileIter_ != inputFiles_.end()) {
      podioFile_ =
	std::make_unique<PodioFile>(*currentFileIter_, processHistoryRegistryForUpdate(), ignoreMissingOnFirstEvent_, runOffset_, eventOffset_);
      firstFileHasProvenanceMetadata_ = podioFile_->hasProvenanceMetadata();
    }

    std::vector<std::string> processOrder;
    processingOrderMerge(processHistoryRegistry(), processOrder);

    productRegistryUpdate().updateFromInput(podioFile_->productRegistry()->productList(), processOrder);
  }

  void PodioSource::fillDescriptions(ConfigurationDescriptions& descriptions) {
    ParameterSetDescription desc;
    desc.addUntracked<std::vector<std::string>>("fileNames")->setComment("Names of files to be processed.");
    desc.addUntracked<bool>("ignoreMissingOnFirstEvent", false)
        ->setComment(
            "By default, PodioSource will throw an exception if a collection that Podio says "
            "is available is missing in the first event of a file. PodioSource needs all collections "
            "in the first event to initialize properly. If this parameter is set to true, the "
            "source will skip registry inference for that null collection. Any frame advertising "
            "a collection without a non-dropped event ProductDescription is rejected with "
            "FormatIncompatibility to prevent frame-owned relation targets. This option does not "
            "ignore unregistered collection converters. "
            "It could also cause a file merge failure if the problem is in the first file of a "
            "file merge process.");
    desc.addUntracked<unsigned int>("eventOffset", 0)->setComment("offset the starting event number");
    desc.addUntracked<unsigned int>("runOffset", 0)->setComment("offset the starting run number");
    InputSource::fillDescription(desc);
    descriptions.add("source", desc);
  }

  InputSource::ItemTypeInfo PodioSource::getNextItemType() {
    if (firstFile_) {
      if (currentFileIter_ == inputFiles_.end()) {
        return ItemTypeInfo(ItemType::IsStop);
      }
      return ItemTypeInfo(ItemType::IsFile);
    }
    ItemType nextItemType = podioFile_->getNextItemType();
    if (nextItemType != ItemType::IsFile) {
      return ItemTypeInfo(nextItemType);
    }
    if (currentFileIter_ == lastFileIter_) {
      return ItemTypeInfo(ItemType::IsStop);
    }
    return ItemTypeInfo(ItemType::IsFile);
  }

  std::shared_ptr<RunAuxiliary> PodioSource::readRunAuxiliary_() { return podioFile_->readRunAuxiliary_(); }

  std::shared_ptr<LuminosityBlockAuxiliary> PodioSource::readLuminosityBlockAuxiliary_() {
    return podioFile_->readLuminosityBlockAuxiliary_();
  }

  std::shared_ptr<FileBlock> PodioSource::readFile_() {
    if (firstFile_) {
      firstFile_ = false;
    } else {
      ++currentFileIter_;
      podioFile_ =
	std::make_unique<PodioFile>(*currentFileIter_, processHistoryRegistryForUpdate(), ignoreMissingOnFirstEvent_, runOffset_, eventOffset_);
      // Ordinary input does not replace the event's provenance reader. Mixing modes
      // could retain a previous Code4hep event's reader and expose stale provenance.
      if (podioFile_->hasProvenanceMetadata() != firstFileHasProvenanceMetadata_) {
        throw Exception(errors::MismatchedInputFiles, "PodioSource::readFile_()")
            << "First input file '" << inputFiles_.front() << "' uses "
            << (firstFileHasProvenanceMetadata_ ? "Code4hep with provenance metadata" : "ordinary podio")
            << ", but current input file '" << *currentFileIter_ << "' uses "
            << (podioFile_->hasProvenanceMetadata() ? "Code4hep with provenance metadata" : "ordinary podio")
            << ". Do not mix Code4hep files with provenance metadata and ordinary podio files in the same job.";
      }
    }

    // make sure the new product registry is compatible with the main one
    std::string const& mergeInfo =
        productRegistryUpdate().merge(*podioFile_->productRegistry(), *currentFileIter_, ProductDescription::Strict);
    if (!mergeInfo.empty()) {
      throw Exception(errors::MismatchedInputFiles, "PodioSource::readFile_()") << mergeInfo;
    }

    return std::make_shared<FileBlock>();
  }

  void PodioSource::closeFile_() { podioFile_.reset(); }

  void PodioSource::readRun_(RunPrincipal& runPrincipal) {
    podioFile_->readRun_(runPrincipal, processHistoryRegistry());
  }

  void PodioSource::readLuminosityBlock_(LuminosityBlockPrincipal& lumiPrincipal) {
    podioFile_->readLuminosityBlock_(lumiPrincipal, processHistoryRegistry());
  }

  void PodioSource::readEvent_(EventPrincipal& eventPrincipal) {
    podioFile_->readEvent_(eventPrincipal, processHistoryRegistry());
  }

}  // namespace c4h
using c4h::PodioSource;
DEFINE_FWK_INPUT_SOURCE(PodioSource);

#ifndef Code4hep_IO_Output2InputTestHelpers_h
#define Code4hep_IO_Output2InputTestHelpers_h

#include <memory>
#include <string>

#include "DataFormats/Provenance/interface/BranchIDListHelper.h"
#include "DataFormats/Provenance/interface/ProcessConfiguration.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/FileBlock.h"
#include "FWCore/Framework/interface/HistoryAppender.h"
#include "FWCore/Framework/interface/InputSource.h"
#include "FWCore/Framework/interface/LuminosityBlockPrincipal.h"
#include "FWCore/Framework/interface/PreallocationConfiguration.h"
#include "FWCore/Framework/interface/RunPrincipal.h"
#include "FWCore/ServiceRegistry/interface/ProcessContext.h"
#include "FWCore/ServiceRegistry/interface/ServiceToken.h"
#include "FWCore/TestProcessor/interface/EventFromSource.h"
#include "FWCore/TestProcessor/interface/LuminosityBlockFromSource.h"
#include "FWCore/TestProcessor/interface/RunFromSource.h"
#include "oneapi/tbb/global_control.h"

namespace c4h::test {
  inline constexpr auto s_tag = "[PodioOutputSource]";
  extern std::string const baseOutConfig;
  extern std::string const baseSourceConfig;

  // Exercise the real source through public APIs without TestSourceProcessor's
  // registry-copy/merge defaults. Input histories define newest-to-oldest order.
  class SourceTester {
  public:
    explicit SourceTester(std::string const& config);
    ~SourceTester();

    edm::InputSource::ItemTypeInfo findNextTransition();
    std::shared_ptr<edm::FileBlock> openFile();
    void closeFile(std::shared_ptr<edm::FileBlock> const& file);
    edm::test::RunFromSource readRun();
    edm::test::LuminosityBlockFromSource readLuminosityBlock();
    edm::test::EventFromSource readEvent();

  private:
    oneapi::tbb::global_control control_{oneapi::tbb::global_control::max_allowed_parallelism, 1};
    edm::ServiceToken token_;
    edm::PreallocationConfiguration allocations_{1, 1, 1, 1};
    std::shared_ptr<edm::ProcessConfiguration const> process_;
    edm::ProcessContext context_;
    edm::HistoryAppender appender_;
    std::shared_ptr<edm::ProductRegistry> registry_;
    std::shared_ptr<edm::BranchIDListHelper> helper_;
    std::shared_ptr<edm::RunPrincipal> run_;
    std::shared_ptr<edm::LuminosityBlockPrincipal> lumi_;
    std::unique_ptr<edm::EventPrincipal> event_;
    std::unique_ptr<edm::InputSource> source_;
  };

  std::string setOutputFile(std::string const& iConfig, std::string const& iFileName);
  std::string setInputFile(std::string const& iConfig, std::string const& iFileName);
  void completeTestProcessorHistory(std::string const& fileName, std::string const& config);
}  // namespace c4h::test

#endif

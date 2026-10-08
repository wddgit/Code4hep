import FWCore.ParameterSet.Config as cms

process = cms.Process("TEST2")

process.load("FWCore.MessageService.MessageLogger_cfi")

process.source = cms.Source("PodioSource",
    fileNames = cms.untracked.vstring(
        "testPodioSource.root"
    )
)

process.maxEvents = cms.untracked.PSet(
    input = cms.untracked.int32(-1)
)

process.testTracksProducer2 = cms.EDProducer("c4h::TestTracksProducer")

from FWCore.TestModules.modules import RunLumiEventAnalyzer
process.test = RunLumiEventAnalyzer(
    verbose = False,
    expectedRunLumiEvents = [
      43, 0, 0,
      43, 1, 0,
      43, 1, 42,
      43, 1, 42,
      43, 1, 42,
      43, 1, 42,
      43, 1, 42,
      43, 1, 42,
      43, 1, 0,
      43, 0, 0
    ],
    expectedEndingIndex = 30
)

process.readTrackCollection = cms.EDAnalyzer("c4h::TestTracksAnalyzer",
    tracks = cms.untracked.InputTag("TrackCollection")
)

process.readTracksFromProducer1 = cms.EDAnalyzer("c4h::TestTracksAnalyzer",
    tracks = cms.untracked.InputTag("testTracksProducer1")
)

process.readTracksFromProducer2 = cms.EDAnalyzer("c4h::TestTracksAnalyzer",
    tracks = cms.untracked.InputTag("testTracksProducer2")
)

process.testEventHeaderAnalyzer = cms.EDAnalyzer("c4h::TestEventHeaderAnalyzer",
    eventHeaders = cms.untracked.InputTag("EventHeader"),
)

# The next part tests relations and links. It exercises
# one-to-one relations, one-to-many relations (including a
# circular case and one that isn't), and links. It includes
# cases where the target of the link was in the input file,
# created in another module in the same process, and created
# by the same module. It also includes cases where the collection
# containing the links was created in the current process or
# read from an input file. There are several producers and
# analyzers to hit the various combinations of those cases.

# The initial input files are created by this script:
#   https://github.com/key4hep/EDM4hep/blob/main/scripts/createEDM4hepFile.py
# Note this script is in the EDM4hep repository, not in the
# Code4hep repository. If the people who manage that repository
# ever change it, then this test may fail and need to be updated.

# Links and relations point into collections in the initial file
# from the EDM4hep script.
process.testRelationsAndLinksProducer41 = cms.EDProducer("c4h::TestRelationsAndLinksProducer",
    tracks = cms.InputTag("TrackCollection"),
    mcParticles = cms.InputTag("MCParticleCollection"),
    testValue = cms.int32(41)
)

# Links and relations point into collections created by
# this process in the same module
process.testRelationsAndLinksProducer51 = cms.EDProducer("c4h::TestRelationsAndLinksProducer",
    tracks = cms.InputTag("NotUsed"),
    mcParticles = cms.InputTag("NotUsed"),
    linkToCollectionsMadeInModule = cms.bool(True),
    testValue = cms.int32(51)
)

# For tracks, links and relations point into collections
# created by this process in a different module
process.testRelationsAndLinksProducer61 = cms.EDProducer("c4h::TestRelationsAndLinksProducer",
    tracks = cms.InputTag("testTracksProducer2"),
    mcParticles = cms.InputTag("MCParticleCollection"),
    testValue = cms.int32(61)
)

# Links and relations point into collections created by
# the previous process
process.testRelationsAndLinksProducer71 = cms.EDProducer("c4h::TestRelationsAndLinksProducer",
    tracks = cms.InputTag("testRelationsAndLinksProducer11", "", "TEST"),
    mcParticles = cms.InputTag("testRelationsAndLinksProducer11", "mcParticles", "TEST"),
    testValue = cms.int32(71)
)

process.testRelationsAndLinks = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("TrackCollection"),
    recDqdxCollection = cms.untracked.InputTag("RecDqdxCollection"),
    reconstructedParticleCollection = cms.untracked.InputTag("ReconstructedParticleCollection"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("TrackMCParticleLinkCollection"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(42),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(42),
    expectedTestValueInMCParticle = cms.untracked.int32(42)
)

process.testRelationsAndLinksRead11 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer11", "", "TEST"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer11", "recDqdxs", "TEST"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer11", "reconstructedParticles", "TEST"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer11", "links", "TEST"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(11),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(42),
    expectedTestValueInMCParticle = cms.untracked.int32(42)
)

process.testRelationsAndLinksRead21 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "", "TEST"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "recDqdxs", "TEST"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "reconstructedParticles", "TEST"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "links", "TEST"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(21),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(21),
    expectedTestValueInMCParticle = cms.untracked.int32(21)
)

process.testRelationsAndLinksRead21NoProcess = process.testRelationsAndLinksRead21.clone(
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "recDqdxs"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "reconstructedParticles"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer21", "links")
)

process.testRelationsAndLinksRead31 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer31", "", "TEST"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer31", "recDqdxs", "TEST"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer31", "reconstructedParticles", "TEST"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer31", "links", "TEST"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(31),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(0),
    expectedTestValueInMCParticle = cms.untracked.int32(42)
)

process.testRelationsAndLinksRead41 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer41"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer41", "recDqdxs"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer41", "reconstructedParticles"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer41", "links"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(41),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(42),
    expectedTestValueInMCParticle = cms.untracked.int32(42)
)

process.testRelationsAndLinksRead51 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer51"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer51", "recDqdxs"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer51", "reconstructedParticles"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer51", "links"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(51),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(51),
    expectedTestValueInMCParticle = cms.untracked.int32(51)
)

process.testRelationsAndLinksRead61 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer61"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer61", "recDqdxs"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer61", "reconstructedParticles"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer61", "links"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(61),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(0),
    expectedTestValueInMCParticle = cms.untracked.int32(42)
)

process.testRelationsAndLinksRead71 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("testRelationsAndLinksProducer71"),
    recDqdxCollection = cms.untracked.InputTag("testRelationsAndLinksProducer71", "recDqdxs"),
    reconstructedParticleCollection = cms.untracked.InputTag("testRelationsAndLinksProducer71", "reconstructedParticles"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("testRelationsAndLinksProducer71", "links"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(71),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(11),
    expectedTestValueInMCParticle = cms.untracked.int32(11)
)

# Products stored through an EDAlias in the previous process are read by
# the alias label. They are written again below, still under the
# original module's collection names.
process.testRelationsAndLinksReadAlias81 = cms.EDAnalyzer("c4h::TestRelationsAndLinksAnalyzer",
    trackCollection = cms.untracked.InputTag("aliasedRelations81", "", "TEST"),
    recDqdxCollection = cms.untracked.InputTag("aliasedRelations81", "recDqdxs", "TEST"),
    reconstructedParticleCollection = cms.untracked.InputTag("aliasedRelations81", "reconstructedParticles", "TEST"),
    trackToMCParticleLinkCollection = cms.untracked.InputTag("aliasedRelations81", "links", "TEST"),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(81),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(81),
    expectedTestValueInMCParticle = cms.untracked.int32(81)
)

# Provenance reached through a Handle, as in CMSSW: the ProductDescription,
# the producer's ParameterSet (edm::parameterSet with the event's
# ProcessHistory), the ProcessHistory, and the recorded parents.
# Products written by the previous process (TEST) were read from
# testPodioSource.root; products of this process (TEST2) were not.
process.testProvenance11 = cms.EDAnalyzer("c4h::TestProvenanceAnalyzer",
    tracks = cms.untracked.InputTag("testRelationsAndLinksProducer11", "", "TEST"),
    expectedModuleLabel = cms.untracked.string("testRelationsAndLinksProducer11"),
    expectedProcessName = cms.untracked.string("TEST"),
    expectedModuleType = cms.untracked.string("c4h::TestRelationsAndLinksProducer"),
    expectedParents = cms.untracked.vstring("TrackCollection::PODIO", "MCParticleCollection::PODIO"),
    expectedProcessHistory = cms.untracked.vstring("PODIO", "TEST", "TEST2")
)

# A product made in the previous process from another product of that process.
process.testProvenance31 = process.testProvenance11.clone(
    tracks = cms.untracked.InputTag("testRelationsAndLinksProducer31", "", "TEST"),
    expectedModuleLabel = cms.untracked.string("testRelationsAndLinksProducer31"),
    expectedParents = cms.untracked.vstring("testTracksProducer1::TEST", "MCParticleCollection::PODIO")
)

# A product of this process whose parents were read from the input file.
process.testProvenance71 = process.testProvenance11.clone(
    tracks = cms.untracked.InputTag("testRelationsAndLinksProducer71"),
    expectedModuleLabel = cms.untracked.string("testRelationsAndLinksProducer71"),
    expectedProcessName = cms.untracked.string("TEST2"),
    expectedParents = cms.untracked.vstring("testRelationsAndLinksProducer11::TEST",
                                            "testRelationsAndLinksProducer11:mcParticles:TEST")
)

# A product stored in the previous process only through an EDAlias. Its
# producer's ParameterSet is found through the original product.
process.testProvenanceAlias81 = process.testProvenance11.clone(
    tracks = cms.untracked.InputTag("aliasedRelations81", "", "TEST"),
    expectedModuleLabel = cms.untracked.string("aliasedRelations81"),
    expectedOriginalModuleLabel = cms.untracked.string("testRelationsAndLinksProducer81"),
    # The original producer reads no input, so it has no recorded parents.
    expectedParents = cms.untracked.vstring()
)

process.p = cms.Path(process.testTracksProducer2 +
                     process.testRelationsAndLinksProducer41 +
                     process.testRelationsAndLinksProducer51 +
                     process.testRelationsAndLinksProducer61 +
                     process.testRelationsAndLinksProducer71
)

process.e = cms.EndPath(process.test +
                        process.readTrackCollection +
                        process.readTracksFromProducer1 +
                        process.readTracksFromProducer2 +
                        process.testEventHeaderAnalyzer +
                        process.testRelationsAndLinks +
                        process.testRelationsAndLinksRead11 +
                        process.testRelationsAndLinksRead21 +
                        process.testRelationsAndLinksRead21NoProcess +
                        process.testRelationsAndLinksRead31 +
                        process.testRelationsAndLinksRead41 +
                        process.testRelationsAndLinksRead51 +
                        process.testRelationsAndLinksRead61 +
                        process.testRelationsAndLinksRead71 +
                        process.testRelationsAndLinksReadAlias81 +
                        process.testProvenance11 +
                        process.testProvenance31 +
                        process.testProvenance71 +
                        process.testProvenanceAlias81
)

process.out = cms.OutputModule("PodioOutputModule",
    fileName = cms.untracked.string('testPodioSource2.root')
)

process.outpath = cms.EndPath(process.out)

#!/bin/bash

# Pass in name and status
function die { echo $1: status $2 ;  exit $2; }

# Fail with message $3 unless text $1 contains the fixed string $2.
function require { echo "$1" | grep -qF -- "$2" || die "$3" 1; }

# c4hProvDump to use. ctest sets C4H_PROVDUMP to the executable built in this
# tree (see CMakeLists.txt); otherwise use the one in PATH.
PROVDUMP=${C4H_PROVDUMP:-c4hProvDump}
function c4hProvDump { "$PROVDUMP" "$@"; }

# Print each stored process history of file $1 on one line, as its process
# names from oldest to newest, separated by single spaces.
function histories {
  c4hProvDump -p "$1" | awk '
    /^History/ { if (n++) print seq; seq = "" }
    /^  Process: / { seq = (seq == "" ? $2 : seq " " $2) }
    END { if (n) print seq }'
}

F1=testPodioSource_cfg.py
F2=testPodioSource2_cfg.py

# create an input file to use as input for the test
createEDM4hepFile.py

# We need a second file to test that multiple files can be read.
# The easiest way to create a second file is to copy the first one.
cp edm4hep.root edm4hep_copy.root

# The following test checks that the expected run, lumi and event
# transitions occur as the Framework processes the file.
# It also checks that run, lumi and event numbers delivered from the
# Run, Lumi and Event objects have the expected values.
# The test depends on the content of the input files. These
# input files are created by the createEDM4hepFile.py script
# called above. createEDM4hepFile.py is a script from the
# external repository named EDM4hep. We have no control over
# that repository. If we update the version of EDM4hep and
# createEDM4hepFile.py has changed, then this test may fail.
# If that happens, then we will need to update the expected
# values in the testPodioSource_cfg.py file.
(cmsRun $F1 ) || die "Failure using $F1" $?
(cmsRun $F2 ) || die "Failure using $F2" $?

# testPodioSource_cfg.py stores the products of testRelationsAndLinksProducer81
# only through the EDAlias aliasedRelations81, and testPodioSource2_cfg.py reads
# them by the alias label and writes them again. Check the stored provenance:
# each output file must describe the alias products, stored under the original
# module's collection names, so podio collection IDs never change.
command -v "$PROVDUMP" > /dev/null \
  || die "c4hProvDump not found (set C4H_PROVDUMP or add it to PATH): $PROVDUMP" 1
for f in testPodioSource.root testPodioSource2.root
do
  # The exit status is not checked; the checks below look for specific output.
  out=$(c4hProvDump -f aliasedRelations81 "$f")
  echo "$out" | grep -q "edm4hep::TrackCollection aliasedRelations81::TEST" \
    || die "$f: EDAlias product description is missing" 1
  echo "$out" | grep -q "collection: testRelationsAndLinksProducer81 " \
    || die "$f: EDAlias product is not stored under the original collection name" 1
  for instance in recDqdxs reconstructedParticles mcParticles links
  do
    echo "$out" | grep -q "collection: testRelationsAndLinksProducer81$instance " \
      || die "$f: EDAlias product '$instance' is not stored under the original collection name" 1
  done
  echo "$out" | grep -q "aliasForBranchID: 0$" \
    && die "$f: an EDAlias product description has no alias target" 1

  # Provenance of the stored products is keyed by the original BranchIDs.
  deps=$(c4hProvDump -d -p -f aliasedRelations81 "$f")
  echo "$deps" | grep -q "Dependencies for event index 0" \
    || die "$f: no event dependencies printed" 1
  echo "$deps" | grep -q "<unknown boundary: no provenance record>" \
    && die "$f: an EDAlias product has no provenance record" 1
done

# Provenance written and read by real cmsRun jobs across processes.
# PODIO is the process name PodioSource gives the products of edm4hep.root,
# which has no Code4hep provenance metadata. TEST (testPodioSource_cfg.py)
# writes testPodioSource.root, and TEST2 (testPodioSource2_cfg.py) reads it
# and writes testPodioSource2.root. These checks do not depend on IDs, so
# they are stable across releases.

# Process histories: each process is appended to the history read from its input.
histories testPodioSource.root | grep -qx "PODIO TEST" \
  || die "testPodioSource.root: no process history PODIO TEST" 1
histories testPodioSource2.root | grep -qx "PODIO TEST TEST2" \
  || die "testPodioSource2.root: no process history PODIO TEST TEST2" 1

# ParameterSets: module configurations of both processes are stored in, and
# restored from, the second file. Only tracked parameters are persisted, so
# check the tracked module type.
for m in "testRelationsAndLinksProducer11 (process TEST)" "testRelationsAndLinksProducer71 (process TEST2)"
do
  label=${m%% *}
  out=$(c4hProvDump -p -f "$label" testPodioSource2.root)
  require "$out" "Module $m = {" "testPodioSource2.root: configuration of module $m is unavailable"
  require "$out" "c4h::TestRelationsAndLinksProducer" \
    "testPodioSource2.root: configuration of module $m has no module type"
done

# Parentage in the first file: testRelationsAndLinksProducer11 consumes the
# PODIO input collections TrackCollection and MCParticleCollection.
out=$(c4hProvDump -d -p -f testRelationsAndLinksProducer11 testPodioSource.root)
for parent in "edm4hep::TrackCollection TrackCollection::PODIO" \
              "edm4hep::MCParticleCollection MCParticleCollection::PODIO"
do
  require "$out" "$parent" "testPodioSource.root: testRelationsAndLinksProducer11 has no parent $parent"
done

# Parentage across two process boundaries in the second file:
# testRelationsAndLinksProducer71 (TEST2) consumes products of
# testRelationsAndLinksProducer11 (TEST), whose own parentage was read from
# testPodioSource.root and written again. The extended ancestors must reach
# the PODIO input collections.
out=$(c4hProvDump -x -p -f testRelationsAndLinksProducer71 testPodioSource2.root)
require "$out" "Ancestors:" "testPodioSource2.root: no ancestors printed for testRelationsAndLinksProducer71"
for ancestor in "edm4hep::TrackCollection testRelationsAndLinksProducer11::TEST ParentageID" \
                "edm4hep::MCParticleCollection testRelationsAndLinksProducer11:mcParticles:TEST ParentageID" \
                "edm4hep::TrackCollection TrackCollection::PODIO" \
                "edm4hep::MCParticleCollection MCParticleCollection::PODIO"
do
  require "$out" "$ancestor" "testPodioSource2.root: testRelationsAndLinksProducer71 has no ancestor $ancestor"
done
exit 0
